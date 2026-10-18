/**
 * @file      trash.h
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
#include <bmds/filenames.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The trash (`devplan/files.md` F2, F3; `devplan/filebrowser.md` §4k):
 *        on each mounted filesystem, `MOUNT/.bmds/trash/files/NAME` (what was
 *        deleted) and `MOUNT/.bmds/trash/info/NAME.trashinfo` (where it was,
 *        relative to the filesystem's root, and when), freedesktop.org's
 *        layout. A file moves in and out by a rename. `/trash` shows every
 *        trash as one folder (`Vfs::TrashPath`); entries are named here by
 *        their names there.
 *
 * @par Thread Safety
 * Thread-safe as the VFS is; two programs trashing the same name at once may
 * both pick it (one rename fails: an error, nothing lost).
 */
namespace Trash {

        /** @brief The trash's directory under a mount. */
        inline constexpr StringView Directory(".bmds/trash");

        /** @brief One thing in a trash, as `list()` and `find()` tell it. */
        struct Entry {
                        String mount; ///< Its filesystem's mount (`/flash`).
                        String name;  ///< Its name in `/trash` (`restore()`'s).
                        String real;  ///< Its name in its own trash's `files`.
                        String path;  ///< Where it was: absolute, decoded.
                        String date;  ///< When it was deleted (`YYYY-MM-DDThh:mm:ss`; may be empty).
                        bool   isDirectory = false;
        };

        /** @brief Told of each entry (`list()`). */
        using Listed = void (*)(void *arg, const Entry &entry);

        /** @brief What `restore()` does when something is at the entry's old place. */
        enum class Conflict : uint8_t {
                Refuse,   ///< `Exists`.
                Replace,  ///< As a rename that replaces: a file a file, a folder an empty one.
                KeepBoth, ///< Back under a copy's name (`NAME (copy).EXT`, `CopyName`).
        };

        /**
         * @brief Moves @p path (a file or a directory) into its filesystem's trash,
         *        under its own name, else the first of `NAME (2)`, `NAME (3)`...
         *        free there and in `/trash`.
         * @param name [out] Its name there (may be null).
         * @return `Ok`, `NotSupported` (not on a mounted filesystem), `Invalid` (a
         *         mount's root, or in `.bmds`), `NotExist`, `Full` (no free name),
         *         or the VFS's error.
         */
        Error move(StringView path, const Vfs::Context &context, String *name);

        /** @brief `move()`, named @p as in the trash (else as its own name is). */
        Error moveAs(StringView path, StringView as, const Vfs::Context &context, String *name);

        /**
         * @brief Tells @p fn of every entry of every mounted filesystem's trash, as
         *        `/trash` lists them (an info file without its file, or unreadable,
         *        skipped).
         * @return `Ok`, or the VFS's error.
         */
        Error list(Listed fn, void *arg);

        /**
         * @brief Describes entry @p name (its name in `/trash`) into @p entry.
         * @return `Ok`, `NotExist`, `CorruptData` (its info unreadable), or the VFS's error.
         */
        Error find(StringView name, Entry &entry);

        /**
         * @brief Puts entry @p name (its name in `/trash`) back where it was,
         *        making the folders on the way that are gone.
         * @param restoredTo [out] Where it went (may be null).
         * @return `Ok`, `NotExist` (no such entry), `Exists` (something is there,
         *         with `Refuse`), `Full` (no copy's name free), or the VFS's error.
         */
        Error restore(StringView name, Conflict conflict, String *restoredTo);

        /**
         * @brief Empties @p mount's trash, or every one (@p mount empty).
         * @param removed [out] Entries removed (may be null).
         * @return `Ok`, `NotExist` (no such mount), or the VFS's first error.
         */
        Error empty(StringView mount, size_t *removed);

        /**
         * @brief Writes the mount of @p path (normalized, absolute) into @p mount.
         * @return `Ok`, or `NotSupported` (not on a mounted filesystem).
         */
        Error mountOf(StringView path, String &mount);

} // namespace Trash

BMDS_NAMESPACE_END
