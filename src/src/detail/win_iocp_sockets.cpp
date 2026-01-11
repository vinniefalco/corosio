//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/win_iocp_sockets.hpp"

#ifdef _WIN32

#include "src/detail/win_iocp_scheduler.hpp"

#include <Ws2tcpip.h>

namespace boost {
namespace corosio {
namespace detail {

namespace {

// Completion key for socket I/O operations
constexpr ULONG_PTR socket_key = 2;

} // namespace

//------------------------------------------------------------------------------
// win_socket_impl

//------------------------------------------------------------------------------
// accept_op

void
accept_op::
operator()()
{
    stop_cb.reset();

    bool success = (error == 0 && !cancelled.load(std::memory_order_acquire));

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = make_error_code(system::errc::operation_canceled);
        else if (error != 0)
            *ec_out = system::error_code(
                static_cast<int>(error), system::system_category());
    }

    // Transfer accepted socket on success
    if (success && transfer_fn && peer_socket && sockets_svc && 
        accepted_socket != INVALID_SOCKET)
    {
        // Update accepted socket context
        ::setsockopt(
            accepted_socket,
            SOL_SOCKET,
            SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<char*>(&listen_socket),
            sizeof(SOCKET));

        // Call the transfer function to set up peer
        transfer_fn(peer_socket, sockets_svc, peer_impl, accepted_socket);
        accepted_socket = INVALID_SOCKET;
        peer_impl = nullptr;
    }
    else
    {
        // Clean up on failure
        if (accepted_socket != INVALID_SOCKET)
        {
            ::closesocket(accepted_socket);
            accepted_socket = INVALID_SOCKET;
        }

        if (peer_impl)
        {
            peer_impl->release();
            peer_impl = nullptr;
        }
    }

    d(h).resume();
}

//------------------------------------------------------------------------------
// win_socket_impl

void
win_socket_impl::
cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        // Cancel all pending I/O on this socket
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(socket_),
            nullptr);
    }

    // Mark operations as cancelled
    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
    acc_.request_cancel();
}

void
win_socket_impl::
close_socket() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

void
win_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

void
win_socket_impl::
connect(
    capy::coro h,
    capy::any_dispatcher d,
    tcp::endpoint endpoint,
    std::stop_token token,
    system::error_code* ec)
{
    auto& op = conn_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.start(token);

    // ConnectEx requires the socket to be bound first
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;

    if (::bind(socket_,
        reinterpret_cast<sockaddr*>(&bind_addr),
        sizeof(bind_addr)) == SOCKET_ERROR)
    {
        op.error = ::WSAGetLastError();
        svc_.post(&op);
        return;
    }

    // Get the ConnectEx function pointer
    auto connect_ex = svc_.connect_ex();
    if (!connect_ex)
    {
        op.error = WSAEOPNOTSUPP;
        svc_.post(&op);
        return;
    }

    // Prepare the target address
    sockaddr_in addr = endpoint.to_sockaddr();

    // Notify scheduler of pending I/O
    svc_.work_started();

    // Start the async connect
    BOOL result = connect_ex(
        socket_,
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr),
        nullptr,  // No send buffer
        0,        // No send buffer size
        nullptr,  // No bytes sent out param
        &op);

    if (!result)
    {
        DWORD err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            // Immediate failure - no IOCP completion will occur
            svc_.work_finished();
            op.error = err;
            svc_.post(&op);
            return;
        }
        // ERROR_IO_PENDING means the operation is in progress
    }
    else
    {
        // Synchronous completion with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
        svc_.work_finished();
        op.error = 0;
        svc_.post(&op);
    }
}

void
win_socket_impl::
read_some(
    capy::coro h,
    capy::any_dispatcher d,
    buffers_param<true>& param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    // Fill WSABUF array from the buffer sequence
    buffers::mutable_buffer bufs[read_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, read_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    op.flags = 0;

    // Notify scheduler of pending I/O
    svc_.work_started();

    // Start the async read
    int result = ::WSARecv(
        socket_,
        op.wsabufs,
        op.wsabuf_count,
        nullptr,      // Bytes received (not used with overlapped)
        &op.flags,
        &op,
        nullptr);     // No completion routine

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            // Immediate failure - no IOCP completion will occur
            svc_.work_finished();
            op.error = err;
            svc_.post(&op);
            return;
        }
        // WSA_IO_PENDING means the operation is in progress
    }
    else
    {
        // Synchronous completion with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
        svc_.work_finished();
        op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
        op.error = 0;
        svc_.post(&op);
    }
}

