/**
 * @file      workerpool.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <atomic>
#include <mutex>
#include <vector>
#include <bmds/taskowner.h>
#include <bmds/thread.h>
#include <bmds/workerpool.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::TaskOwner;
using bmds::Thread;
using bmds::WorkerPool;

namespace {

        void bump(void *arg) {
                static_cast<std::atomic<int> *>(arg)->fetch_add(1);
        }

        // A process as the loader keeps one: its threads, and whether it takes more.
        class FakeOwner final : public TaskOwner {
                public:
                        Error attach(Member &member) override {
                                std::lock_guard<std::mutex> lock(_mutex);
                                if (refusing) return Error::InvalidState;
                                members.push_back(&member);
                                ++attached;
                                return Error::Ok;
                        }

                        void detach(Member &member) override {
                                std::lock_guard<std::mutex> lock(_mutex);
                                std::erase(members, &member);
                                ++detached;
                                return;
                        }

                        std::vector<Member *> members;
                        int                   attached = 0;
                        int                   detached = 0;
                        bool                  refusing = false;

                private:
                        std::mutex _mutex;
        };

        struct Seen {
                        TaskOwner *owner = nullptr;
                        Thread     inner; // started by the thread itself
                        TaskOwner *innerOwner = nullptr;
        };

        void noteInner(void *arg) {
                static_cast<Seen *>(arg)->innerOwner = TaskOwner::current();
                return;
        }

        void noteOwner(void *arg) {
                auto *seen = static_cast<Seen *>(arg);
                seen->owner = TaskOwner::current();
                REQUIRE(seen->inner.start(Thread::Config(), noteInner, seen).isOk());
                seen->inner.join();
                return;
        }

} // namespace

TEST_CASE("Thread: in its starter's process (TaskOwner)") {
        // A kernel task starts kernel threads.
        REQUIRE(TaskOwner::current() == nullptr);
        Seen   kernel;
        Thread t;
        REQUIRE(t.start(Thread::Config(), noteOwner, &kernel).isOk());
        t.join();
        CHECK(kernel.owner == nullptr);
        CHECK(kernel.innerOwner == nullptr);

        // A process's task starts threads of that process, and they start more of it; each
        // attaches before it runs and detaches once its entry has returned.
        FakeOwner owner;
        TaskOwner::setCurrent(&owner);
        Seen seen;
        REQUIRE(t.start(Thread::Config(), noteOwner, &seen).isOk());
        t.join();
        CHECK(seen.owner == &owner);
        CHECK(seen.innerOwner == &owner);
        CHECK(owner.attached == 2);
        CHECK(owner.detached == 2);
        CHECK(owner.members.empty());

        // An ending process takes no more threads: the start fails and nothing runs.
        owner.refusing = true;
        std::atomic<int> n{0};
        CHECK(t.start(Thread::Config(), bump, &n) == Error::InvalidState);
        CHECK_FALSE(t.isStarted());
        CHECK(n.load() == 0);
        TaskOwner::setCurrent(nullptr);
}

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
