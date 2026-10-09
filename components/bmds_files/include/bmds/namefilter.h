/**
 * @file      namefilter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A file dialog's "Files of type" patterns (`devplan/mediaplayer.md`
 *        §6d): `*.mp3;*.ogg`, matched against file names.
 *
 * Patterns are separated by `;` (spaces around them ignored). `*` matches any
 * run of characters and `?` any one; the rest match without ASCII case, as
 * FAT names do. A UTF-8 character is several bytes, each matched by `?` or
 * itself, so `?` may match part of one. No patterns matches every name;
 * `NoFiles` matches none (a name never holds a `/`).
 *
 * @par Thread Safety
 * Stateless; reentrant.
 */
namespace NameFilter {

        /** @brief Patterns that match no name (choosing a folder). */
        inline constexpr StringView NoFiles("/");

        /** @brief Returns true if @p name matches one of @p patterns (or there are none). */
        bool matches(StringView patterns, StringView name);

        /** @brief Returns true if @p pattern (one) matches all of @p name. */
        bool matchesOne(StringView pattern, StringView name);

        /** @brief Returns true if @p text holds a `*` or a `?`. */
        bool hasWildcard(StringView text);

        /**
         * @brief Returns the extension of @p patterns' first `*.ext` pattern
         *        (`mp3`), what a name typed without one is given; empty if the
         *        first pattern isn't one (`*`, `*.*`, `a*`).
         */
        StringView firstExtension(StringView patterns);

} // namespace NameFilter

BMDS_NAMESPACE_END
