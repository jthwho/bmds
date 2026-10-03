/**
 * @file      args.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Splits a command line into arguments, in place.
 *
 * Rules:
 * - Spaces and tabs separate arguments.
 * - `"..."` quotes, with `\"` and `\\` as escapes inside.
 * - `'...'` quotes literally.
 * - Outside quotes, a backslash makes the next character literal.
 * - `#` at the start of an argument starts a comment that runs to the end of the line.
 *
 * Quotes may join with surrounding text: `a"b c"d` is one argument, `ab cd`.
 *
 * The line is rewritten: each argument is moved into place and
 * NUL-terminated, and @p argv points into the line. Nothing is allocated.
 *
 * @param line A NUL-terminated line; modified.
 * @param argv [out] Receives pointers to the arguments.
 * @return The argument count. `Error::Invalid` for an unterminated quote,
 *         or `Error::Full` for more arguments than `argv.size()`.
 */
Result<size_t> tokenizeArgs(char *line, Span<char *> argv);

BMDS_NAMESPACE_END
