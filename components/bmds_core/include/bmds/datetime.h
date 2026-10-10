/**
 * @file      datetime.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class String;

/**
 * @brief A point in time: nanoseconds since 1970-01-01 00:00:00 UTC.
 *
 * Signed 64 bits reach from 1677 to 2262. Leap seconds don't exist here
 * (as in POSIX time).
 *
 * @par Thread Safety
 * A trivially copyable value.
 */
struct Timestamp {
                /** @brief Nanoseconds in a second. */
                static constexpr int64_t NsPerSecond = 1'000'000'000;

                int64_t ns = 0; ///< Nanoseconds since the epoch.

                /** @brief Returns the timestamp @p seconds (plus @p nanoseconds) after the epoch. */
                static constexpr Timestamp fromSeconds(int64_t seconds, int32_t nanoseconds = 0) {
                        return Timestamp{seconds * NsPerSecond + nanoseconds};
                }

                /** @brief Returns whole seconds since the epoch, rounded down (also before 1970). */
                constexpr int64_t seconds() const {
                        return ns >= 0 ? ns / NsPerSecond : -((-ns + NsPerSecond - 1) / NsPerSecond);
                }

                /** @brief Returns the nanoseconds past `seconds()` (0 to 999999999). */
                constexpr int32_t nanoseconds() const { return static_cast<int32_t>(ns - seconds() * NsPerSecond); }

                /** @brief Compares two timestamps. */
                constexpr auto operator<=>(const Timestamp &) const = default;
};

/** @brief Calendar arithmetic on the proleptic Gregorian calendar. Pure functions. */
namespace Calendar {

        /** @brief Seconds in a day. */
        inline constexpr int64_t SecondsPerDay = 86400;

        /** @brief Returns true if @p year is a leap year. */
        constexpr bool isLeapYear(int64_t year) {
                return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        }

        /** @brief Returns the days in @p month (1-12) of @p year. */
        constexpr int daysInMonth(int64_t year, unsigned month) {
                constexpr uint8_t Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                return month == 2 && isLeapYear(year) ? 29 : Days[(month - 1) % 12];
        }

        /**
         * @brief Returns days since 1970-01-01 of a civil date.
         * @param year  Any year (negative works).
         * @param month 1-12.
         * @param day   1-31.
         */
        constexpr int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
                // Howard Hinnant's algorithm: eras of 400 years from 0000-03-01.
                year -= month <= 2;
                const int64_t  era = (year >= 0 ? year : year - 399) / 400;
                const unsigned yoe = static_cast<unsigned>(year - era * 400);
                const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
                const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                return era * 146097 + static_cast<int64_t>(doe) - 719468;
        }

        /** @brief Returns the weekday (0 = Sunday) of a day number from `daysFromCivil()`. */
        constexpr unsigned weekday(int64_t days) {
                const int64_t w = (days + 4) % 7; // 1970-01-01 was a Thursday.
                return static_cast<unsigned>(w < 0 ? w + 7 : w);
        }

} // namespace Calendar

/**
 * @brief A broken-down civil date and time, with no time zone.
 *
 * `fromSeconds()` fills every field; `toSeconds()` reads only the date and
 * time of day (not `weekday`/`yearDay`).
 *
 * @par Thread Safety
 * A trivially copyable value.
 */
struct DateTime {
                int32_t  year = 1970;    ///< Year (e.g. 2026).
                uint8_t  month = 1;      ///< Month, 1-12.
                uint8_t  day = 1;        ///< Day of the month, 1-31.
                uint8_t  hour = 0;       ///< Hour, 0-23.
                uint8_t  minute = 0;     ///< Minute, 0-59.
                uint8_t  second = 0;     ///< Second, 0-59.
                uint8_t  weekday = 4;    ///< Day of the week, 0 = Sunday.
                uint16_t yearDay = 0;    ///< Day of the year, 0-365.
                int32_t  nanosecond = 0; ///< Nanoseconds, 0-999999999.

                /**
                 * @brief Breaks down @p seconds since the epoch (the epoch of whatever
                 *        clock: UTC seconds give UTC, local seconds give local time).
                 */
                static DateTime fromSeconds(int64_t seconds, int32_t nanosecond = 0);

                /** @brief Returns seconds since the epoch of the date and time of day. */
                int64_t toSeconds() const;

                /** @brief Returns true if the date exists and the time of day is in range. */
                bool isValid() const;
};

/**
 * @brief A time as shown in a time zone: what `TimeZone::localTime()` returns.
 */
struct LocalTime {
                /** @brief Longest zone abbreviation kept (POSIX allows up to `TZNAME_MAX`). */
                static constexpr size_t MaxAbbreviation = 15;

                DateTime local;                                  ///< The local date and time.
                int64_t  unixSeconds = 0;                        ///< The same instant as UTC seconds since the epoch.
                int32_t  utcOffset = 0;                          ///< Seconds east of UTC (local = UTC + offset).
                uint8_t  isDst = 0;                              ///< 1 during daylight saving time.
                char     abbreviation[MaxAbbreviation + 1] = {}; ///< Zone abbreviation, e.g. "CEST".
};

/**
 * @brief Formats @p time as `strftime` would, appending to @p out.
 *
 * Conversions: `%a %A %b %B %c %C %d %D %e %F %h %H %I %j %k %l %m %M %n %N
 * %p %r %R %s %S %t %T %u %w %y %Y %z %Z %%` (as GNU `date`; `%N` is
 * nanoseconds), and GNU's flags before a number's: `-` not padded (`%-d`:
 * "1"), `_` padded with spaces, `0` with zeros. Anything else is copied as
 * it is.
 * @return `Ok` or `NoMem`.
 */
Error formatTime(String &out, StringView format, const LocalTime &time);

BMDS_NAMESPACE_END
