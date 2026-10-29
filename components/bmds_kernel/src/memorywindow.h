/**
 * @file      memorywindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/** @brief `Memory`'s side of the PSRAM window (`devplan/psramwindow.md` §3b). */
namespace MemoryWindow {

        /**
         * @brief Starts large blocks through the window: its records, and the heap's TLSF bitmaps
         *        found and checked. Called by `PsramWindow::init()` once the addresses are reserved.
         */
        Error init();

} // namespace MemoryWindow

BMDS_NAMESPACE_END
