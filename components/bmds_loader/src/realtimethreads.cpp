/**
 * @file      realtimethreads.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Program threads' scheduling (devplan/processes.md §3.12).
 */

#include "realtimethreads.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <bmds/audiodevice.h>
#include <bmds/errno.h>
#include <bmds/log.h>
#include <bmds/mutex.h>
#include <bmds/realtimebudget.h>
#include <bmds/staticobject.h>
#include <bmds/tasklist.h>
#include <bmds/waketimer.h>

#include "running.h"

BMDS_NAMESPACE_BEGIN

namespace RealTimeThreads {

        namespace {

                // How often the budget is told the real-time threads' time, while there are any.
                constexpr uint64_t CheckUs = 50'000;

                struct Slot {
                                ProgramThread *thread = nullptr;
                                uint64_t       cpuUs = 0; // its CPU time at the last check
                };

                struct State {
                                Mutex              lock;
                                Slot               slots[MaxThreads];
                                size_t             count = 0;
                                RealTimeBudget     budget;
                                esp_timer_handle_t timer = nullptr;
                                bool               warned = false; // since the first real-time thread
                };

                constinit StaticObject<State> state;

                bool isRealTime(uint32_t policy) {
                        return policy == BMDS_SCHED_FIFO || policy == BMDS_SCHED_RR;
                }

                unsigned priorityLocked(const ProgramThread &t) {
                        if (!isRealTime(t.policy) || state->budget.isThrottled()) return t.normalPriority;
                        return RealTimeBudget::kernelPriority(t.rtPriority, CONFIG_BMDS_PROGRAM_RT_PRIORITY_LOW,
                                                              CONFIG_BMDS_PROGRAM_RT_PRIORITY_HIGH);
                }

                // Puts a started thread's task at the priority its policy and the budget give it.
                // An audio device that raised it forgets it, and raises it again at its next cycle.
                void applyLocked(ProgramThread &t) {
                        TaskHandle_t task = t.task.handle();
                        if (t.realtime) AudioDevice::forgetTask(task);
                        vTaskPrioritySet(task, priorityLocked(t));
                        return;
                }

                void check(void *) {
                        State &s = *state;
                        if (s.lock.tryLock().isError()) return; // its time counts at the next check
                        uint64_t used = 0;
                        for (size_t i = 0; i < s.count; ++i) {
                                Slot          &slot = s.slots[i];
                                const uint64_t now = TaskList::cpuUs(slot.thread->task.handle());
                                used += now - slot.cpuUs;
                                slot.cpuUs = now;
                        }
                        const RealTimeBudget::Action action = s.budget.account(esp_timer_get_time(), used);
                        if (action != RealTimeBudget::Action::None) {
                                for (size_t i = 0; i < s.count; ++i) {
                                        ProgramThread &t = *s.slots[i].thread;
                                        if (action == RealTimeBudget::Action::Throttle) ++t.throttled;
                                        applyLocked(t);
                                }
                        }
                        if (action == RealTimeBudget::Action::Throttle && !s.warned) {
                                s.warned = true;
                                bmdsWarn("Loader: real-time threads had 95%% of a second: held back for the rest of "
                                         "each (once logged)");
                        }
                        s.lock.unlock();
                        return;
                }

                bool countLocked(ProgramThread &t) {
                        State &s = *state;
                        for (size_t i = 0; i < s.count; ++i)
                                if (s.slots[i].thread == &t) return true;
                        if (s.count == MaxThreads) return false;
                        if (s.timer == nullptr) {
                                esp_timer_create_args_t args = {};
                                args.callback = check;
                                args.name = "rt budget";
                                if (esp_timer_create(&args, &s.timer) != ESP_OK) {
                                        s.timer = nullptr;
                                        return false;
                                }
                        }
                        s.slots[s.count].thread = &t;
                        s.slots[s.count].cpuUs = TaskList::cpuUs(t.task.handle());
                        if (++s.count == 1) {
                                s.budget.reset();
                                s.warned = false;
                                (void)esp_timer_start_periodic(s.timer, CheckUs);
                        }
                        return true;
                }

