/**
 * @file      desktopmenu.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class CommandContext;
class WindowManager;

/**
 * @brief The window manager's desktop menu as `win start` gives it
 *        (`devplan/gui.md` §4o): the items of `/cfg/config/wm.conf`, run as
 *        the panel runs a launcher entry, with what @p ctx's command had (its
 *        exported environment and directory, copied) and standard files on
 *        `/dev/null`.
 *
 * @par Thread Safety
 * `win start`'s, once the window manager started (it tells no choice before).
 */
Error installDesktopMenu(WindowManager &wm, CommandContext &ctx);

/**
 * @brief Runs @p command (a command line, as a menu item's) as the desktop menu runs its items:
 *        a window's own menu item's (`WindowManager::setWindowItem()`).
 * @return `Ok` (started on a thread of its own: its errors are logged), `NotReady` (no desktop
 *         menu: `win start` hasn't run), `Busy` (another is being started), `Invalid`.
 */
Error runDesktopCommand(StringView command);

BMDS_NAMESPACE_END
