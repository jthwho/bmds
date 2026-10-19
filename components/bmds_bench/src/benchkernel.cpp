/**
 * @file      benchkernel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The kernel's own benchmarks (devplan/bench.md §6, §14-16, §20): a task switch there and back,
 * on one core and across them; locks, critical sections and the clock; semaphores and a mutex
 * under contention, checked.
 */

#include <atomic>
#include <new>
#include <freertos/FreeRTOS.h>
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
                // FreeRTOS's critical section, which each of the above enters (a mutex's pair four
                // times, a semaphore's twice): critical.cpp's (devplan/bench.md §16).
                static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
                if (Error err = ctx.measure("critical_section_ns", "ns", BenchBetter::Lower,
                                            [&](double &out) {
                                                    Stopwatch watch;
                                                    for (int i = 0; i < Pairs; ++i) {
                                                            portENTER_CRITICAL(&mux);
                                                            portEXIT_CRITICAL(&mux);
                                                    }
                                                    out = double(watch.elapsedUs()) * 1000.0 / Pairs;
                                                    return Error(Error::Ok);
                                            });
                    err.isError())
                        return err;
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

namespace {

        // Semaphores and mutexes under contention, checked (devplan/bench.md §20-21): workers on both
        // cores take a semaphore with a short timeout, hold it briefly and give it back, and lock a
        // mutex around a shared counter. Fails if more ever hold the semaphore than it has counts,
        // if the counter lost an increment, or if the counts aren't all back at the end. A second a
        // sample: operations a second. The workers run at 1, below the shell, which must run to stop
        // them: a first version ran some above it, and those that never blocked starved it (§20).
        struct SyncWorld {
                        SyncWorld(int32_t c, uint32_t h) : sem(unsigned(c), unsigned(c)), counts(c), hold(h) {}
                        Semaphore             sem;
                        const int32_t         counts; // the semaphore's
                        const uint32_t        hold;   // spins at most while holding a count
                        Mutex                 mutex;
                        std::atomic<int32_t>  inside{0};
                        std::atomic<int32_t>  most{0};
                        std::atomic<bool>     stop{false};
                        std::atomic<uint32_t> ops{0}, timeouts{0};
                        uint32_t              counter = 0; // under mutex
                        uint32_t              locked = 0;  // increments made, all workers
        };

        struct SyncWorker {
                        SyncWorld *world = nullptr;
                        Task       task;
                        Semaphore  done{1, 0};
                        uint32_t   increments = 0;
                        uint32_t   seed = 1;

                        static void entry(void *arg) {
                                auto      *w = static_cast<SyncWorker *>(arg);
                                SyncWorld &world = *w->world;
                                while (!world.stop.load(std::memory_order_relaxed)) {
                                        w->seed = w->seed * 1103515245u + 12345u;
                                        if (world.sem.acquire(1 + (w->seed >> 16) % 2).isOk()) {
                                                const int32_t now = world.inside.fetch_add(1) + 1;
                                                int32_t       most = world.most.load();
                                                while (now > most && !world.most.compare_exchange_weak(most, now)) {}
                                                for (uint32_t spin = (w->seed >> 8) % world.hold; spin > 0; --spin)
                                                        __asm__ volatile("");
                                                world.inside.fetch_sub(1);
                                                world.sem.release();
                                        } else {
                                                world.timeouts.fetch_add(1, std::memory_order_relaxed);
                                        }
                                        {
                                                Mutex::Locker locker(world.mutex, WaitForever);
                                                ++world.counter;
                                        }
                                        ++w->increments;
                                        world.ops.fetch_add(1, std::memory_order_relaxed);
                                        if ((w->seed >> 12) % 64 == 0) taskYIELD();
                                }
                                w->done.release();
                                for (;;) Task::sleep(1000);
                        }
        };

