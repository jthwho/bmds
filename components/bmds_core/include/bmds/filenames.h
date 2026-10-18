/**
 * @file      filenames.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A copy's name, as file managers give it (`devplan/files.md` F1):
 *        `NAME (copy).EXT`, then `NAME (copy 2).EXT`, `(copy 3)`...
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace CopyName {

        /** @brief The highest number tried. */
        inline constexpr unsigned MaxCopies = 999;

        /**
         * @brief Writes copy @p n (1: ` (copy)`; 2 and up: ` (copy N)`) of @p name
         *        into @p out (NUL-ended), before its extension (from its last `.`
         *        that isn't its first character).
         * @return The name (a view of @p out), `Invalid` (@p n 0 or over
         *         `MaxCopies`, @p name empty or holding `/`), or `BufferTooSmall`.
         */
        Result<StringView> make(StringView name, unsigned n, Span<char> out);

} // namespace CopyName

/**
 * @brief Paths in URIs: `file://` lists (`text/uri-list`), the trash's info
 *        files.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Uri {

        /**
         * @brief Writes @p path into @p out percent-encoded (`%XX` for each byte but
         *        RFC 3986's unreserved characters and `/`), NUL-ended.
         * @return The length, or `BufferTooSmall`.
         */
        Result<size_t> encodePath(StringView path, Span<char> out);

        /**
         * @brief Writes @p text into @p out with its `%XX` decoded (a `%` not
         *        followed by two hex digits kept), NUL-ended.
         * @return The length, or `BufferTooSmall`.
         */
        Result<size_t> decode(StringView text, Span<char> out);

} // namespace Uri

/**
 * @brief A trash info file (`devplan/files.md` F3; freedesktop.org's Trash
 *        specification): `[Trash Info]`, `Path=`, `DeletionDate=`.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace TrashInfo {

        /** @brief `DeletionDate`'s length: `YYYY-MM-DDThh:mm:ss`. */
        inline constexpr size_t DateBytes = 19;

        /**
         * @brief Writes the info of @p path (relative to its filesystem's root,
         *        freedesktop.org's rule for a volume's trash; or absolute) deleted
         *        at @p when (local time) into @p out.
         * @return The text (a view of @p out), or `BufferTooSmall`.
         */
        Result<StringView> format(StringView path, const DateTime &when, Span<char> out);

        /**
         * @brief Reads @p text: the original path (absolute, or relative to the
         *        trash's filesystem), decoded, into @p path (NUL-ended), and the date into @p date (`DateBytes` + 1, NUL-ended;
         *        empty when missing).
         * @return `Ok`, `CorruptData` (not a trash info, or no `Path`), or
         *         `BufferTooSmall`.
         */
        Error parse(StringView text, Span<char> path, char (&date)[DateBytes + 1]);

} // namespace TrashInfo

BMDS_NAMESPACE_END
