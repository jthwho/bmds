/**
 * @file      regex.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A subset of POSIX basic regular expressions (what `grep` takes).
 *
 * Supported: literal characters, `.`, `*` (after an atom), `^` at the start,
 * `$` at the end, bracket expressions `[abc]`, `[a-z]`, `[^...]`, and `\`
 * before one of `.*[]^$\/-` to make it literal. Everything else that BRE
 * gives meaning to (`\{m,n\}`, `\(...\)`, back-references, character
 * classes like `[:alpha:]`, GNU `\<` `\w` ...) is rejected by `check()`
 * rather than matched literally, so supporting it later changes no
 * existing pattern's meaning.
 *
 * Matching is a small backtracking matcher: no allocation, recursion depth
 * bounded by the pattern length.
 *
 * @par Thread Safety
 * Stateless; reentrant.
 */
namespace Regex {

        /**
         * @brief Checks that @p pattern is well-formed and supported.
         * @return `Ok`, `Invalid` (malformed: unterminated `[`, trailing `\`),
         *         or `NotSupported`.
         */
        Error check(StringView pattern);

        /**
         * @brief Returns true if @p pattern matches anywhere in @p text.
         * @param pattern    A pattern that passed `check()`.
         * @param text       The text (one line, without its newline).
         * @param ignoreCase Compare letters case-insensitively (ASCII).
         */
        bool search(StringView pattern, StringView text, bool ignoreCase);

} // namespace Regex

BMDS_NAMESPACE_END
