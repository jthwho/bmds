/**
 * @file      kept.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The keepers' thread (devplan/keptstate.md §2c).
 */

#include <atomic>
#include <bmds/kept.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

namespace Kept {

        namespace {

                struct Entry {
                                Keeper keeper = nullptr;
                                void  *arg = nullptr;
                };

                // Made by the first add() or init(), both during the boot (one task).
                struct State {
                                Mutex     pass; // one pass at a time
                                Semaphore wake{1, 0};
                                Thread    thread;
                };

                constinit Entry                 entries[MaxKeepers] = {};
                constinit std::atomic<size_t>   entryCount{0};
                constinit std::atomic<uint32_t> intervalMs{DefaultIntervalMs};
                constinit StaticObject<State>   state;

                // A clean restart waits this long for a running pass, then goes without.
                constexpr unsigned int FlushWaitMs = 1000;

                State &theState() {
                        if (!state.isCreated()) state.create();
                        return *state;
                }

                void runPass() {
                        const size_t n = entryCount.load(std::memory_order_acquire);
                        for (size_t i = 0; i < n; ++i) entries[i].keeper(entries[i].arg);
                        return;
                }

                void threadMain(void *) {
                        State &s = *state;
                        for (;;) {
                                {
                                        Mutex::Locker locker(s.pass, WaitForever);
                                        runPass();
                                }
                                const uint32_t ms = intervalMs.load(std::memory_order_relaxed);
                                (void)s.wake.acquire(ms == 0 ? WaitForever : ms); // setInterval() wakes it
                        }
                }

        } // namespace

        Error add(Keeper keeper, void *arg) {
                if (keeper == nullptr) return Error::Invalid;
                const size_t n = entryCount.load(std::memory_order_relaxed);
                if (n == MaxKeepers) return Error::Full;
                entries[n] = Entry{keeper, arg};
                entryCount.store(n + 1, std::memory_order_release);
                return Error::Ok;
        }

        Error init() {
                State         &s = theState();
                Thread::Config config;
                config.name = "kept";
                config.stackBytes = 4096;
                config.priority = 1;
                config.stackSpace = MemSpace::Psram;
                config.kernel = true;
                return s.thread.start(config, threadMain, nullptr); // runs for good: never joined
        }

        void setInterval(uint32_t ms) {
                intervalMs.store(ms, std::memory_order_relaxed);
                if (state.isCreated()) state->wake.release();
                return;
        }

        uint32_t interval() {
                return intervalMs.load(std::memory_order_relaxed);
        }

        void flush() {
                if (!state.isCreated()) return;
                Mutex::Locker locker(state->pass, FlushWaitMs);
                if (locker.isLocked()) runPass();
                return;
        }

} // namespace Kept

BMDS_NAMESPACE_END
