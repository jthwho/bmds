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
 * for file systems that store local time. It is POSIX rules, or a name in
 * the tz database (devplan/time.md §9), which the filesystem looks up
 * through a resolver (`setZoneResolver()`).
 *
 * @par Thread Safety
 * Thread-safe in tasks; reads take no lock. Not ISR-safe.
 */
namespace Clock {

        /** @brief Longest zone name kept (`America/Argentina/ComodRivadavia` is 32). */
        constexpr size_t MaxZoneName = 39;

        /** @brief Why a zone name isn't in effect yet (UTC is, meanwhile). */
        enum class ZonePending : uint8_t {
                None,       ///< In effect (or no name: POSIX rules).
                NoDatabase, ///< No database to look it up in (no card yet).
                Unknown,    ///< The database doesn't have it (set before the database was there).
        };

        /** @brief The kernel zone's name, when it was set by name. */
        struct ZoneName {
                        char        name[MaxZoneName + 1] = {}; ///< Empty when the zone is POSIX rules.
                        ZonePending pending = ZonePending::None;
                        uint8_t     reserved[3] = {};

                        /** @brief Returns the name ("" for POSIX rules). */
                        StringView view() const { return StringView(name); }
        };

        /**
         * @brief Looks zone @p name up and fills @p rules with its current rules.
         * @return `Ok`; `NotExist` if the database doesn't have it; any other
         *         error if there's no database to ask now (it stays pending).
         */
        using ZoneResolver = Error (*)(StringView name, TimeZone &rules);

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
         * @brief Sets the kernel time zone: POSIX rules, or a zone name
         *        (told apart by `TimeZone::isName()`; a leading `:` is dropped).
         *
         * A name is looked up through the resolver. One that can't be looked
         * up now (no resolver or no database) is kept pending, with UTC in
         * effect, and looked up again by `retryZoneName()`. Blocking (the
         * lookup reads a file); not from the storage lock.
         *
         * @return `Ok` (pending too), `Invalid` (malformed rules, or a name
         *         longer than `MaxZoneName`, with a `.` or starting with `/`),
         *         or `NotExist` (not in the database); the zone is unchanged on error.
         */
        Error setTimeZone(StringView tz);

        /**
         * @brief Checks @p tz as `setTimeZone()` would, without setting it.
         * @return As `setTimeZone()`.
         * @blocking A lookup in the zone database, for a name.
         */
        Error checkTimeZone(StringView tz);

        /** @brief Returns the kernel zone's name, and whether it is pending. */
        ZoneName zoneName();

        /** @brief Installs the resolver (the VFS's), and retries a pending name with it. */
        void setZoneResolver(ZoneResolver resolver);

        /**
         * @brief Looks a pending zone name up again (after a mount). Cheap
         *        when nothing is pending. Not from the storage lock.
         */
        void retryZoneName();

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
