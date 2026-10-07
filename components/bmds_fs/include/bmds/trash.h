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
 * @brief The trash (`devplan/files.md` F2, F3): on each mounted filesystem,
 *        `MOUNT/.trash/files/NAME` (what was deleted) and
 *        `MOUNT/.trash/info/NAME.trashinfo` (where it was, and when),
 *        freedesktop.org's layout. A file moves in and out by a rename.
 *
 * @par Thread Safety
 * Thread-safe as the VFS is; two programs trashing the same name at once may
 * both pick it (one rename fails: an error, nothing lost).
 */
namespace Trash {

        /** @brief The trash's directory under a mount. */
        inline constexpr StringView Directory(".trash");

        /** @brief One thing in a trash, as `list()` tells it. */
        struct Entry {
                        StringView mount; ///< Its filesystem's mount (`/boot`).
                        StringView name;  ///< Its name in the trash (`restore()`'s).
                        StringView path;  ///< Where it was.
                        StringView date;  ///< When it was deleted (`YYYY-MM-DDThh:mm:ss`; may be empty).
                        bool       isDirectory = false;
        };

        /** @brief Told of each entry (`list()`). */
        using Listed = void (*)(void *arg, const Entry &entry);

        /**
         * @brief Moves @p path (a file or a directory) into its filesystem's trash.
         * @param name [out] Its name there (may be null).
         * @return `Ok`, `NotSupported` (not on a mounted filesystem), `Invalid` (a
         *         mount's root, or in the trash), `NotExist`, `Full` (no free name),
         *         or the VFS's error.
         */
        Error move(StringView path, const Vfs::Context &context, String *name);

        /**
         * @brief Tells @p fn of every entry of every mounted filesystem's trash (an
         *        info file without its file, or unreadable, skipped).
         * @return `Ok`, or the VFS's error.
         */
        Error list(Listed fn, void *arg);

        /**
         * @brief Puts @p name, in @p mount's trash, back where it was.
         * @return `Ok`, `NotExist` (no such entry), `Exists` (something is there
         *         now), or the VFS's error (its directory gone: `NotExist`).
         */
        Error restore(StringView mount, StringView name, const Vfs::Context &context);

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
