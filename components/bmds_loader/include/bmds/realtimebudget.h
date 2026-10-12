/**
 * @file      realtimebudget.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Program threads' real-time scheduling, the portable part (devplan/processes.md
 *        §3.12): which kernel priority a real-time priority runs at, and Linux's budget for
 *        real-time threads (95 % of each second, `sched_rt_runtime_us`).
 *
 * The budget is told, now and then, how much CPU time the real-time threads have had since
 * it was last told; once they have had `RuntimeUs` of the current `PeriodUs` it says to
 * throttle them (run them at their normal priority), and at the next period's start to
 * release them.
 *
 * @par Thread Safety
 * Not thread-safe: its owner's lock.
 */
class RealTimeBudget {
        public:
                static constexpr int64_t PeriodUs = 1'000'000; ///< Linux's `sched_rt_period_us`.
                static constexpr int64_t RuntimeUs = 950'000;  ///< Linux's `sched_rt_runtime_us`.

                /** @brief Real-time priorities (`SCHED_FIFO`, `SCHED_RR`). */
                static constexpr uint32_t MinPriority = 1;
                static constexpr uint32_t MaxPriority = 99;

                /** @brief What `account()` asks of its caller. */
                enum class Action : uint8_t {
                        None,     ///< Nothing changes.
                        Throttle, ///< The real-time threads go to their normal priority.
                        Release,  ///< They get their real-time priority back.
                };

                /**
                 * @brief The kernel priority real-time priority @p priority (1-99) runs at:
                 *        1-99 spread evenly over @p low to @p high (at most 99 of them).
                 */
                static constexpr uint32_t kernelPriority(uint32_t priority, uint32_t low, uint32_t high) {
                        if (priority < MinPriority) priority = MinPriority;
                        if (priority > MaxPriority) priority = MaxPriority;
                        return low + (priority - MinPriority) * (high - low + 1) / MaxPriority;
                }

                /**
                 * @brief The real-time threads had @p usedUs of CPU time since the last call;
                 *        it is @p nowUs (monotonic).
                 */
                Action account(int64_t nowUs, uint64_t usedUs);

                /** @brief Starts over, released (no real-time thread is left). */
                void reset();

                /** @brief Returns true while throttled. */
                bool isThrottled() const { return _throttled; }

        private:
                int64_t  _periodStart = -1;
                uint64_t _used = 0;
                bool     _throttled = false;
};

BMDS_NAMESPACE_END
