/**
 * @file      sdhostpatch.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief ESP-IDF's SD host made cheaper per command (devplan/storage.md §17), through `--wrap`.
 *
 * - `sdmmc_wait_for_idle()` waits for the card to release D0 while it programs, woken by the
 *   line's rising edge (a GPIO interrupt on its pad), instead of sending CMD13 back to back.
 * - The slot's bus width, sampling mode, delay phase and delay line, which ESP-IDF applies on
 *   every command, are applied only when they differ from those last applied, and the clock read
 *   back is remembered for the frequency it was set to.
 *
 * @par Thread Safety
 * The wrapped calls run under ESP-IDF's controller mutex (the setup) or the card's lock (the
 * wait); `init()` once, `forget()` under the card's lock with no command in flight.
 */
namespace SdHostPatch {

        /**
         * @brief Takes D0's edge interrupt (through the shared GPIO ISR service). Without it
         *        (or with @p d0Gpio negative) a wait sleeps between looks at the line.
         * @return `Ok`, or the GPIO driver's error.
         */
        Error init(int d0Gpio);

        /** @brief Forgets the settings last applied: the host was started or stopped (its reset clears them). */
        void forget();

} // namespace SdHostPatch

BMDS_NAMESPACE_END
