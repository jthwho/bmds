/**
 * @file      futex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The futex table: program threads waiting on words (processes.md §3.10).
 */

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <bmds/deadline.h>
#include <bmds/futex.h>
#include <bmds/interrupt.h>

using bmds::Deadline;
using bmds::Error;
using bmds::FutexTable;
using bmds::Interrupt;

namespace {

        void pause(int ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }

        // Waits until @p n wait on @p word (a second at most).
        bool waitersReach(FutexTable &table, const volatile int32_t *word, int n) {
                for (int i = 0; i < 1000 && table.waiters(word) != n; ++i) pause(1);
                return table.waiters(word) == n;
        }

} // namespace

TEST_CASE("Futex: a word holding another value doesn't wait") {
        FutexTable       table;
        volatile int32_t word = 1;
        CHECK(table.wait(&word, 0, Deadline(bmds::WaitForever), nullptr) == Error::WouldBlock);
        CHECK(table.wake(&word, 1) == 0);
}

TEST_CASE("Futex: wait until woken; wakes count and go first come, first served") {
        FutexTable       table;
        volatile int32_t word = 0;
        std::atomic<int> order{0};
        std::atomic<int> first{0}, second{0};
        std::thread      a([&] {
                CHECK(table.wait(&word, 0, Deadline(bmds::WaitForever), nullptr).isOk());
                first = ++order;
        });
        REQUIRE(waitersReach(table, &word, 1));
        std::thread b([&] {
                CHECK(table.wait(&word, 0, Deadline(bmds::WaitForever), nullptr).isOk());
                second = ++order;
        });
        REQUIRE(waitersReach(table, &word, 2));
        CHECK(table.wake(&word, 1) == 1);
        REQUIRE(waitersReach(table, &word, 1));
        a.join();
        CHECK(first == 1);
        CHECK(table.wake(&word, 5) == 1);
        b.join();
        CHECK(second == 2);
        CHECK(table.wake(&word, 1) == 0);
}

TEST_CASE("Futex: requeue wakes some and moves the rest") {
        FutexTable       table;
        volatile int32_t cond = 0, lock = 0;
        std::atomic<int> done{0};
        std::thread      waiters[3];
        for (auto &t : waiters) {
                t = std::thread([&] {
                        CHECK(table.wait(&cond, 0, Deadline(bmds::WaitForever), nullptr).isOk());
                        ++done;
                });
        }
        REQUIRE(waitersReach(table, &cond, 3));
        CHECK(table.requeue(&cond, 1, &lock, 1) == 1);
        REQUIRE(waitersReach(table, &cond, 1));
        CHECK(table.waiters(&lock) == 1);
        CHECK(table.wake(&cond, 1) == 1);
        CHECK(table.wake(&lock, 1) == 1);
        for (auto &t : waiters) t.join();
        CHECK(done == 3);
}

TEST_CASE("Futex: timeouts and interrupts leave the table") {
        FutexTable       table;
        volatile int32_t word = 7;
        CHECK(table.wait(&word, 7, Deadline(20), nullptr) == Error::Timeout);
        CHECK(table.waiters(&word) == 0);

        Interrupt   interrupt;
        Error       got = Error::Ok;
        std::thread t([&] { got = table.wait(&word, 7, Deadline(bmds::WaitForever), &interrupt); });
        REQUIRE(waitersReach(table, &word, 1));
        interrupt.raise();
        t.join();
        CHECK(got == Error::Interrupted);
        CHECK(table.waiters(&word) == 0);
        CHECK(table.wake(&word, 1) == 0);
}

TEST_CASE("Futex: a stopped waiter woken meanwhile is woken once continued") {
        FutexTable        table;
        volatile int32_t  word = 0;
        Interrupt         interrupt;
        std::atomic<bool> returned{false};
        Error             got = Error::Timeout;
        std::thread       t([&] {
                got = table.wait(&word, 0, Deadline(bmds::WaitForever), &interrupt);
                returned = true;
        });
        REQUIRE(waitersReach(table, &word, 1));
        interrupt.hold();
        CHECK(table.wake(&word, 1) == 1);
        pause(30);
        CHECK_FALSE(returned);
        interrupt.resume();
        t.join();
        CHECK(got.isOk());
}
