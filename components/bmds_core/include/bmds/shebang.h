/**
 * @file      shebang.h
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
 * @brief A script's `#!` line, split as Linux does: the interpreter, then
 *        the rest of the line (trimmed) as at most one argument.
 *
 * Shared by the shell (running a script by name) and the loader (`exec`
 * of a script): the rules are an interface (devplan/programs.md §10).
 *
 * @par Thread Safety
 * Stateless; the views point into the head given to `parse()`.
 */
struct Shebang {
                /** @brief Bytes of a file looked at: the line must end within them. */
                static constexpr size_t HeadBytes = 256;

                StringView interpreter; ///< The interpreter's path.
                StringView argument;    ///< Its one argument, or empty.

                /** @brief Returns true if @p head (a file's first bytes) starts with `#!`. */
                static bool isScript(StringView head) { return head.startsWith(StringView("#!")); }

                /**
                 * @brief Splits the `#!` line at the start of @p head.
                 * @param head  The file's first bytes (starting with `#!`).
                 * @param whole True if @p head is the whole file (a last line
                 *              without a newline then counts).
                 * @param out   [out] The interpreter and its argument; CRs and
                 *              blanks around them are dropped (CR LF scripts).
                 * @return False if the line doesn't end within @p head.
                 */
                static bool parse(StringView head, bool whole, Shebang &out) {
                        StringView   line = head.mid(2);
                        const size_t newline = line.indexOf('\n');
                        if (newline != StringView::NPos) {
                                line = line.left(newline);
                        } else if (!whole) {
                                return false;
                        }
                        line = line.trimmed();
                        size_t end = 0;
                        while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
                        out.interpreter = line.left(end);
                        out.argument = line.mid(end).trimmed();
                        return true;
                }
};

BMDS_NAMESPACE_END
