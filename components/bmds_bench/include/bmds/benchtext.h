/**
 * @file      benchtext.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/** @brief A metric's figures over its runs (`devplan/bench.md` §4b). */
struct BenchStats {
                double   median = 0;
                double   min = 0;
                double   max = 0;
                double   mean = 0;
                double   spread = 0; ///< Half the range, in percent of the median.
                uint32_t runs = 0;
                bool     noisy = false; ///< `spread` above `BenchText::NoisyPercent`.
};

/**
 * @brief The `bench` command's pure parts (host-tested): the statistics, which
 *        benchmarks a pattern picks, the tags' names, the results file's name,
 *        and how figures print.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace BenchText {

        /** @brief A spread above this many percent marks a metric noisy. */
        inline constexpr double NoisyPercent = 5.0;

        /** @brief Summarizes @p samples (sorted in place). No samples: all zero. */
        BenchStats summarize(Span<double> samples);

        /**
         * @brief True if @p pattern picks benchmark @p name with @p tags: `@TAG`
         *        (`@all` picks all), a glob (`*`, `?`), or a dotted name or its
         *        prefix (`storage` picks `storage.sd.read`, `stor` doesn't).
         */
        bool matches(StringView pattern, StringView name, uint16_t tags);

        /** @brief Returns the tag bit named @p name (`quick`), or 0. */
        uint16_t tagNamed(StringView name);

        /** @brief Appends @p tags' names to @p out, separated by `,`. */
        Error appendTags(uint16_t tags, String &out);

        /** @brief A local date and time, as the results file's name and record take it. */
        struct When {
                        int year = 1970;
                        int month = 1; ///< 1-12.
                        int day = 1;
                        int hour = 0;
                        int minute = 0;
                        int second = 0;
                        int offsetMinutes = 0; ///< East of UTC.
        };

        /**
         * @brief Writes the results file's name to @p out: `SERIAL-VERSION-DATE-TIME.json`
         *        (`e8f60ae2e222-0.1.0-20261017-143205.json`), any character but a letter,
         *        digit, `.` or `_` in the serial or version made `_`.
         */
        Error fileName(StringView serial, StringView version, const When &when, String &out);

        /** @brief Appends @p when in ISO 8601 with its offset (`2026-10-17T14:32:05-07:00`). */
        Error appendIso8601(const When &when, String &out);

        /**
         * @brief Writes @p value for the terminal into @p out: four significant
         *        digits below 1000 (`21.34`, `0.0123`), whole above (`23456`).
         */
        void formatValue(double value, char *out, size_t outBytes);

} // namespace BenchText

BMDS_NAMESPACE_END
