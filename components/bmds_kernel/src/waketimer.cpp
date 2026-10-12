/**
 * @file      waketimer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Precise sleeps on a GPTimer (devplan/processes.md §3.12).
 */

#include <atomic>
#include <driver/gptimer.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <bmds/deadline.h>
#include <bmds/log.h>
#include <bmds/semaphore.h>
#include <bmds/waketimer.h>

BMDS_DEBUG(WakeTimer)

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t TimerHz = 1'000'000; // counts are microseconds, as the deadlines

        // A sleep this close is spun out: an alarm set in the critical section is never past.
        constexpr int64_t SpinUs = 20;

        // An alarm set from the interrupt for a sleeper just short of its time.
        constexpr int64_t MinLeadUs = 2;

        // One sleeper, on its own stack; linked while it waits, under `lock`.
        struct Sleeper {
                        int64_t    deadlineUs = 0;
                        Semaphore *wake = nullptr;
                        Sleeper   *next = nullptr;
                        bool       linked = false;
        };

        enum State : uint8_t {
                Unset,
                Starting,
                Ready,
                Failed
        };

        constinit portMUX_TYPE         lock = portMUX_INITIALIZER_UNLOCKED;
        constinit Sleeper             *head = nullptr; // by deadline, the first soonest
        constinit gptimer_handle_t     timer = nullptr;
        constinit std::atomic<uint8_t> state{Unset};

        // Sets the alarm @p leadUs (at least) from now, for the first sleeper's deadline.
        // Returns false if the count has already passed it (the caller handles the sleepers due).
        bool armLocked(int64_t nowUs, int64_t leadUs) {
                int64_t ahead = head->deadlineUs - nowUs;
                if (ahead < leadUs) ahead = leadUs;
                uint64_t count = 0;
                (void)gptimer_get_raw_count(timer, &count);
                gptimer_alarm_config_t alarm = {};
                alarm.alarm_count = count + static_cast<uint64_t>(ahead);
                (void)gptimer_set_alarm_action(timer, &alarm);
                uint64_t after = 0;
                (void)gptimer_get_raw_count(timer, &after);
                return after < alarm.alarm_count;
        }

        // The alarm: every sleeper due is released, and the alarm set for the next. Releasing
        // under the lock: a sleeper leaving takes it first, so its semaphore is still there.
        bool onAlarm(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *) {
                portENTER_CRITICAL_ISR(&lock);
                for (;;) {
                        const int64_t now = esp_timer_get_time();
                        while (head != nullptr && head->deadlineUs <= now) {
                                Sleeper *s = head;
                                head = s->next;
                                s->linked = false;
                                s->wake->releaseFromIsr();
                        }
                        if (head == nullptr || armLocked(now, MinLeadUs)) break;
                }
                portEXIT_CRITICAL_ISR(&lock);
                return false; // releaseFromIsr() yields at the interrupt's exit
        }

        // The tick's sleep: whole ticks, at least to the deadline.
        Error tickSleepUntil(int64_t deadlineUs, Interrupt *interrupt) {
                Semaphore never(1, 0);
                for (;;) {
                        const int64_t left = deadlineUs - esp_timer_get_time();
                        if (left <= 0) return Error::Ok;
                        const int64_t ms = (left + 999) / 1000;
                        Deadline      d(static_cast<unsigned int>(ms < INT32_MAX ? ms : INT32_MAX));
                        const Error   err = Interrupt::wait(interrupt, never, d);
                        if (err == Error::Interrupted) return err;
                }
        }

} // namespace

Error WakeTimer::init() {
        uint8_t was = Unset;
        if (!state.compare_exchange_strong(was, Starting, std::memory_order_acq_rel)) {
                // Another caller's start, done or under way (sleeps stay on the tick meanwhile).
                return was == Failed ? Error::NotSupported : Error::Ok;
        }
        gptimer_config_t config = {};
        config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        config.direction = GPTIMER_COUNT_UP;
        config.resolution_hz = TimerHz;
        esp_err_t err = gptimer_new_timer(&config, &timer);
        if (err == ESP_OK) {
                gptimer_event_callbacks_t callbacks = {};
                callbacks.on_alarm = onAlarm;
                err = gptimer_register_event_callbacks(timer, &callbacks, nullptr);
        }
        if (err == ESP_OK) err = gptimer_enable(timer);
        if (err == ESP_OK) err = gptimer_start(timer);
        if (err != ESP_OK) {
                bmdsErr("WakeTimer: no timer (%s): sleeps stay on the tick", esp_err_to_name(err));
                state.store(Failed, std::memory_order_release);
                return Error::NotSupported;
        }
        bmdsDebug("WakeTimer: ready");
        state.store(Ready, std::memory_order_release);
        return Error::Ok;
}

bool WakeTimer::isReady() {
        return state.load(std::memory_order_acquire) == Ready;
}

Error WakeTimer::sleepUntil(int64_t deadlineNs, Interrupt *interrupt) {
        // Rounded up: never before the time asked for.
        const int64_t deadlineUs = deadlineNs / 1000 + (deadlineNs % 1000 > 0 ? 1 : 0);
        if (interrupt != nullptr && interrupt->isRaised()) return Error::Interrupted;
        if (!isReady()) return tickSleepUntil(deadlineUs, interrupt);
        Semaphore wake(1, 0);
        Sleeper   me;
        me.deadlineUs = deadlineUs;
        me.wake = &wake;
        bool spin = false;
        portENTER_CRITICAL(&lock);
        const int64_t now = esp_timer_get_time();
        if (deadlineUs - now <= SpinUs) {
                spin = true;
        } else {
                Sleeper **at = &head;
                while (*at != nullptr && (*at)->deadlineUs <= deadlineUs) at = &(*at)->next;
                me.next = *at;
                *at = &me;
                me.linked = true;
                // Never past: the lead is far more than this section takes.
                if (head == &me) (void)armLocked(now, SpinUs);
        }
        portEXIT_CRITICAL(&lock);
        if (spin) {
                while (esp_timer_get_time() < deadlineUs) {}
                return Error::Ok;
        }
        // The tick's wait behind the alarm, a tick or two past the time: the alarm releases
        // `wake` first, unless held off (a flash write) or the process is stopped.
        Error err = Error::Ok;
        for (;;) {
                const int64_t left = deadlineUs - esp_timer_get_time();
                if (left <= 0) break;
                const int64_t ms = left / 1000 + 2;
                Deadline      backstop(static_cast<unsigned int>(ms < INT32_MAX ? ms : INT32_MAX));
                err = Interrupt::wait(interrupt, wake, backstop);
                if (err == Error::Interrupted) break;
        }
        portENTER_CRITICAL(&lock);
        if (me.linked) {
                for (Sleeper **at = &head; *at != nullptr; at = &(*at)->next) {
                        if (*at != &me) continue;
                        *at = me.next;
                        break;
                }
                me.linked = false;
        }
        portEXIT_CRITICAL(&lock);
        return err == Error::Interrupted ? err : Error(Error::Ok);
}

BMDS_NAMESPACE_END
