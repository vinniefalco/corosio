//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/socket.hpp>
#include "src/detail/win_iocp_sockets.hpp"

#include <boost/corosio/detail/except.hpp>

#include <cassert>

namespace boost {
namespace corosio {

socket::
~socket()
{
    close();
}

socket::
socket(
    capy::execution_context& ctx)
    : ctx_(&ctx)
    , impl_(nullptr)
{
}

void
socket::
open()
{
    if (impl_)
        return; // Already open

    auto& svc = ctx_->use_service<detail::win_iocp_sockets>();
    auto& impl = svc.create_impl();
    impl_ = &impl;

    system::error_code ec = svc.open_socket(impl);
    if (ec)
    {
        impl.release();
        impl_ = nullptr;
        detail::throw_system_error(ec, "socket::open");
    }
}

void
socket::
close()
{
    if (!impl_)
        return; // Already closed

    impl_->release();
    impl_ = nullptr;
}

void
socket::
cancel()
{
    assert(impl_ != nullptr);
    static_cast<detail::win_socket_impl*>(impl_)->cancel();
}

} // namespace corosio
} // namespace boost
