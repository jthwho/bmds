/**
 * @file      flashwrites.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Holds back the writes that can wait (devplan/cfg.md §3): while the flash is erased or
 *        written, both cores' caches are off, and work that runs from flash or PSRAM stops (the
 *        audio engine: a glitch). Work that can't stop takes a hold; `/flash`'s write queue waits
 *        while any is held, unless somebody waits for it (`sync`, a restart) or it is half full.
 *
 * @par Thread Safety
 * All functions from any task.
 */
namespace FlashWrites {

        /** @brief Takes a hold (counted). @nonblocking */
        void hold();

        /** @brief Gives one back; the last tells `releases()`. @nonblocking */
        void release();

        /** @brief True while a hold is taken. @nonblocking */
        bool isHeld();

        /** @brief Released when the last hold is given back. */
        WaitList &releases();

} // namespace FlashWrites

BMDS_NAMESPACE_END
