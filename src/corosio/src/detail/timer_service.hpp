//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SRC_DETAIL_TIMER_SERVICE_HPP
#define BOOST_COROSIO_SRC_DETAIL_TIMER_SERVICE_HPP

#include <boost/corosio/timer.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <chrono>
#include <cstddef>

namespace boost {
namespace corosio {
namespace detail {

class timer_service : public capy::execution_context::service
{
public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;

    // Create timer implementation
    virtual timer::timer_impl* create_impl() = 0;

    // Query methods for scheduler
    virtual bool empty() const noexcept = 0;
    virtual time_point nearest_expiry() const noexcept = 0;

    // Process expired timers - scheduler calls this after wait
    virtual std::size_t process_expired() = 0;

protected:
    timer_service() = default;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
