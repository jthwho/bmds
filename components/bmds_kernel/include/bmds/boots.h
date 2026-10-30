/**
 * @file      boots.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The record of the last boots (`devplan/keptstate.md` §4): when each started, when it was
 *        last seen alive, what ended it, which firmware. Kept in the LP RAM (`KeptRecord`), so any
 *        reset but a power cut leaves it; the time last alive is saved each pass of `Kept`.
 *
 * Times are the RTC timer's (`Detail::persistentTimeUs()`), which counts across resets: the
 * difference between two boots' starts is the time between them.
 *
 * @par Thread Safety
 * Thread-safe (a lock).
 */
namespace Boots {

        /** @brief Boots kept. */
        constexpr size_t MaxEntries = 8;

        /** @brief `Entry::endReason` while the boot runs. */
        constexpr int32_t Running = -1;

        /** @brief A boot. */
        struct Entry {
                        int64_t startUs;      ///< The RTC timer at the application's start.
                        int64_t aliveUs;      ///< The RTC timer when last seen alive.
                        int32_t endReason;    ///< What ended it (`System::resetReasonCode()`), or `Running`.
                        char    firmware[32]; ///< The firmware's version.
        };

        /** @brief Boot stage: ends the last boot's entry with this reset's reason, starts this one's. */
        Error init();

        /**
         * @brief Copies the boots into @p out, the newest (this one) first.
         * @return How many.
         */
        size_t list(Span<Entry> out);

        /**
         * @brief Converts the RTC timer's @p rtcUs to the wall time, from the clock now.
         * @return True, or false while the clock isn't set (@p out unchanged).
         */
        bool wallTime(int64_t rtcUs, Timestamp &out);

        /** @brief Starts the record again with this boot. */
        void clear();

} // namespace Boots

BMDS_NAMESPACE_END
