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

#include <sys/types.h> // newlib's regex.h needs off_t first
#include <regex.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A POSIX regular expression, basic (what `grep` takes) or extended (`grep -E`): the C
 *        library's `regcomp()` and `regexec()` (newlib's on the board), compiled once and matched
 *        line after line.
 *
 * Basic: `.`, `*`, `^`, `$`, brackets with ranges and classes (`[[:alpha:]]`), `\(...\)`,
 * `\{m,n\}`, back-references. Extended adds `+`, `?`, `|`, `(...)` and `{m,n}` unescaped.
 *
 * @par Thread Safety
 * One object per thread; matching does not change it.
 */
class Regex {
        public:
                /** @brief compile()'s flags. */
                enum Flag : unsigned int {
                        Basic = 0,      ///< A basic expression (BRE).
                        Extended = 1,   ///< An extended expression (ERE).
                        IgnoreCase = 2, ///< Letters compared case-insensitively.
                };

                Regex() = default;
                ~Regex();
                Regex(const Regex &) = delete;
                Regex &operator=(const Regex &) = delete;

                /**
                 * @brief Compiles @p pattern (any earlier one freed).
                 * @return `Ok`, `Invalid` (malformed: an unterminated `[`, a trailing `\`, ...) with
                 *         its reason in errorText(), or `NoMem`.
                 */
                Error compile(StringView pattern, unsigned int flags);

                /**
                 * @brief Returns true if the compiled pattern matches anywhere in @p text (one line,
                 *        without its newline, NUL-terminated). False before a compile().
                 */
                bool search(const char *text) const;

                /** @brief Why the last compile() failed ("" if it didn't). */
                const char *errorText() const { return _error; }

        private:
                regex_t _re{};
                bool    _compiled = false;
                char    _error[64] = {};
};

BMDS_NAMESPACE_END
