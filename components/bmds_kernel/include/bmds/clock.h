/**
 * @file      clock.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/clockmodel.h>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/stringview.h>
#include <bmds/timezone.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system clock: monotonic time, and wall time (UTC) kept the way
 *        the Linux kernel keeps it.
 *
 * Wall time is a software clock over the monotonic time base (see
 * `ClockModel`): sources step it (`set()`), slew it (`adjust()`) and
 * correct its rate (`setFrequency()`). Until a source sets it, it counts
 * from 2026-01-01. A set clock survives resets: the target keeps its
 * offset from the RTC timer in memory a reset doesn't clear, and
 * `init()` resumes from it (source `Rtc`). The C library's `time()` is
 * stepped along with it.
 *
 * The kernel keeps UTC; the time zone (`setTimeZone()`) is for display and
 * for file systems that store local time.
 *
 * @par Thread Safety
 * Thread-safe in tasks; reads take no lock. Not ISR-safe.
 */
namespace Clock {

        /**
         * @brief Boot stage (target): sets the kernel time zone from
         *        `CONFIG_BMDS_TIMEZONE` and resumes the time a reset carried over.
         * @return `Ok` (also when there was nothing to resume).
         */
        Error init();

        /** @brief Returns nanoseconds since boot. Never steps. @nonblocking */
        int64_t monotonicNs();

        /** @brief Returns the wall time (UTC). @nonblocking */
        Timestamp now();

        /** @brief Returns true once a source has set the clock. */
        bool isSet();

        /** @brief Returns the source, last set time, remaining slew and frequency. */
        ClockStatus status();

        /**
         * @brief Steps the clock to @p wall (`settimeofday`).
         * @return `Ok`, or `Invalid` for `ClockSource::None`.
         */
        Error set(Timestamp wall, ClockSource source);

        /**
         * @brief Slews @p offsetNs in at 500 ppm (`adjtime`), replacing what was left.
         * @return The part of the previous adjustment not yet applied.
         */
        int64_t adjust(int64_t offsetNs);

        /**
         * @brief Sets the frequency correction (`adjtimex`), for a discipline loop.
         * @return `Ok`, or `OutOfRange` beyond ±500 ppm.
         */
        Error setFrequency(int32_t ppb);

        /**
         * @brief Sets the kernel time zone from a POSIX `TZ` string.
         * @return `Ok` or `Invalid`.
         */
        Error setTimeZone(StringView posixTz);

        /** @brief Returns the kernel time zone. */
        TimeZone timeZone();

        /** @brief Returns @p time in the kernel time zone. */
        LocalTime localTime(Timestamp time);

        /**
         * @brief Saves the reset record now (done after every change; call
         *        before a deliberate restart so the RTC's drift since is small).
         */
        void saveResetRecord();

} // namespace Clock

BMDS_NAMESPACE_END