void
win_socket_impl::
write_some(
    capy::coro h,
    capy::any_dispatcher d,
    buffers_param<false>& param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    // Fill WSABUF array from the buffer sequence
    buffers::const_buffer bufs[write_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, write_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = const_cast<char*>(
            static_cast<char const*>(bufs[i].data()));
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    // Notify scheduler of pending I/O
    svc_.work_started();

    // Start the async write
    int result = ::WSASend(
        socket_,
        op.wsabufs,
        op.wsabuf_count,
        nullptr,      // Bytes sent (not used with overlapped)
        0,            // Flags
        &op,
        nullptr);     // No completion routine

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            // Immediate failure - no IOCP completion will occur
            svc_.work_finished();
            op.error = err;
            svc_.post(&op);
            return;
        }
        // WSA_IO_PENDING means the operation is in progress
    }
    else
    {
        // Synchronous completion with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
        svc_.work_finished();
        op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
        op.error = 0;
        svc_.post(&op);
    }
}

//------------------------------------------------------------------------------
// win_iocp_sockets

win_iocp_sockets::
win_iocp_sockets(
    capy::execution_context& ctx)
    : sched_(ctx.use_service<win_iocp_scheduler>())
    , iocp_(sched_.native_handle())
{
    load_extension_functions();
}

win_iocp_sockets::
~win_iocp_sockets()
{
}

void
win_iocp_sockets::
shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Destroy all socket implementations
    for (auto* impl = list_.pop_front(); impl != nullptr;
         impl = list_.pop_front())
    {
        impl->close_socket();
        delete impl;
    }
}

win_socket_impl&
win_iocp_sockets::
create_impl()
{
    auto* impl = new win_socket_impl(*this);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.push_back(impl);
    }

    return *impl;
}

void
win_iocp_sockets::
destroy_impl(win_socket_impl& impl)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.remove(&impl);
    }

    delete &impl;
}

system::error_code
win_iocp_sockets::
open_socket(win_socket_impl& impl)
{
    // Close existing socket if any
    impl.close_socket();

    // Create an overlapped IPv4 TCP socket
    SOCKET sock = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
    {
        return system::error_code(
            ::WSAGetLastError(),
            system::system_category());
    }

    // Associate the socket with the IOCP
    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        static_cast<HANDLE>(iocp_),
        socket_key,
        0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(sock);
        return system::error_code(
            static_cast<int>(err),
            system::system_category());
    }

    // Disable IOCP notification for synchronous completions
    // This prevents spurious completions when operations complete inline
    ::SetFileCompletionNotificationModes(
        reinterpret_cast<HANDLE>(sock),
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    impl.socket_ = sock;
    return {};
}

void
win_iocp_sockets::
load_extension_functions()
{
    // Create a temporary socket to load extension functions
    SOCKET sock = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return;

    DWORD bytes = 0;

    // Load ConnectEx
    GUID connect_ex_guid = WSAID_CONNECTEX;
    ::WSAIoctl(
        sock,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &connect_ex_guid,
        sizeof(connect_ex_guid),
        &connect_ex_,
        sizeof(connect_ex_),
        &bytes,
        nullptr,
        nullptr);

    // Load AcceptEx
    GUID accept_ex_guid = WSAID_ACCEPTEX;
    ::WSAIoctl(
        sock,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &accept_ex_guid,
        sizeof(accept_ex_guid),
        &accept_ex_,
        sizeof(accept_ex_),
        &bytes,
        nullptr,
        nullptr);

    ::closesocket(sock);
}

void
win_iocp_sockets::
post(overlapped_op* op)
{
    sched_.post(op);
}

void
win_iocp_sockets::
work_started() noexcept
{
    sched_.work_started();
}

void
win_iocp_sockets::
work_finished() noexcept
{
    sched_.work_finished();
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32
