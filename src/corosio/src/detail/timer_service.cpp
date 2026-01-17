//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "timer_service.hpp"

#include <boost/capy/core/intrusive_list.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <limits>
#include <mutex>
#include <stop_token>
#include <vector>

namespace boost {
namespace corosio {
namespace detail {

class timer_service_impl;

struct timer_impl
    : timer::timer_impl
    , capy::intrusive_list<timer_impl>::node
{
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;
    using duration = clock_type::duration;

    timer_service_impl* svc_ = nullptr;
    time_point expiry_;
    std::size_t heap_index_ = (std::numeric_limits<std::size_t>::max)();

    // Wait operation state
    std::coroutine_handle<> h_;
    capy::any_dispatcher d_;
    system::error_code* ec_out_ = nullptr;
    std::stop_token token_;
    bool waiting_ = false;

    explicit timer_impl(timer_service_impl& svc) noexcept
        : svc_(&svc)
    {
    }

    void release() override;

    void wait(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        std::stop_token,
        system::error_code*) override;
};

//------------------------------------------------------------------------------

class timer_service_impl : public timer_service
{
public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;

private:
    struct heap_entry
    {
        time_point time_;
        timer_impl* timer_;
    };

    mutable std::mutex mutex_;
    std::vector<heap_entry> heap_;
    capy::intrusive_list<timer_impl> timers_;
    capy::intrusive_list<timer_impl> free_list_;

public:
    explicit timer_service_impl(
        capy::execution_context&)
        : timer_service()
    {
    }

    ~timer_service_impl()
    {
    }

    timer_service_impl(timer_service_impl const&) = delete;
    timer_service_impl& operator=(timer_service_impl const&) = delete;

    void shutdown() override
    {
        while (auto* impl = timers_.pop_front())
            delete impl;
        while (auto* impl = free_list_.pop_front())
            delete impl;
    }

    timer::timer_impl* create_impl() override
    {
        std::lock_guard lock(mutex_);
        timer_impl* impl;
        if (auto* p = free_list_.pop_front())
        {
            impl = p;
            impl->heap_index_ = (std::numeric_limits<std::size_t>::max)();
        }
        else
        {
            impl = new timer_impl(*this);
        }
        timers_.push_back(impl);
        return impl;
    }

    void destroy_impl(timer_impl& impl)
    {
        std::lock_guard lock(mutex_);
        remove_timer_impl(impl);
        timers_.remove(&impl);
        free_list_.push_back(&impl);
    }

    void update_timer(timer_impl& impl, time_point new_time)
    {
        std::lock_guard lock(mutex_);
        if (impl.heap_index_ < heap_.size())
        {
            // Already in heap, update position
            time_point old_time = heap_[impl.heap_index_].time_;
            heap_[impl.heap_index_].time_ = new_time;

            if (new_time < old_time)
                up_heap(impl.heap_index_);
            else
                down_heap(impl.heap_index_);
        }
        else
        {
            // Not in heap, add it
            impl.heap_index_ = heap_.size();
            heap_.push_back({new_time, &impl});
            up_heap(heap_.size() - 1);
        }
    }

    void remove_timer(timer_impl& impl)
    {
        std::lock_guard lock(mutex_);
        remove_timer_impl(impl);
    }

    bool empty() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return heap_.empty();
    }

    time_point nearest_expiry() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return heap_.empty() ? time_point::max() : heap_[0].time_;
    }

    std::size_t process_expired() override
    {
        std::lock_guard lock(mutex_);
        std::size_t count = 0;
        auto now = clock_type::now();

        while (!heap_.empty() && heap_[0].time_ <= now)
        {
            timer_impl* t = heap_[0].timer_;
            remove_timer_impl(*t);

            if (t->waiting_)
            {
                t->waiting_ = false;
                if (t->ec_out_)
                    *t->ec_out_ = {};
                t->d_(t->h_);
                ++count;
            }
        }
        return count;
    }

private:
    void remove_timer_impl(timer_impl& impl)
    {
        std::size_t index = impl.heap_index_;
        if (index >= heap_.size())
            return; // Not in heap

        if (index == heap_.size() - 1)
        {
            // Last element, just pop
            impl.heap_index_ = (std::numeric_limits<std::size_t>::max)();
            heap_.pop_back();
        }
        else
        {
            // Swap with last and reheapify
            swap_heap(index, heap_.size() - 1);
            impl.heap_index_ = (std::numeric_limits<std::size_t>::max)();
            heap_.pop_back();

            if (index > 0 && heap_[index].time_ < heap_[(index - 1) / 2].time_)
                up_heap(index);
            else
                down_heap(index);
        }
    }

    void up_heap(std::size_t index)
    {
        while (index > 0)
        {
            std::size_t parent = (index - 1) / 2;
            if (!(heap_[index].time_ < heap_[parent].time_))
                break;
            swap_heap(index, parent);
            index = parent;
        }
    }

    void down_heap(std::size_t index)
    {
        std::size_t child = index * 2 + 1;
        while (child < heap_.size())
        {
            std::size_t min_child = (child + 1 == heap_.size() ||
                heap_[child].time_ < heap_[child + 1].time_)
                ? child : child + 1;

            if (heap_[index].time_ < heap_[min_child].time_)
                break;

            swap_heap(index, min_child);
            index = min_child;
            child = index * 2 + 1;
        }
    }

    void swap_heap(std::size_t i1, std::size_t i2)
    {
        heap_entry tmp = heap_[i1];
        heap_[i1] = heap_[i2];
        heap_[i2] = tmp;
        heap_[i1].timer_->heap_index_ = i1;
        heap_[i2].timer_->heap_index_ = i2;
    }
};

//------------------------------------------------------------------------------

void
timer_impl::
release()
{
    svc_->destroy_impl(*this);
}

void
timer_impl::
wait(
    std::coroutine_handle<> h,
    capy::any_dispatcher d,
    std::stop_token token,
    system::error_code* ec)
{
    h_ = h;
    d_ = std::move(d);
    token_ = std::move(token);
    ec_out_ = ec;
    waiting_ = true;
}

//------------------------------------------------------------------------------
//
// Extern free functions called from timer.cpp
//
//------------------------------------------------------------------------------

timer::timer_impl*
timer_service_create(capy::execution_context& ctx)
{
    return ctx.use_service<timer_service_impl>().create_impl();
}

void
timer_service_destroy(timer::timer_impl& base) noexcept
{
    static_cast<timer_impl&>(base).release();
}

timer::time_point
timer_service_expiry(timer::timer_impl& base) noexcept
{
    return static_cast<timer_impl&>(base).expiry_;
}

void
timer_service_expires_at(timer::timer_impl& base, timer::time_point t)
{
    auto& impl = static_cast<timer_impl&>(base);
    impl.expiry_ = t;
    impl.svc_->update_timer(impl, t);
}

void
timer_service_expires_after(timer::timer_impl& base, timer::duration d)
{
    auto& impl = static_cast<timer_impl&>(base);
    impl.expiry_ = timer::clock_type::now() + d;
    impl.svc_->update_timer(impl, impl.expiry_);
}

void
timer_service_cancel(timer::timer_impl& base) noexcept
{
    auto& impl = static_cast<timer_impl&>(base);
    impl.svc_->remove_timer(impl);
}

} // namespace detail
} // namespace corosio
} // namespace boost
