/**
 * @file      wmconfig.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_ui (target): the window manager's palette in the system
 * configuration, `wm/palette/<role>` (devplan/sysconf.md §8).
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>
#include <bmds/windowmanager.h>
#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

namespace WmConfig {

        /** @brief The palette the system configuration holds now. */
        WindowPalette palette();

        /** @brief Has @p wm's palette follow the system configuration's from now on. */
        Error follow(WindowManager &wm);

        /** @brief Writes `wm/palette/<role>` for role @p name into @p key (false: no such role). */
        bool keyOf(StringView name, char (&key)[48]);

} // namespace WmConfig

BMDS_NAMESPACE_END
