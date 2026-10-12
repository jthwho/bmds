/**
 * @file      realtime.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Program threads' real-time scheduling (devplan/processes.md §3.12): the
 * kernel priorities 1-99 map to, and the 95 % budget.
 */

#include <doctest.h>

#include <bmds/realtimebudget.h>

using bmds::RealTimeBudget;
using Action = RealTimeBudget::Action;

TEST_CASE("RealTimeBudget: 1-99 spread over the band, ends included, in order") {
        CHECK(RealTimeBudget::kernelPriority(1, 7, 18) == 7);
        CHECK(RealTimeBudget::kernelPriority(99, 7, 18) == 18);
        CHECK(RealTimeBudget::kernelPriority(50, 7, 18) == 12);
        uint32_t last = 0;
        uint32_t seen = 0;
        for (uint32_t p = 1; p <= 99; ++p) {
                const uint32_t k = RealTimeBudget::kernelPriority(p, 7, 18);
                CHECK(k >= last);
                if (k != last) ++seen;
                last = k;
        }
        CHECK(seen == 12); // every priority of the band is used
        // Out of range: clamped. A band of one.
        CHECK(RealTimeBudget::kernelPriority(0, 7, 18) == 7);
        CHECK(RealTimeBudget::kernelPriority(200, 7, 18) == 18);
        CHECK(RealTimeBudget::kernelPriority(99, 9, 9) == 9);
}

TEST_CASE("RealTimeBudget: a busy real-time thread is throttled for the period's last 5 %") {
        RealTimeBudget b;
        int64_t        t = 10'000'000;
        // 50 ms checks, every one busy: throttled at 950 ms, released at the next second.
        int checks = 0;
        for (; checks < 30; ++checks) {
                const Action a = b.account(t, checks == 0 ? 0 : 50'000);
                t += 50'000;
                if (a == Action::Throttle) break;
                CHECK(a == Action::None);
        }
        CHECK(checks == 19);
        CHECK(b.isThrottled());
        CHECK(b.account(t, 0) == Action::Release); // t = start + 1 s
        CHECK_FALSE(b.isThrottled());
}

TEST_CASE("RealTimeBudget: a thread within its budget never is") {
        RealTimeBudget b;
        int64_t        t = 0;
        for (int i = 0; i < 200; ++i) {
                CHECK(b.account(t, 45'000) == Action::None); // 90 %
                t += 50'000;
        }
        CHECK_FALSE(b.isThrottled());
}

TEST_CASE("RealTimeBudget: a quiet gap starts a new period; reset() releases") {
        RealTimeBudget b;
        CHECK(b.account(0, 960'000) == Action::Throttle);
        // Seconds later (no check meanwhile), released and spent again at once: still throttled.
        CHECK(b.account(3'500'000, 960'000) == Action::None);
        CHECK(b.isThrottled());
        CHECK(b.account(4'000'000, 0) == Action::Release); // the period from 3 s ended
        b.account(4'000'001, 999'999);
        CHECK(b.isThrottled());
        b.reset();
        CHECK_FALSE(b.isThrottled());
        CHECK(b.account(9'000'000, 10) == Action::None);
}
