/**
 * @file      workerpool.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <atomic>
#include <bmds/thread.h>
#include <bmds/workerpool.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::Thread;
using bmds::WorkerPool;

namespace {

        void bump(void *arg) {
                static_cast<std::atomic<int> *>(arg)->fetch_add(1);
        }

} // namespace

TEST_CASE("Thread: start, join, restart") {
        Thread           t;
        std::atomic<int> n{0};
        CHECK_FALSE(t.isStarted());
        t.join(); // not started: no effect
        REQUIRE(t.start(Thread::Config(), bump, &n).isOk());
        CHECK(t.isStarted());
        CHECK(t.start(Thread::Config(), bump, &n) == Error::InvalidState);
        t.join();
        CHECK_FALSE(t.isStarted());
        CHECK(n.load() == 1);
        REQUIRE(t.start(Thread::Config(), bump, &n).isOk());
        t.join();
        CHECK(n.load() == 2);
}

TEST_CASE("WorkerPool") {
        const size_t base = HostMemory::liveAllocations();
        {
                WorkerPool::Config config;
                config.maxWorkers = 3;
                config.maxIdle = 1;
                WorkerPool       pool(config);
                std::atomic<int> n{0};

                WorkerPool::Worker *w[3];
                for (auto &worker : w) {
                        auto [started, err] = pool.start(bump, &n);
                        REQUIRE(err.isOk());
                        worker = started;
                }
                CHECK(pool.busyCount() == 3);
                CHECK(pool.start(bump, &n).error() == Error::Busy);
                for (auto *worker : w) pool.wait(worker);
                CHECK(n.load() == 3);
                CHECK(pool.busyCount() == 0);
                CHECK(pool.idleCount() == 1); // the rest were retired

                // The idle worker is reused.
                auto [again, err] = pool.start(bump, &n);
                REQUIRE(err.isOk());
                CHECK(again == w[0]);
                pool.wait(again);
                CHECK(n.load() == 4);

                HostMemory::FailGuard fail;
                auto [reused, err2] = pool.start(bump, &n); // needs no allocation
                CHECK(err2.isOk());
                pool.wait(reused);
                auto [extra, err3] = pool.start(bump, &n);
                CHECK(err3.isOk());
                auto [none, err4] = pool.start(bump, &n);
                CHECK(err4 == Error::NoMem);
                pool.wait(extra);
        }
        CHECK(HostMemory::liveAllocations() == base);
}
