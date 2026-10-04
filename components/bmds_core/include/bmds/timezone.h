/**
 * @file      timezone.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A time zone described by a POSIX `TZ` string.
 *
 * `std offset [dst [offset] [,start[/time],end[/time]]]`, as glibc reads
 * it without a zone database:
 *
 * - Names are three or more letters, or anything in `<...>` (`<+0530>`).
 * - Offsets are `[+|-]hh[:mm[:ss]]` **west** of UTC (`EST5`, `CET-1`). The
 *   DST offset defaults to one hour east of standard time.
 * - Rules are `Mm.w.d` (day `d`, 0 = Sunday, of week `w`, 5 = last, of
 *   month `m`), `Jn` (day 1-365, never counting February 29) or `n` (day
 *   0-365). The time is local wall time, default `02:00`, and may be
 *   negative or past 24 hours. A DST name without rules gets the US rules
 *   (`M3.2.0,M11.1.0`), as glibc does.
 *
 * Examples: `UTC0`, `CET-1CEST,M3.5.0,M10.5.0/3`, `EST5EDT,M3.2.0,M11.1.0`,
 * `AEST-10AEDT,M10.1.0,M4.1.0/3` (southern hemisphere), `<+0530>-5:30`.
 * The empty string and the bare names `UTC` and `GMT` mean UTC.
 *
 * A default-constructed (zero-filled) TimeZone is UTC. The object is a
 * fixed-size value with no allocation and no padding, so it can be held in
 * a `SeqLocked`.
 *
 * @par Thread Safety
 * A trivially copyable value; `const` methods are pure.
 */
class TimeZone {
        public:
                /** @brief Longest `TZ` string kept. */
                static constexpr size_t MaxSpec = 47;

                /** @brief Constructs UTC. */
                constexpr TimeZone() = default;

                /**
                 * @brief Parses a POSIX `TZ` string.
                 * @return The zone, or `Invalid` (malformed, or longer than `MaxSpec`).
                 */
                static Result<TimeZone> parse(StringView spec);

                /** @brief Returns the `TZ` string it was parsed from (empty for UTC by default). */
                StringView spec() const { return StringView(_spec); }

                /** @brief Returns true if the zone has daylight saving time. */
                bool hasDst() const { return _hasDst != 0; }

                /** @brief Returns the standard offset in seconds east of UTC. */
                int32_t standardOffset() const { return _stdOffset; }

                /** @brief Returns @p time as shown in this zone. */
                LocalTime localTime(Timestamp time) const;

                /**
                 * @brief Returns the UTC seconds of local time @p localSeconds
                 *        (local seconds since the epoch, as `DateTime::toSeconds()` gives).
                 *
                 * A local time that happens twice (when DST ends) is the
                 * earlier instant; one that doesn't happen (when DST starts)
                 * is read with the standard offset, so it lands after the gap.
                 */
                int64_t toUtc(int64_t localSeconds) const;

                /** @brief Returns true if daylight saving time is in effect at @p utcSeconds. */
                bool isDst(int64_t utcSeconds) const;

        private:
                // When a DST transition happens, in its year.
                struct Rule {
                                enum Kind : uint8_t {
                                        MonthWeekDay,
                                        Julian1,
                                        Julian0
                                };

                                int32_t  time = 7200; // Local seconds after midnight.
                                uint16_t day = 0;     // Jn / n.
                                uint8_t  kind = MonthWeekDay;
                                uint8_t  month = 0; // Mm.w.d
                                uint8_t  week = 0;
                                uint8_t  weekday = 0;
                                uint8_t  reserved[2] = {};
                };

                static bool    parseRule(StringView &text, Rule &rule);
                static int64_t transition(int64_t year, const Rule &rule, int32_t offsetBefore);

                char    _spec[MaxSpec + 1] = {};
                char    _stdName[LocalTime::MaxAbbreviation + 1] = {};
                char    _dstName[LocalTime::MaxAbbreviation + 1] = {};
                int32_t _stdOffset = 0; // Seconds east of UTC.
                int32_t _dstOffset = 0;
                Rule    _start;
                Rule    _end;
                uint8_t _hasDst = 0;
                uint8_t _reserved[3] = {};
};

BMDS_NAMESPACE_END
