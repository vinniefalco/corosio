//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/timer.hpp>

#include <boost/corosio/io_context.hpp>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

//------------------------------------------------
// Timer-specific tests
// Focus: timer construction and basic interface
//------------------------------------------------

struct timer_test
{
    void
    testConstruction()
    {
        io_context ioc;
        timer t(ioc);

        // Timer constructed successfully
        BOOST_TEST_PASS();
    }

    void
    testMoveConstruct()
    {
        io_context ioc;
        timer t1(ioc);

        // Move construct
        timer t2(std::move(t1));
        BOOST_TEST_PASS();
    }

    void
    testMoveAssign()
    {
        io_context ioc;
        timer t1(ioc);
        timer t2(ioc);

        // Move assign
        t2 = std::move(t1);
        BOOST_TEST_PASS();
    }

    void
    run()
    {
        testConstruction();
        testMoveConstruct();
        testMoveAssign();
    }
};

TEST_SUITE(timer_test, "boost.corosio.timer");

} // namespace corosio
} // namespace boost