                void uncountLocked(ProgramThread &t) {
                        State &s = *state;
                        for (size_t i = 0; i < s.count; ++i) {
                                if (s.slots[i].thread != &t) continue;
                                s.slots[i] = s.slots[--s.count];
                                s.slots[s.count] = Slot();
                                break;
                        }
                        if (s.count == 0 && s.timer != nullptr) {
                                (void)esp_timer_stop(s.timer);
                                s.budget.reset();
                        }
                        return;
                }

        } // namespace

        Error init() {
                if (!state.isCreated()) state.create();
                return Error::Ok;
        }

        unsigned inherit(ProgramThread &child, const ProgramThread *parent, unsigned normalPriority, bool exec) {
                Mutex::Locker locker(state->lock, WaitForever);
                child.normalPriority = static_cast<uint8_t>(normalPriority);
                child.resetOnFork = exec && parent != nullptr && parent->resetOnFork;
                if (parent != nullptr && isRealTime(parent->policy) && (exec || !parent->resetOnFork)) {
                        child.policy = parent->policy;
                        child.rtPriority = parent->rtPriority;
                } else {
                        child.policy = BMDS_SCHED_OTHER;
                        child.rtPriority = 0;
                }
                return priorityLocked(child);
        }

        void started(ProgramThread &thread) {
                if (isRealTime(thread.policy)) (void)WakeTimer::init();
                Mutex::Locker locker(state->lock, WaitForever);
                thread.begun = true;
                if (!isRealTime(thread.policy)) return;
                if (!countLocked(thread)) {
                        // No room: it runs as any other (the thread that set it was told 0).
                        bmdsWarn("Loader: thread %d: more than %u real-time threads, running it as SCHED_OTHER",
                                 static_cast<int>(thread.tid), static_cast<unsigned>(MaxThreads));
                        thread.policy = BMDS_SCHED_OTHER;
                        thread.rtPriority = 0;
                }
                vTaskPrioritySet(nullptr, priorityLocked(thread));
                return;
        }

        void forget(ProgramThread &thread) {
                if (!state.isCreated()) return;
                Mutex::Locker locker(state->lock, WaitForever);
                uncountLocked(thread);
                return;
        }

        int32_t set(ProgramThread &thread, uint32_t policy, uint32_t priority) {
                const bool     reset = (policy & BMDS_SCHED_RESET_ON_FORK) != 0;
                const uint32_t base = policy & ~BMDS_SCHED_RESET_ON_FORK;
                if (isRealTime(base)) {
                        if (priority < RealTimeBudget::MinPriority || priority > RealTimeBudget::MaxPriority)
                                return -BMDS_EINVAL;
                        (void)WakeTimer::init(); // its sleeps: on the timer if there is one
                } else if (base == BMDS_SCHED_OTHER || base == BMDS_SCHED_BATCH || base == BMDS_SCHED_IDLE) {
                        if (priority != 0) return -BMDS_EINVAL;
                } else {
                        return -BMDS_EINVAL;
                }
                Mutex::Locker locker(state->lock, WaitForever);
                // Not yet started: started() counts it and sets its priority.
                if (thread.begun) {
                        if (isRealTime(base)) {
                                if (!countLocked(thread)) return -BMDS_EAGAIN;
                        } else {
                                uncountLocked(thread);
                        }
                }
                thread.policy = static_cast<uint8_t>(base);
                thread.rtPriority = static_cast<uint8_t>(priority);
                thread.resetOnFork = reset;
                if (thread.begun) applyLocked(thread);
                return 0;
        }

        void get(ProgramThread &thread, bool self, bmds_sched_t &out) {
                Mutex::Locker locker(state->lock, WaitForever);
                out.policy = thread.policy | (thread.resetOnFork ? BMDS_SCHED_RESET_ON_FORK : 0u);
                out.priority = thread.rtPriority;
                const BaseType_t core = thread.begun ? xTaskGetCoreID(thread.task.handle()) : tskNO_AFFINITY;
                out.cores = core == tskNO_AFFINITY ? (1u << portNUM_PROCESSORS) - 1u : 1u << core;
                out.core = self ? static_cast<int32_t>(xPortGetCoreID()) : -1;
                out.kernel_priority = thread.begun ? uxTaskPriorityGet(thread.task.handle()) : priorityLocked(thread);
                out.throttled = thread.throttled;
                return;
        }

} // namespace RealTimeThreads

BMDS_NAMESPACE_END
