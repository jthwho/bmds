/**
 * @file      arith.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/stringview.h>
#include <bmds/variables.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Shell arithmetic (`$((...))`, `((...))`, `for ((...))`).
 *
 * 64-bit signed integers that wrap on overflow (as bash's), bash's operators and
 * precedence: `, = += -= *= /= %= <<= >>= &= ^= |= ?: || && | ^ & == != < <=
 * > >= << >> + - * / % ** ! ~ ++ --` and parentheses. Numbers are decimal,
 * `0x` hex, `0` octal or `base#digits`. A name is a variable, whose value is
 * itself evaluated as an expression (unset or empty is 0). `&&`, `||` and
 * `?:` skip the side effects of the operand they don't evaluate.
 *
 * The caller expands `$...` in the expression first, as bash does.
 *
 * @par Thread Safety
 * Reentrant; uses only @p vars.
 */
namespace Arith {

        /** @brief Why an expression failed. */
        struct Failure {
                        const char *message = nullptr; ///< E.g. "division by 0".
                        StringView  token;             ///< The rest of the expression at the error.
        };

        /**
         * @brief Evaluates @p expr.
         * @return The value; `Invalid` (see @p failure), `ReadOnly` (assigned a
         *         read-only variable) or `NoMem`.
         */
        Result<int64_t> evaluate(StringView expr, Variables &vars, Failure &failure);

} // namespace Arith

BMDS_NAMESPACE_END
