/**
 * @file      intervaltimers.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <bmds/intervaltimers.h>

using bmds::Error;

namespace {

        using Timers = bmds::IntervalTimers<4>;
        constexpr int64_t Ms = 1000000;

        Timers::Setting timer(uint64_t valueNs, uint64_t intervalNs = 0) {
                Timers::Setting s;
                s.valueNs = valueNs;
                s.intervalNs = intervalNs;
                return s;
        }

} // namespace

TEST_CASE("IntervalTimers") {
        Timers  timers;
        int32_t due[4] = {};
        int64_t next = 0;

        SUBCASE("nothing armed") {
                CHECK(timers.armed() == 0);
                CHECK(timers.setting(7, 0).valueNs == 0);
                CHECK(timers.takeDue(1000 * Ms, due, next) == 0);
                CHECK(next == Timers::Never);
        }

        SUBCASE("a one-shot timer fires at its deadline, not before, once") {
                CHECK(timers.set(7, 100 * Ms, timer(50 * Ms)) == Error::Ok);
                CHECK(timers.takeDue(149 * Ms, due, next) == 0);
                CHECK(next == 150 * Ms);
                CHECK(timers.setting(7, 120 * Ms).valueNs == uint64_t(30 * Ms));
                CHECK(timers.takeDue(150 * Ms, due, next) == 1);
                CHECK(due[0] == 7);
                CHECK(next == Timers::Never);
                CHECK(timers.armed() == 0);
                CHECK(timers.takeDue(500 * Ms, due, next) == 0);
        }

        SUBCASE("an armed timer past its deadline still reads armed") {
                CHECK(timers.set(7, 0, timer(10 * Ms)) == Error::Ok);
                CHECK(timers.setting(7, 20 * Ms).valueNs == 1);
        }

        SUBCASE("a repeating timer moves on by whole intervals: a late look fires once") {
                CHECK(timers.set(7, 0, timer(100 * Ms, 100 * Ms)) == Error::Ok);
                CHECK(timers.takeDue(100 * Ms, due, next) == 1);
                CHECK(next == 200 * Ms);
                CHECK(timers.takeDue(450 * Ms, due, next) == 1);
                CHECK(next == 500 * Ms);
                const Timers::Setting s = timers.setting(7, 450 * Ms);
                CHECK(s.valueNs == uint64_t(50 * Ms));
                CHECK(s.intervalNs == uint64_t(100 * Ms));
        }

        SUBCASE("setting again replaces; 0 disarms; drop forgets") {
                CHECK(timers.set(7, 0, timer(100 * Ms)) == Error::Ok);
                CHECK(timers.set(7, 0, timer(300 * Ms)) == Error::Ok);
                CHECK(timers.armed() == 1);
                CHECK(timers.takeDue(100 * Ms, due, next) == 0);
                CHECK(next == 300 * Ms);
                CHECK(timers.set(7, 0, timer(0)) == Error::Ok);
                CHECK(timers.armed() == 0);
                CHECK(timers.set(8, 0, timer(10 * Ms)) == Error::Ok);
                timers.drop(8);
                timers.drop(9); // none: nothing happens
                CHECK(timers.armed() == 0);
                CHECK(timers.takeDue(1000 * Ms, due, next) == 0);
        }

        SUBCASE("several owners: each at its own time, the earliest next") {
                CHECK(timers.set(1, 0, timer(50 * Ms)) == Error::Ok);
                CHECK(timers.set(2, 0, timer(120 * Ms)) == Error::Ok);
                CHECK(timers.set(3, 0, timer(300 * Ms, 300 * Ms)) == Error::Ok);
                CHECK(timers.takeDue(10 * Ms, due, next) == 0);
                CHECK(next == 50 * Ms);
                CHECK(timers.takeDue(120 * Ms, due, next) == 2);
                CHECK(((due[0] == 1 && due[1] == 2) || (due[0] == 2 && due[1] == 1)));
                CHECK(next == 300 * Ms);
                CHECK(timers.takeDue(300 * Ms, due, next) == 1);
                CHECK(due[0] == 3);
                CHECK(next == 600 * Ms);
        }

        SUBCASE("full, and owner 0 refused") {
                for (int32_t pid = 1; pid <= 4; ++pid) CHECK(timers.set(pid, 0, timer(Ms)) == Error::Ok);
                CHECK(timers.set(5, 0, timer(Ms)) == Error::Full);
                CHECK(timers.set(4, 0, timer(2 * Ms)) == Error::Ok); // its own entry
                timers.drop(2);
                CHECK(timers.set(5, 0, timer(Ms)) == Error::Ok);
                CHECK(timers.set(0, 0, timer(Ms)) == Error::Invalid);
        }

        SUBCASE("huge values are cut, nothing overflows") {
                const int64_t now = 1000000 * Ms;
                CHECK(timers.set(7, now, timer(UINT64_MAX, UINT64_MAX)) == Error::Ok);
                const Timers::Setting s = timers.setting(7, now);
                CHECK(s.valueNs == Timers::MaxNs);
                CHECK(s.intervalNs == Timers::MaxNs);
                CHECK(timers.takeDue(now, due, next) == 0);
                CHECK(next == now + int64_t(Timers::MaxNs));
                CHECK(timers.takeDue(next, due, next) == 1);
                CHECK(next > 0);
                CHECK(next == now + 2 * int64_t(Timers::MaxNs));
        }
}