        Error syncPhase(BenchContext &ctx, const char *metric, int32_t counts, uint32_t hold) {
                constexpr int Workers = 6;
                void         *mem = Memory::alloc(MemSpace::Internal, sizeof(SyncWorld) + Workers * sizeof(SyncWorker));
                if (mem == nullptr) return Error::NoMem;
                auto *world = ::new (mem) SyncWorld(counts, hold);
                auto *workers = reinterpret_cast<SyncWorker *>(static_cast<uint8_t *>(mem) + sizeof(SyncWorld));
                Error err = ctx.measure(metric, "/s", BenchBetter::Higher, [&](double &out) {
                        world->stop.store(false);
                        world->ops.store(0);
                        const uint32_t before = world->counter;
                        int            started = 0;
                        Error          e;
                        for (int i = 0; i < Workers && e.isOk(); ++i, ++started) {
                                SyncWorker *w = ::new (&workers[i]) SyncWorker();
                                w->world = world;
                                w->seed = uint32_t(i * 7919 + 1);
                                Task::Config tc;
                                tc.name = "benchsync";
                                tc.stackBytes = 2048;
                                tc.priority = 1;
                                tc.core = i % 2;
                                e = w->task.start(tc, SyncWorker::entry, w);
                        }
                        Stopwatch watch;
                        Task::sleep(1000);
                        world->stop.store(true);
                        uint32_t increments = 0;
                        for (int i = 0; i < started; ++i) {
                                (void)workers[i].done.acquire(2000);
                                increments += workers[i].increments;
                                (void)workers[i].task.stop();
                                workers[i].~SyncWorker();
                        }
                        out = double(world->ops.load()) * 1e6 / double(watch.elapsedUs());
                        if (e.isError()) return e;
                        if (world->most.load() > counts || world->counter - before != increments) {
                                ctx.note("broken: %d held the semaphore at once; %u increments, the counter %u",
                                         int(world->most.load()), unsigned(increments),
                                         unsigned(world->counter - before));
                                return Error(Error::IoError);
                        }
                        int back = 0;
                        while (world->sem.tryAcquire().isOk()) ++back;
                        for (int i = 0; i < back; ++i) world->sem.release();
                        if (back != counts) {
                                ctx.note("broken: %d counts back of %d", back, int(counts));
                                return Error(Error::IoError);
                        }
                        return Error(Error::Ok);
                });
                if (err.isOk()) ctx.note("%s: %u timeouts", metric, unsigned(world->timeouts.load()));
                world->~SyncWorld();
                Memory::free(mem);
                return err;
        }

        // Priority inheritance through Mutex (devplan/bench.md §21), checked by the priorities
        // seen: a holder at 2 raised by a waiter at 6 on one mutex and one at 8 on another stays at 8
        // when it unlocks the first and goes back to 2 at the second; a waiter at 7 that times out
        // leaves it raised until it unlocks. All on one core, so the raise is what lets it run.
        struct Inherit {
                        Mutex     a, b;
                        Semaphore ready{1, 0}, go{1, 0}, done{4, 0};
                        int       script = 0; // the holder's: 0 two mutexes, 1 one
                        Task      holders[2], waiters[3];
                        struct Waiter {
                                        Inherit     *self;
                                        Mutex       *mutex;
                                        unsigned int timeoutMs;
                                        Error        got;
                        } w[3];

                        static void holderEntry(void *arg) {
                                auto *t = static_cast<Inherit *>(arg);
                                if (t->script == 0) {
                                        (void)t->a.lock(WaitForever);
                                        (void)t->b.lock(WaitForever);
                                        t->ready.release();
                                        (void)t->go.acquire(WaitForever);
                                        t->a.unlock();
                                        t->ready.release();
                                        (void)t->go.acquire(WaitForever);
                                        t->b.unlock();
                                } else {
                                        (void)t->a.lock(WaitForever);
                                        t->ready.release();
                                        (void)t->go.acquire(WaitForever);
                                        t->a.unlock();
                                }
                                t->ready.release();
                                for (;;) Task::sleep(1000);
                        }

                        static void waiterEntry(void *arg) {
                                auto *w = static_cast<Waiter *>(arg);
                                w->got = w->mutex->lock(w->timeoutMs);
                                if (w->got.isOk()) w->mutex->unlock();
                                w->self->done.release();
                                for (;;) Task::sleep(1000);
                        }
        };

