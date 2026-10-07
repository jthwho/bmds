/**
 * @file      panelentries.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/desktopentry.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/** @brief A Desktop Entry file read: the launcher's or the desktop's (`devplan/gui.md` §4c, §4m). */
struct PanelEntry {
                DesktopEntry entry;
                char         file[48] = {}; ///< Its file's name: the first of a name wins.
};

/** @brief Told of a file that isn't a Desktop Entry (@p path). */
using PanelEntryReport = void (*)(void *arg, const char *path);

/**
 * @brief Adds directory @p directory's `NAME.desktop` files to @p entries (@p count of
 *        @p max used), but those of a name already there; hidden ones too (they hide
 *        later ones of their name).
 * @return False if the directory isn't there (no card yet: no error).
 */
bool readEntryDirectory(StringView directory, const Vfs::Context &context, PanelEntry *entries, size_t &count,
                        size_t max, PanelEntryReport report, void *arg);

/** @brief Leaves out @p entries' hidden and not-shown ones, and sorts the rest by name. */
void keepShownEntries(PanelEntry *entries, size_t &count);

BMDS_NAMESPACE_END
