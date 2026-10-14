/**
 * @file      recoveryscreen.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/console.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Recovery's screen (devplan/update.md §12): "Recovery Mode" across the top, the update's
 *        progress and result under it, and a terminal with a shell on the rest, typed into with
 *        a USB keyboard.
 *
 * @par Thread Safety
 * `init()` once at boot.
 */
namespace RecoveryScreen {

        /**
         * @brief Boot stage: starts the display at 1280×720, draws the screen and starts its task.
         *        Without a display, recovery runs on without it (`Ok`, logged).
         */
        Error init();

        /** @brief The terminal's console, for a shell; null when there is no screen. */
        Console *console();

} // namespace RecoveryScreen

BMDS_NAMESPACE_END
