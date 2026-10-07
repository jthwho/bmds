/**
 * @file      fileformat.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a file is, as a MIME type (`devplan/filetypes.md` T1-T5): by its
 *        name, or by its first bytes. Pure: no I/O (`Vfs::identify()`
 *        reads the bytes).
 *
 * The types are freedesktop.org's names (`text/plain`, `image/png`,
 * `application/x-executable`). Every type returned is a view of a string in
 * rodata: it outlives everything.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace FileFormat {

        /** @brief The longest type, in bytes (`BMDS_FILE_TYPE_MAX`). */
        constexpr size_t MaxType = 127;

        /** @brief The bytes `byContent()` looks at, at most. */
        constexpr size_t HeadBytes = 512;

        /** @brief A directory's type. */
        constexpr StringView Directory{"inode/directory"};

        /** @brief A device's type. */
        constexpr StringView Device{"inode/chardevice"};

        /** @brief An empty file's type (text-like: `isText()`). */
        constexpr StringView Empty{"application/x-zerosize"};

        /** @brief Bytes not known to be anything. */
        constexpr StringView Binary{"application/octet-stream"};

        /** @brief Text not known to be anything more. */
        constexpr StringView PlainText{"text/plain"};

        /**
         * @brief Returns the type of a file named @p name (a path's last part is
         *        used): by its extension, from its last `.` that isn't its first
         *        character (any case), or by its whole name (`Makefile`); empty
         *        if not known.
         * @hotpath A walk of a table in rodata.
         */
        StringView byName(StringView name);

        /**
         * @brief Returns the type of a file starting with @p head (its first
         *        bytes, `HeadBytes` of them or all of it): its magic number, else
         *        text (valid UTF-8, no NUL, only the usual controls) or binary.
         *        @p whole says @p head is the whole file: an empty one is `Empty`,
         *        and a character cut at its end is a fault.
         */
        StringView byContent(Span<const uint8_t> head, bool whole);

        /** @brief Returns true if @p type is text, or text-like (JSON, a script, an empty file...). */
        bool isText(StringView type);

        /**
         * @brief Writes @p type's icon name into @p out (NUL-ended): `/` made `-`
         *        (`text/plain` is `text-plain`).
         * @return The name (a view of @p out), or empty if it doesn't fit.
         */
        StringView iconName(StringView type, Span<char> out);

        /**
         * @brief Returns the generic icon name for @p type: by its media
         *        (`text-x-generic`, `image-x-generic`...), archives
         *        `package-x-generic`, programs `application-x-executable`,
         *        directories `folder`, else `unknown`.
         */
        StringView genericIcon(StringView type);

} // namespace FileFormat

BMDS_NAMESPACE_END
