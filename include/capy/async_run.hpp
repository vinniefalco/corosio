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
#include <capy/executor.hpp>
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

template<executor Executor, typename T, typename Handler>
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
        Executor ex_;
        Handler handler_;
        std::exception_ptr ep_;

        template<typename E, typename H, typename... Args>
        promise_type(E&& e, H&& h, Args&&...)
            : ex_(std::forward<E>(e))
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
                return a_.await_suspend(h, p_->ex_);
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

template<executor Executor, typename T, typename Handler>
root_task<Executor, T, Handler>
make_root_task(Executor, Handler handler, task<T> t)
{
    if constexpr (std::is_void_v<T>)
        co_await std::move(t);
    else
        co_return co_await std::move(t);
}

} // namespace detail

/** Starts a task for execution on an executor.

    This function initiates execution of a task by dispatching it to the
    specified executor. If the caller is already running on the executor's
    thread, the task may begin executing immediately (inline). Otherwise,
    the task is queued for later execution.

    The completion handler is invoked when the task finishes. For a
    `task<T>`, the handler must provide overloads for success and error:

    @code
    void operator()(T result);       // Success (non-void)
    void operator()();               // Success (void)
    void operator()(std::exception_ptr ep);  // Error
    @endcode

    The default handler discards successful results and rethrows exceptions.
    The executor and handler are captured by value to ensure they remain
    valid for the duration of the task's execution.

    @param ex The executor on which to run the task.
    @param t The task to execute.
    @param handler Completion handler invoked when the task completes.

    @par Example
    @code
    io_context ioc;
    
    // Fire and forget (default handler)
    async_run(ioc.get_executor(), my_coroutine());
    
    // With completion handler
    async_run(ioc.get_executor(), compute_value(), overload{
        [](int result) { std::cout << "Got: " << result << "\n"; },
        [](std::exception_ptr ep) { }
    });
    
    ioc.run();
    @endcode
*/
template<
    executor Executor,
    typename T,
    typename Handler = default_handler>
void async_run(Executor ex, task<T> t, Handler handler = {})
{
    auto root = detail::make_root_task<Executor, T, Handler>(
        std::move(ex), std::move(handler), std::move(t));
    root.h_.promise().ex_.dispatch(coro{root.h_}).resume();
    root.release();
}

} // namespace capy

#endif
