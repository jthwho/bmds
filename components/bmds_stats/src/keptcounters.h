/**
 * @file      keptcounters.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system monitor's counters kept across boots (`devplan/keptstate.md` §5): lows,
 *        highs and totals of a fixed list, read each pass of `Kept` and saved in the LP RAM when
 *        they change.
 *
 * @par Thread Safety
 * Thread-safe (a lock).
 */
namespace KeptCounters {

        /** @brief How a counter is kept. */
        enum class Mode : uint8_t {
                Low,  ///< The lowest value seen across boots.
                High, ///< The highest.
                Total ///< A `Total`'s count summed over boots.
        };

        /** @brief A counter as `stats kept` shows it. */
        struct Row {
                        const char *name;
                        Mode        mode;
                        int64_t     thisBoot; ///< At the last pass (`Stat::Unavailable`: not seen this boot).
                        int64_t     allBoots; ///< Across boots, this one included (`Stat::Unavailable`: never seen).
        };

        /** @brief The record as a whole. */
        struct Summary {
                        uint32_t boots = 0;   ///< Boots counted, this one included.
                        int64_t  startUs = 0; ///< The RTC timer when it started (`Boots::wallTime()`).
        };

        /** @brief From `Sysmon::init()`: the record loaded, this boot counted, its keeper added. */
        Error init();

        /** @brief The counters kept. */
        size_t count();

        /** @brief Copies the counters into @p out. @return How many. */
        size_t rows(Span<Row> out);

        /** @brief Returns the record's summary. */
        Summary summary();

        /** @brief Starts again from this boot. */
        void clear();

} // namespace KeptCounters

BMDS_NAMESPACE_END
