/**
 * @file      benchkernel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The kernel's own benchmarks (devplan/bench.md §6, §14): a task switch there and back, on one
 * core and across them.
 */

#include <atomic>
#include <new>
#include <bmds/bench.h>
#include <bmds/detail/platform.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/stopwatch.h>
#include <bmds/task.h>

// esp_timer's own read (private in IDF): kernel.clock's comparison.
extern "C" int64_t esp_timer_impl_get_time(void);

BMDS_NAMESPACE_BEGIN

namespace {

        // A task that answers each ping with a pong, until told to stop.
        struct Partner {
                        Semaphore         ping{1, 0};
                        Semaphore         pong{1, 0};
                        std::atomic<bool> stop{false};
                        Task              task;

                        static void entry(void *arg) {
                                auto *p = static_cast<Partner *>(arg);
                                for (;;) {
                                        (void)p->ping.acquire(WaitForever);
                                        const bool stopping = p->stop.load();
                                        p->pong.release();
                                        if (stopping) break;
                                }
                                for (;;) Task::sleep(1000);
                        }
        };

        // Round trips to a partner task on @p core, a level above the shell: µs each (two switches
        // and two semaphores).
        Error roundTrips(BenchContext &ctx, const char *metric, int core) {
                void *mem = Memory::alloc(MemSpace::Internal, sizeof(Partner));
                if (mem == nullptr) return Error::NoMem;
                auto        *p = ::new (mem) Partner();
                Task::Config tc;
                tc.name = "benchpong";
                tc.stackBytes = 2048;
                tc.priority = 3;
                tc.core = core;
                Error err = p->task.start(tc, Partner::entry, p);
                if (err.isOk()) {
                        constexpr int Rounds = 2000;
                        err = ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                                Stopwatch watch;
                                for (int i = 0; i < Rounds; ++i) {
                                        p->ping.release();
                                        (void)p->pong.acquire(WaitForever);
                                }
                                out = double(watch.elapsedUs()) / Rounds;
                                return Error(Error::Ok);
                        });
                        p->stop.store(true);
                        p->ping.release();
                        (void)p->pong.acquire(1000);
                        (void)p->task.stop();
                }
                p->~Partner();
                Memory::free(mem);
                return err;
        }

        Error runSwitch(BenchContext &ctx) {
                const int here = Task::currentCore();
                if (Error err = roundTrips(ctx, "same_core_us", here); err.isError()) return err;
                return roundTrips(ctx, "cross_core_us", here == 0 ? 1 : 0);
        }


        // An uncontended lock and unlock of a kernel Mutex in internal RAM and in PSRAM, and a
        // Semaphore given and taken (internal RAM): ns a pair.
        Error runLocks(BenchContext &ctx) {
                constexpr int Pairs = 20000;
                for (const MemSpace space : {MemSpace::Internal, MemSpace::Psram}) {
                        void *mem = Memory::alloc(space, sizeof(Mutex));
                        if (mem == nullptr) return Error::NoMem;
                        auto       *mutex = ::new (mem) Mutex();
                        const Error err = ctx.measure(space == MemSpace::Internal ? "mutex_internal_pair_ns"
                                                                                  : "mutex_psram_pair_ns",
                                                      "ns", BenchBetter::Lower, [&](double &out) {
                                                              Stopwatch watch;
                                                              for (int i = 0; i < Pairs; ++i) {
                                                                      (void)mutex->lock(WaitForever);
                                                                      mutex->unlock();
                                                              }
                                                              out = double(watch.elapsedUs()) * 1000.0 / Pairs;
                                                              return Error(Error::Ok);
                                                      });
                        mutex->~Mutex();
                        Memory::free(mem);
                        if (err.isError()) return err;
                }
                void *mem = Memory::alloc(MemSpace::Internal, sizeof(Semaphore));
                if (mem == nullptr) return Error::NoMem;
                auto       *semaphore = ::new (mem) Semaphore(1, 0);
                const Error err = ctx.measure("semaphore_pair_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        for (int i = 0; i < Pairs; ++i) {
                                semaphore->release();
                                (void)semaphore->acquire(WaitForever);
                        }
                        out = double(watch.elapsedUs()) * 1000.0 / Pairs;
                        return Error(Error::Ok);
                });
                semaphore->~Semaphore();
                Memory::free(mem);
                return err;
        }


        // The monotonic clock read: BMDS's (the counter, a shift) and esp_timer's own (the ROM's
        // division): ns a read.
        Error runClock(BenchContext &ctx) {
                constexpr int Reads = 20000;
                Error         err = ctx.measure("monotonic_read_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        int64_t   sum = 0;
                        for (int i = 0; i < Reads; ++i) sum += Detail::monotonicUs();
                        out = double(watch.elapsedUs()) * 1000.0 / Reads;
                        return sum != 0 ? Error(Error::Ok) : Error(Error::Invalid);
                });
                if (err.isError()) return err;
                return ctx.measure("esp_timer_read_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        int64_t   sum = 0;
                        for (int i = 0; i < Reads; ++i) sum += esp_timer_impl_get_time();
                        out = double(watch.elapsedUs()) * 1000.0 / Reads;
                        return sum != 0 ? Error(Error::Ok) : Error(Error::Invalid);
                });
        }

} // namespace

BMDS_BENCH(kernelClock, "kernel.clock", runClock, BenchQuick,
           "The monotonic clock read: the firmware's, and esp_timer's own through the ROM (ns a read)")
BMDS_BENCH(kernelLocks, "kernel.locks", runLocks, BenchQuick,
           "A kernel Mutex locked and unlocked, a Semaphore given and taken, uncontended (ns a pair)")
BMDS_BENCH(kernelSwitch, "kernel.switch", runSwitch, BenchQuick,
           "A task woken and waited for, there and back by semaphores: on this core and the other (us)")

BMDS_NAMESPACE_END
