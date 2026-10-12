/**
 * @file      waketimer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/interrupt.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Sleeps that end at their time, not on the next tick (devplan/processes.md §3.12).
 *
 * One hardware timer serves every sleeper: each waits on a semaphore of its own, in a list
 * ordered by deadline on its own stack, and the timer's alarm, set for the first, releases
 * the ones due. A tick-based wait stays behind it, so a sleep ends even if the timer is held
 * off (a flash write masks its interrupt). Until `init()` has succeeded (and on the host),
 * `sleepUntil()` sleeps on the tick.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class WakeTimer {
        public:
                WakeTimer() = delete;

                /**
                 * @brief Takes the hardware timer, once (later calls return what the first did).
                 * @return `Ok`, or the driver's error (no timer free, no memory).
                 * @blocking
                 */
                static Error init();

                /** @brief Returns true once `init()` has succeeded. */
                static bool isReady();

                /**
                 * @brief Sleeps until `Clock::monotonicNs()` reaches @p deadlineNs, or until
                 *        @p interrupt (may be null) is raised.
                 * @return `Ok` (at or after the deadline), or `Interrupted`.
                 * @blocking
                 */
                static Error sleepUntil(int64_t deadlineNs, Interrupt *interrupt);
};

BMDS_NAMESPACE_END
