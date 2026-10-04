/**
 * @file      glob.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/list.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Shell pattern matching (`case`, `${x#pattern}`) and pathname expansion.
 *
 * `*` matches any string, `?` any one character, `[abc]`, `[a-z]`,
 * `[!...]`/`[^...]` a set; `\` makes the next character literal (the
 * expander escapes quoted text this way). An unterminated `[` is literal.
 *
 * @par Thread Safety
 * Stateless; reentrant. `expandPaths()` takes the VFS's lock while it reads
 * each directory.
 */
namespace Glob {

        /** @brief Returns true if @p pattern matches all of @p text. */
        bool match(StringView pattern, StringView text);

        /**
         * @brief Returns true if @p pattern contains an unescaped `*`, `?` or
         *        bracket expression (a `[` with its `]`).
         */
        bool hasMagic(StringView pattern);

        /**
         * @brief Length of the prefix of @p text that @p pattern matches.
         * @param longest The longest such prefix (`##`) rather than the shortest (`#`).
         * @return The length, or `StringView::NPos` if no prefix matches.
         */
        size_t matchPrefix(StringView pattern, StringView text, bool longest);

        /** @brief Like `matchPrefix()`, for a suffix (`%`, `%%`). */
        size_t matchSuffix(StringView pattern, StringView text, bool longest);

        /**
         * @brief Pathname expansion: appends the paths @p pattern matches to @p out.
         *
         * Component by component, as the shell does: each component with
         * magic is matched against the names in its directory, the others
         * are taken as they are. A name starting with `.` matches only a
         * component that starts with a literal `.` (and never `.` or `..`).
         * A trailing `/` matches directories only and is kept. Matches are
         * sorted the way `ls` sorts (case-insensitive, then bytewise).
         * Directories that can't be read contribute nothing.
         *
         * @param pattern A pattern with `hasMagic()`; relative ones are
         *                relative to `context.cwd` and stay relative.
         * @return How many paths were appended, or `NoMem`.
         * @blocking
         */
        Result<size_t> expandPaths(StringView pattern, const Vfs::Context &context, List<String> &out);

        /** @brief Sorts @p list from @p first on the way `ls` does (case-insensitive, then bytewise). */
        void sortNames(List<String> &list, size_t first);

        /** @brief The order `sortNames()` uses: true if @p a goes before @p b. */
        bool nameLess(StringView a, StringView b);

} // namespace Glob

BMDS_NAMESPACE_END
