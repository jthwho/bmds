/**
 * @file      kept.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief The keepers of state kept across resets (`devplan/keptstate.md` §2c): functions that
 *        copy their records (`KeptRecord`) into the LP RAM if they changed, called together each
 *        pass of one thread, and at once before a clean restart.
 *
 * The LP RAM is slow (about 500 ns a word), so records are kept in ordinary RAM and saved only
 * here: a hardware reset or a panic loses at most the last interval.
 *
 * @par Thread Safety
 * Thread-safe. Keepers run one pass at a time, on the `kept` thread or the caller of `flush()`.
 */
namespace Kept {

        /** @brief Saves its record if it changed (never blocks long: a pass waits on it). */
        using Keeper = void (*)(void *arg);

        /** @brief Keepers at most. */
        constexpr size_t MaxKeepers = 8;

        /** @brief The interval unless set (`sysmon/keep-interval`). */
        constexpr uint32_t DefaultIntervalMs = 10000;

        /**
         * @brief Adds @p keeper (before or after `init()`; never removed).
         * @return `Ok`, or `Full`.
         */
        Error add(Keeper keeper, void *arg);

        /** @brief Boot stage: starts the thread, its first pass at once. @return `Ok` or `NoMem`. */
        Error init();

        /** @brief Sets the interval; 0 stops the passes (`flush()` still runs them). */
        void setInterval(uint32_t ms);

        /** @brief Returns the interval. */
        uint32_t interval();

        /**
         * @brief Runs a pass now on the calling thread (a clean restart's last).
         * @blocking A running pass, a second at most (then skipped).
         */
        void flush();

} // namespace Kept

BMDS_NAMESPACE_END
