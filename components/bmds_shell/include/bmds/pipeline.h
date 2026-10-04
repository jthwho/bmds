/**
 * @file      pipeline.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>

BMDS_NAMESPACE_BEGIN

/** @brief One redirection of a pipeline stage, applied left to right. */
struct Redirect {
                /** @brief What the redirection does. */
                enum class Kind : uint8_t {
                        Input,     ///< `n<path`: open @c path for reading on @c fd.
                        Output,    ///< `n>path`: open @c path for writing (truncating) on @c fd.
                        Append,    ///< `n>>path`: open @c path for appending on @c fd.
                        Duplicate, ///< `n>&m`, `n<&m`: make @c fd refer to what @c sourceFd does.
                        Close,     ///< `n>&-`, `n<&-`: close @c fd.
                };

                Kind        kind = Kind::Output;
                int         fd = 1;          ///< The descriptor being redirected.
                int         sourceFd = -1;   ///< `Duplicate`: the descriptor copied.
                const char *path = nullptr;  ///< `Input`/`Output`/`Append`: the file.
                bool        isInput = false; ///< Written with `<` (affects error wording only).
};

/** @brief One command of a pipeline: its words and redirections. */
struct Stage {
                List<char *>   args;      ///< The words; `args[0]` is the command. May be empty.
                List<Redirect> redirects; ///< In the order written.
};

/** @brief A parsed command line: stages joined by `|`. */
struct Pipeline {
                List<Stage> stages; ///< Empty for a blank or comment-only line.
};

/** @brief Why a line didn't parse. */
struct ParseError {
                const char *message = nullptr; ///< What is wrong, e.g. "syntax error near unexpected token".
                const char *token = nullptr;   ///< The offending token, or null.
};

/**
 * @brief Parses a command line into a pipeline, in place.
 *
 * Words follow the familiar rules:
 * - Spaces and tabs separate words.
 * - `"..."` quotes, with `\"` and `\\` as escapes inside; `'...'` quotes
 *   literally; outside quotes, a backslash makes the next character literal.
 *   Quotes join with adjacent text (`a"b c"d` is `ab cd`).
 * - `#` at the start of a word starts a comment.
 *
 * Unquoted operators:
 * - `|` joins stages; `|&` also sends the left stage's stderr down the pipe.
 * - `<`, `>`, `>>`, `>|`, `<&`, `>&`, with an optional descriptor number
 *   written right before them (`2>`), and `&>`, `&>>` (stdout and stderr).
 * - `;`, `&&`, `||`, `&` and here-documents are rejected for now.
 *
 * The line is rewritten so each word is NUL-terminated in place; the
 * pipeline's pointers point into it.
 *
 * @param line  A NUL-terminated line; modified, and must outlive @p out.
 * @param out   [out] The pipeline.
 * @param error [out] Set when the result is `Invalid`.
 * @return `Ok`, `Invalid` (see @p error), or `NoMem`.
 */
Error parsePipeline(char *line, Pipeline &out, ParseError &error);

BMDS_NAMESPACE_END
