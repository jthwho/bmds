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

/**
 * @brief A Desktop Entry file read: the launcher's or the desktop's (`devplan/gui.md` §4c, §4m);
 *        or, on the desktop, any other file (`devplan/filetypes.md` T10): its type, its name.
 */
struct PanelEntry {
                DesktopEntry entry;          ///< Another file's: its name only.
                char         file[48] = {};  ///< Its file's name: the first of a name wins.
                char         path[128] = {}; ///< Its file's path (copied as the icon: devplan/clipboard.md §4f).
                char         type[48] = {};  ///< Another file's type (`Vfs::identify()`); empty: a Desktop Entry.

                /** @brief Returns true if it is a Desktop Entry (a launcher), not another file. */
                bool isLauncher() const { return type[0] == '\0'; }
};

/** @brief Told of a file that isn't a Desktop Entry (@p path). */
using PanelEntryReport = void (*)(void *arg, const char *path);

/**
 * @brief Adds directory @p directory's `NAME.desktop` files to @p entries (@p count of
 *        @p max used), but those of a name already there; hidden ones too (they hide
 *        later ones of their name). With @p allFiles (the desktop's), every other file and
 *        directory too, but those named with a leading `.`, each with its type.
 *
 *        `/apps` (`Vfs::AppsPath`) is the installed applications (`devplan/apps.md` §4): each
 *        link's folder's `app.desktop`, named `ID.desktop` (an entry of that name in an earlier
 *        directory overrides or hides it), its `Exec` run through `/apps/COMMAND`.
 * @return False if the directory isn't there (no card yet: no error).
 */
bool readEntryDirectory(StringView directory, const Vfs::Context &context, PanelEntry *entries, size_t &count,
                        size_t max, PanelEntryReport report, void *arg, bool allFiles = false);

/**
 * @brief Reads application folder @p folder's `app.desktop` (`devplan/apps.md` §3) into
 *        @p entry, its `Exec`'s program replaced by @p program (empty: the folder's own, `Exec`'s
 *        first word in it), its arguments kept.
 * @return False if there's none to use (no file, not an entry, hidden, `Exec` outside the folder).
 */
bool readAppFolder(StringView folder, const Vfs::Context &context, StringView program, DesktopEntry &entry);

/** @brief Leaves out @p entries' hidden and not-shown ones, and sorts the rest by name. */
void keepShownEntries(PanelEntry *entries, size_t &count);

BMDS_NAMESPACE_END