        Error checkInheritance(BenchContext &ctx) {
                void *mem = Memory::alloc(MemSpace::Internal, sizeof(Inherit));
                if (mem == nullptr) return Error::NoMem;
                auto       *t = ::new (mem) Inherit();
                const char *broken = nullptr;
                unsigned    seen = 0;
                auto        start = [](Task &task, unsigned prio, void (*entry)(void *), void *arg) {
                        Task::Config tc;
                        tc.name = "benchpi";
                        tc.stackBytes = 2048;
                        tc.priority = prio;
                        tc.core = 1;
                        return task.start(tc, entry, arg);
                };
                Task *holder = &t->holders[0];
                auto  prio = [&] {
                        return seen = unsigned(uxTaskPriorityGet(holder->handle()));
                };
                auto expect = [&](unsigned want, const char *what) {
                        Task::sleep(20);
                        if (broken == nullptr && prio() != want) broken = what;
                };
                // Two mutexes, two waiters.
                t->script = 0;
                Error err = start(*holder, 2, Inherit::holderEntry, t);
                if (err.isOk()) err = t->ready.acquire(1000);
                t->w[0] = {t, &t->a, WaitForever, Error()};
                t->w[1] = {t, &t->b, WaitForever, Error()};
                if (err.isOk()) err = start(t->waiters[0], 6, Inherit::waiterEntry, &t->w[0]);
                if (err.isOk()) expect(6, "not raised to the first waiter's 6");
                if (err.isOk()) err = start(t->waiters[1], 8, Inherit::waiterEntry, &t->w[1]);
                if (err.isOk()) expect(8, "not raised to the second waiter's 8");
                if (err.isOk()) {
                        t->go.release(); // unlocks a
                        err = t->done.acquire(1000);
                        if (err.isOk()) err = t->ready.acquire(1000);
                }
                if (err.isOk()) expect(8, "lowered at the first unlock, a contended mutex still held");
                if (err.isOk()) {
                        t->go.release(); // unlocks b
                        err = t->done.acquire(1000);
                        if (err.isOk()) err = t->ready.acquire(1000);
                }
                if (err.isOk()) expect(2, "not back to 2 after the last unlock");
                (void)holder->stop();
                (void)t->waiters[0].stop();
                (void)t->waiters[1].stop();
                // One mutex, a waiter that gives up.
                t->script = 1;
                holder = &t->holders[1];
                if (err.isOk()) err = start(*holder, 2, Inherit::holderEntry, t);
                if (err.isOk()) err = t->ready.acquire(1000);
                t->w[2] = {t, &t->a, 30, Error()};
                if (err.isOk()) err = start(t->waiters[2], 7, Inherit::waiterEntry, &t->w[2]);
                if (err.isOk()) err = t->done.acquire(1000);
                if (err.isOk() && t->w[2].got != Error::Timeout && broken == nullptr)
                        broken = "the waiter didn't time out";
                if (err.isOk()) expect(7, "lowered when the waiter gave up, before the unlock");
                if (err.isOk()) {
                        t->go.release();
                        err = t->ready.acquire(1000);
                }
                if (err.isOk()) expect(2, "not back to 2 after the unlock");
                (void)holder->stop();
                (void)t->waiters[2].stop();
                t->~Inherit();
                Memory::free(mem);
                if (err.isError()) return err;
                if (broken != nullptr) {
                        ctx.note("priority inheritance broken: %s (priority %u)", broken, seen);
                        return Error::IoError;
                }
                ctx.note("priority inheritance: as it should be");
                return Error::Ok;
        }

        Error runSync(BenchContext &ctx) {
                if (Error err = checkInheritance(ctx); err.isError()) return err;
                // 3 counts, held briefly: mostly takes that find one. 1 count, held longer: mostly waits,
                // and timeouts racing gives (the waiter's place given back, or the give's wake taken).
                if (Error err = syncPhase(ctx, "sem3_ops_per_s", 3, 200); err.isError()) return err;
                return syncPhase(ctx, "sem1_ops_per_s", 1, 20000);
        }

} // namespace

BMDS_BENCH(kernelSync, "kernel.sync", runSync, BenchQuick,
           "Semaphores and a mutex contended from both cores, checked: no count lost or doubled (ops/s)")
BMDS_BENCH(kernelClock, "kernel.clock", runClock, BenchQuick,
           "The monotonic clock read: the firmware's, and esp_timer's own through the ROM (ns a read)")
BMDS_BENCH(kernelLocks, "kernel.locks", runLocks, BenchQuick,
           "A kernel Mutex locked and unlocked, a Semaphore given and taken, a FreeRTOS critical section, uncontended "
           "(ns a pair)")
BMDS_BENCH(kernelSwitch, "kernel.switch", runSwitch, BenchQuick,
           "A task woken and waited for, there and back by semaphores: on this core and the other (us)")

BMDS_NAMESPACE_END
