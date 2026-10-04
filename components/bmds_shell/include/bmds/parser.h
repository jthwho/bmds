/**
 * @file      parser.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/arena.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/stringview.h>
#include <bmds/syntax.h>

BMDS_NAMESPACE_BEGIN

/** @brief Why source didn't parse. Render with `appendSyntaxError()`. */
struct SyntaxError {
                /** @brief The message; a `%.*s` in it (at most one) stands for `token`. */
                const char *format = nullptr;
                StringView  token;      ///< The offending token, if the message names one.
                size_t      offset = 0; ///< Where in the source.
                unsigned    line = 1;   ///< 1-based line of `offset`.
};

/**
 * @brief Appends the message of @p error to @p out, worded as bash words it,
 *        e.g. "syntax error near unexpected token `fi'".
 * @return `Ok` or `NoMem`.
 */
Error appendSyntaxError(const SyntaxError &error, String &out);

/**
 * @brief Lexes @p text as the inside of double quotes (expansions, backslash
 *        before `$` `` ` `` `\`), for arithmetic and other re-expanded text.
 * @param text  The text; must outlive the word.
 * @param arena Where the word is allocated.
 * @param error [out] Set on `Invalid`.
 * @return The word (all parts quoted); `Invalid` (an unterminated `$(`...) or `NoMem`.
 */
Result<Word *> parseQuotedText(StringView text, Arena &arena, SyntaxError &error);

/**
 * @brief Parses shell source (the bash subset in devplan/shell-language.md).
 *
 * `next()` returns one complete command at a time, as bash reads them: a
 * list up to an unquoted newline, together with any compound command or
 * here-document it opens. A caller can run each command before parsing the
 * next, so a later syntax error doesn't stop earlier commands (as in bash).
 *
 * Input that stops in the middle of a command (an open quote or `if`, a
 * trailing `|` or `&&`, a here-document without its delimiter, a trailing
 * backslash) gives `Error::NotReady`: the interactive shell then prompts
 * for more (`PS2`) and parses again with the longer text.
 *
 * Nodes are allocated from @p arena; text in the tree points into
 * @p source. Command substitutions and function bodies are kept as source
 * text and parsed again when they run.
 *
 * Recursion is bounded by `MaxDepth` (an error, not a stack overflow).
 *
 * @par Thread Safety
 * Not thread-safe; one parser per task.
 */
class Parser {
        public:
                /** @brief Deepest nesting of commands accepted. */
                static constexpr unsigned MaxDepth = 64;

                /**
                 * @brief Prepares to parse @p source.
                 * @param source The text; must outlive the parser and the trees.
                 * @param arena  Where nodes are allocated.
                 */
                Parser(StringView source, Arena &arena);

                Parser(const Parser &) = delete;
                Parser &operator=(const Parser &) = delete;

                /**
                 * @brief Parses the next complete command.
                 * @return The command; null at the end of the source; or
                 *         `Invalid` (see `error()`), `NotReady`, or `NoMem`.
                 */
                Result<Node *> next();

                /** @brief Returns the last syntax error. */
                const SyntaxError &error() const { return _error; }

                /** @brief Returns the offset just past the last command returned by `next()`. */
                size_t position() const { return _end; }

        private:
                friend class ParserImpl;

                StringView  _source;
                Arena      &_arena;
                size_t      _end = 0;
                SyntaxError _error;
};

BMDS_NAMESPACE_END
