/**
 * @file      interrupt.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/semaphore.h>

using bmds::Deadline;
using bmds::Error;
using bmds::Interrupt;
using bmds::Semaphore;

namespace {

        void pause(int ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }

} // namespace

TEST_CASE("Interrupt: flag") {
        Interrupt intr;
        CHECK_FALSE(intr.isRaised());
        intr.raise();
        CHECK(intr.isRaised());
        intr.clear();
        CHECK_FALSE(intr.isRaised());
        intr.raiseFromIsr();
        CHECK(intr.isRaised());
}

TEST_CASE("Interrupt: wait behaves like acquire when not raised") {
        Interrupt intr;
        Semaphore sem(1, 1);
        CHECK(intr.wait(sem, Deadline(100)).isOk());
        CHECK(intr.wait(sem, Deadline(20)) == Error::Timeout);
        CHECK(Interrupt::wait(nullptr, sem, Deadline(20)) == Error::Timeout);
}

TEST_CASE("Interrupt: wait returns at once when already raised") {
        Interrupt intr;
        Semaphore sem(1, 1);
        intr.raise();
        CHECK(intr.wait(sem, Deadline(bmds::WaitForever)) == Error::Interrupted);
        CHECK(sem.tryAcquire().isOk()); // the count wasn't taken
}

TEST_CASE("Interrupt: raise wakes every blocked waiter") {
        Interrupt                intr;
        constexpr int            Waiters = 4;
        Semaphore               *sems[Waiters];
        std::atomic<int>         interrupted{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < Waiters; ++i) {
                sems[i] = new Semaphore(1, 0);
                threads.emplace_back([&, i] {
                        if (intr.wait(*sems[i], Deadline(bmds::WaitForever)) == Error::Interrupted) ++interrupted;
                });
        }
        pause(50);
        CHECK(interrupted.load() == 0);
        intr.raise();
        for (std::thread &t : threads) t.join();
        CHECK(interrupted.load() == Waiters);
        for (Semaphore *s : sems) delete s;
}

TEST_CASE("Interrupt: raises are counted, and a waiter can wait for the next") {
        Interrupt intr;
        Semaphore done(1, 0);
        CHECK(intr.raises() == 0);
        intr.raise();
        intr.raiseFromIsr();
        CHECK(intr.raises() == 2);
        // Past the raises it has seen: blocks until the next one, or a release.
        CHECK(intr.wait(done, Deadline(20), 2) == Error::Timeout);
        CHECK(intr.wait(done, Deadline(20), 1) == Error::Interrupted);
        std::thread raiser([&] {
                pause(20);
                intr.raise();
        });
        CHECK(intr.wait(done, Deadline(bmds::WaitForever), 2) == Error::Interrupted);
        raiser.join();
        CHECK(intr.raises() == 3);
        done.release();
        CHECK(intr.wait(done, Deadline(bmds::WaitForever), 3).isOk());
        intr.clear();
        CHECK(intr.raises() == 0);
        CHECK_FALSE(intr.isRaised());
}

TEST_CASE("Interrupt: sleep") {
        Interrupt  intr;
        const auto start = std::chrono::steady_clock::now();
        CHECK(intr.sleep(30).isOk());
        CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(30));

        std::thread raiser([&] {
                pause(30);
                intr.raise();
        });
        const auto  before = std::chrono::steady_clock::now();
        CHECK(intr.sleep(10000) == Error::Interrupted);
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(5));
        raiser.join();
}

TEST_CASE("Interrupt: many raises racing waiters that come and go") {
        // Exercises the lifetime handshake: each waiter's semaphore is
        // destroyed right after its wait, while raises keep arriving.
        Interrupt                intr;
        std::atomic<bool>        stop{false};
        std::thread              raiser([&] {
                while (!stop.load()) {
                        intr.raise();
                        intr.clear();
                }
        });
        std::vector<std::thread> waiters;
        for (int t = 0; t < 4; ++t) {
                waiters.emplace_back([&] {
                        for (int i = 0; i < 2000; ++i) {
                                Semaphore sem(1, 0);
                                (void)intr.wait(sem, Deadline(1));
                        }
                });
        }
        for (std::thread &t : waiters) t.join();
        stop.store(true);
        raiser.join();
}

TEST_CASE("Deadline") {
        Deadline forever(bmds::WaitForever);
        CHECK(forever.isForever());
        CHECK(forever.remainingUs() == Deadline::Forever);
        CHECK_FALSE(forever.isExpired());

        Deadline soon(20);
        CHECK_FALSE(soon.isForever());
        CHECK(soon.remainingUs() > 0);
        CHECK(soon.remainingUs() <= 20000);
        pause(30);
        CHECK(soon.isExpired());
        CHECK(soon.remainingUs() == 0);
}

TEST_CASE("Interrupt: a Watch wakes a waiter on another interrupt") {
        Interrupt ctrlC, resized;
        Semaphore sem(1, 0);
        {
                Interrupt::Watch watch(resized, sem);
                std::thread      raiser([&] {
                        pause(30);
                        resized.raise();
                });
                // Woken through the watch: the semaphore, not an interrupt.
                CHECK(ctrlC.wait(sem, Deadline(bmds::WaitForever)) == Error::Ok);
                raiser.join();
        }
        // Gone with its scope: raises no longer release the semaphore.
        resized.raise();
        CHECK(sem.acquire(Deadline(20)) == Error::Timeout);
}

TEST_CASE("WaitList: notify releases every waiter; a full list refuses more") {
        bmds::WaitListOf<2> list;
        Semaphore           a(1, 0), b(1, 0), c(1, 0);
        CHECK_FALSE(list.hasWaiters());
        list.notify(); // nobody: nothing happens
        const size_t sa = list.add(a);
        const size_t sb = list.add(b);
        CHECK(sa != bmds::WaitList::NoSlot);
        CHECK(sb != bmds::WaitList::NoSlot);
        CHECK(list.add(c) == bmds::WaitList::NoSlot);
        CHECK(list.hasWaiters());
        list.notify();
        CHECK(a.tryAcquire().isOk());
        CHECK(b.tryAcquire().isOk());
        CHECK(c.tryAcquire().isError());
        list.remove(sa);
        list.notify();
        CHECK(a.tryAcquire().isError());
        CHECK(b.tryAcquire().isOk());
        {
                bmds::WaitList::Entry entry(list, c);
                CHECK(entry.isAdded());
                list.notify();
                CHECK(c.tryAcquire().isOk());
        }
        list.remove(sb);
        CHECK_FALSE(list.hasWaiters());
        list.remove(bmds::WaitList::NoSlot); // nothing
}

TEST_CASE("WaitList: waiters coming and going while notified") {
        bmds::WaitListOf<4>      list;
        std::atomic<bool>        stop{false};
        std::thread              notifier([&] {
                while (!stop.load()) list.notify();
        });
        std::vector<std::thread> waiters;
        for (int t = 0; t < 3; ++t) {
                waiters.emplace_back([&] {
                        for (int i = 0; i < 2000; ++i) {
                                // On the stack: it must never be released once removed.
                                Semaphore             s(1, 0);
                                bmds::WaitList::Entry entry(list, s);
                                REQUIRE(entry.isAdded());
                                const Deadline deadline(1000);
                                CHECK(s.acquire(deadline).isOk());
                        }
                });
        }
        for (std::thread &w : waiters) w.join();
        stop.store(true);
        notifier.join();
        CHECK_FALSE(list.hasWaiters());
}
