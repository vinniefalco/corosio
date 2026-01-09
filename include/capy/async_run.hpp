//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_ASYNC_RUN_HPP
#define CAPY_ASYNC_RUN_HPP

#include <capy/config.hpp>
#include <capy/affine.hpp>
#include <capy/task.hpp>
#include <capy/detail/frame_pool.hpp>

#include <exception>
#include <optional>
#include <utility>

namespace capy {

/** Default completion handler for async_run.

    Discards the result on success, rethrows on exception.
*/
struct default_handler
{
    // Success with result (non-void tasks)
    template<typename T>
    void operator()(T&&) const noexcept
    {
    }

    // Success with no result (void tasks)
    void operator()() const noexcept
    {
    }

    // Error
    void operator()(std::exception_ptr ep) const
    {
        if(ep)
            std::rethrow_exception(ep);
    }
};

namespace detail {

template<dispatcher Dispatcher, typename T, typename Handler>
struct root_task
{
    template<typename U>
    struct result_base
    {
        std::optional<U> result_;

        template<typename V>
        void return_value(V&& value)
        {
            result_ = std::forward<V>(value);
        }
    };

    template<>
    struct result_base<void>
    {
        void return_void()
        {
        }
    };

    struct promise_type
        : capy::detail::frame_pool::promise_allocator
        , result_base<T>
    {
        Dispatcher d_;
        Handler handler_;
        std::exception_ptr ep_;

        template<typename D, typename H, typename... Args>
        promise_type(D&& d, H&& h, Args&&...)
            : d_(std::forward<D>(d))
            , handler_(std::forward<H>(h))
        {
        }

        root_task get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        auto final_suspend() noexcept
        {
            struct awaiter
            {
                promise_type* p_;

                bool await_ready() const noexcept
                {
                    return false;
                }

                coro await_suspend(coro h) const noexcept
                {
                    // Save before destroy
                    auto handler = std::move(p_->handler_);
                    auto ep = p_->ep_;
                    
                    // For non-void, we need to get the result before destroy
                    if constexpr (!std::is_void_v<T>)
                    {
                        auto result = std::move(p_->result_);
                        h.destroy();
                        if(ep)
                            handler(ep);
                        else if(result)
                            handler(std::move(*result));
                    }
                    else
                    {
                        h.destroy();
                        if(ep)
                            handler(ep);
                        else
                            handler();
                    }
                    return std::noop_coroutine();
                }

                void await_resume() const noexcept
                {
                }
            };
            return awaiter{this};
        }

        // return_void() or return_value() inherited from root_task_result

        void unhandled_exception()
        {
            ep_ = std::current_exception();
        }

        template<class Awaitable>
        struct transform_awaiter
        {
            std::decay_t<Awaitable> a_;
            promise_type* p_;

            bool await_ready()
            {
                return a_.await_ready();
            }

            auto await_resume()
            {
                return a_.await_resume();
            }

            template<class Promise>
            auto await_suspend(std::coroutine_handle<Promise> h)
            {
                return a_.await_suspend(h, p_->d_);
            }
        };

        template<class Awaitable>
        auto await_transform(Awaitable&& a)
        {
            return transform_awaiter<Awaitable>{std::forward<Awaitable>(a), this};
        }
    };

    std::coroutine_handle<promise_type> h_;

    void release()
    {
        h_ = nullptr;
    }

    ~root_task()
    {
        if(h_)
            h_.destroy();
    }
};

template<dispatcher Dispatcher, typename T, typename Handler>
root_task<Dispatcher, T, Handler>
make_root_task(Dispatcher, Handler handler, task<T> t)
{
    if constexpr (std::is_void_v<T>)
        co_await std::move(t);
    else
        co_return co_await std::move(t);
}

} // namespace detail

/** Launches a lazy task for detached execution.

    Initiates a task by invoking the dispatcher to schedule the coroutine.
    This is analogous to Asio's `co_spawn`. The task begins executing when
    the dispatcher schedules it; if the dispatcher permits inline execution,
    the task runs immediately until it awaits an I/O operation.

    The dispatcher controls where and how the task resumes after each
    suspension point. Tasks deal only with type-erased dispatchers
    (`coro(coro)` signature), not typed executors. This leverages the
    coroutine handle's natural type erasure.

    @par Dispatcher Behavior
    The dispatcher is invoked to start the task and propagated through
    the coroutine chain via the affine awaitable protocol. When the task
    completes, the handler runs on the same dispatcher context. If inline
    execution is permitted, the call chain proceeds synchronously until
    an I/O await suspends execution.

    @par Handler Requirements
    The handler must provide overloads for success and error:
    @code
    void operator()(T result);            // Success (non-void task)
    void operator()();                    // Success (void task)
    void operator()(std::exception_ptr);  // Error
    @endcode

    @note The dispatcher and handler are captured by value. Use
    `default_handler` to discard results and rethrow exceptions.

    @par Example
    @code
    io_context ioc;

    // Fire and forget
    async_run(ioc.get_executor(), my_coroutine());

    // With completion handler
    async_run(ioc.get_executor(), compute_value(), overload{
        [](int result) { std::cout << "Got: " << result << "\n"; },
        [](std::exception_ptr) { }
    });

    // Donate thread to run queued work
    ioc.run();
    @endcode

    @param d The dispatcher that schedules and resumes the task.
    @param t The lazy task to execute.
    @param handler Completion handler invoked when the task finishes.

    @see task
    @see dispatcher
*/
template<
    dispatcher Dispatcher,
    typename T,
    typename Handler = default_handler>
void async_run(Dispatcher d, task<T> t, Handler handler = {})
{
    auto root = detail::make_root_task<Dispatcher, T, Handler>(
        std::move(d), std::move(handler), std::move(t));
    root.h_.promise().d_(coro{root.h_}).resume();
    root.release();
}

} // namespace capy

#endif
