/**
 * @file      trashindex.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The names `/trash` shows (devplan/filebrowser.md §4k, FB20, FB21).
 */

#pragma once

#include <cstdint>
#include <bmds/span.h>
#include <bmds/stringview.h>

#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief Every mounted filesystem's trash entries by the flat names `/trash`
 *        shows: an entry's name in its own trash, or, when an earlier
 *        filesystem (by mount path) has that name, the name with
 *        ` (LABEL)` before its extension.
 *
 * Built from the trashes' info folders when first needed, again after a
 * change in one (`changedLocked()`) or a mount (`mountsChangedLocked()`).
 * In PSRAM; nothing until `/trash` is used.
 *
 * @par Thread Safety
 * Everything with the storage lock held.
 */
namespace TrashIndex {

        /** @brief A mount slot, as the VFS's table holds it. */
        struct Mount {
                        StringView path;             ///< Its mount path.
                        Volume    *volume = nullptr; ///< Its filesystem.
        };

        /** @brief A trash's folder of trashed files, within its volume. */
        inline constexpr StringView FilesRest("/.bmds/trash/files");

        /** @brief A trash's folder of info files, within its volume. */
        inline constexpr StringView InfoRest("/.bmds/trash/info");

        /** @brief An info file's name: the entry's, then this. */
        inline constexpr StringView InfoSuffix(".trashinfo");

        /** @brief The mounts changed (@p mounts, by slot): the names dropped, the trashes' ids made again. */
        void mountsChangedLocked(Span<const Mount> mounts);

        /**
         * @brief Something in folder @p folder (`FileInfo::id`) changed.
         * @return True if it is a trash's files or info folder (the names are
         *         made again when next needed; `/trash`'s watches are to be told).
         */
        bool changedLocked(uint64_t folder);

        /**
         * @brief Finds flat name @p flat.
         * @param slot [out] Its mount slot.
         * @param real [out] Its name in its own trash (valid until the next change).
         * @return True if there is one.
         */
        bool findLocked(Span<const Mount> mounts, StringView flat, size_t &slot, StringView &real);

        /**
         * @brief Returns the flat name of @p real in mount @p slot's trash (valid
         *        until the next change), or an empty view: no info file names it.
         */
        StringView flatNameLocked(Span<const Mount> mounts, size_t slot, StringView real);

} // namespace TrashIndex

BMDS_NAMESPACE_END
