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
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Shell pattern matching (`case`, `${x#pattern}`; pathnames in Phase 2).
 *
 * `*` matches any string, `?` any one character, `[abc]`, `[a-z]`,
 * `[!...]`/`[^...]` a set; `\` makes the next character literal (the
 * expander escapes quoted text this way). An unterminated `[` is literal.
 *
 * @par Thread Safety
 * Stateless; reentrant.
 */
namespace Glob {

        /** @brief Returns true if @p pattern matches all of @p text. */
        bool match(StringView pattern, StringView text);

        /** @brief Returns true if @p text contains `*`, `?` or `[` unescaped. */
        bool hasMagic(StringView pattern);

        /**
         * @brief Length of the prefix of @p text that @p pattern matches.
         * @param longest The longest such prefix (`##`) rather than the shortest (`#`).
         * @return The length, or `StringView::NPos` if no prefix matches.
         */
        size_t matchPrefix(StringView pattern, StringView text, bool longest);

        /** @brief Like `matchPrefix()`, for a suffix (`%`, `%%`). */
        size_t matchSuffix(StringView pattern, StringView text, bool longest);

} // namespace Glob

BMDS_NAMESPACE_END
