//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SOCKET_IMPL_HPP
#define BOOST_COROSIO_DETAIL_SOCKET_IMPL_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/buffers_param.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/capy/affine.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>

namespace boost {
namespace corosio {
namespace detail {

struct socket_impl
{
    virtual ~socket_impl() = default;

    virtual void release() = 0;

    virtual void connect(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        tcp::endpoint,
        std::stop_token,
        system::error_code*) = 0;

    virtual void read_some(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        buffers_param<true>&,
        std::stop_token,
        system::error_code*,
        std::size_t*) = 0;

    virtual void write_some(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        buffers_param<false>&,
        std::stop_token,
        system::error_code*,
        std::size_t*) = 0;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
