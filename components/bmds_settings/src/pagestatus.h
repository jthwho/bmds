/**
 * @file      pagestatus.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/rect.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a page shows of the system's state over its settings
 *        (`devplan/controlpanel.md` §7), read-only, from the kernel's public
 *        classes.
 *
 * @par Thread Safety
 * Any task (each read takes what it reads under its owner's lock).
 */
namespace PageStatus {

        /** @brief Returns true if page @p id shows a status. */
        bool has(StringView id);

        /** @brief Returns true if page @p id's status changes by itself (read again each second). */
        bool ticks(StringView id);

        /**
         * @brief Writes page @p id's status into @p out, lines between LFs; @p screen
         *        is the window manager's screen (the Display page's).
         */
        void text(StringView id, const Rect &screen, String &out);

        /** @brief Returns the first interface with a DHCP lease (Renew Lease's), or -1. */
        int leasedInterface();

} // namespace PageStatus

BMDS_NAMESPACE_END
