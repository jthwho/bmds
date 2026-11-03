/**
 * @file      textsyntax.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a piece of text is, for its colour (`devplan/editor.md` §5). The SDK's
 *        `BMDS_SYNTAX_...` are these numbers (`syntax_scan`): append only.
 */
enum class SyntaxStyle : uint8_t {
        Normal,
        Keyword,  ///< `if`, `class`; a heading's marks; an INI section.
        Type,     ///< `int`, `string`; a JSON key; an INI key.
        Constant, ///< `true`, `null`; a shell variable.
        Number,
        String,
        Character, ///< `'a'`.
        Comment,
        Preprocessor, ///< `#include`; a diff's `@@` lines.
        Operator,     ///< `+`, `==`, brackets.
        Function,     ///< A name followed by `(`.
        Heading,      ///< Markdown's `#` lines.
        Emphasis,     ///< Markdown's `*text*` and `**text**`.
        Added,        ///< A diff's `+` lines.
        Removed,      ///< A diff's `-` lines.
        Error,        ///< A string or comment left open where that can't be.
        Count
};

/** @brief A run of a line's bytes, `start` to `end` (past), in one style. */
struct StyleSpan {
                uint32_t    start = 0;
                uint32_t    end = 0;
                SyntaxStyle style = SyntaxStyle::Normal;
};

/**
 * @brief A language for colouring, as data (`devplan/editor.md` §5): its comments, strings,
 *        numbers and words. One scanner (`Syntax::scan()`) colours every language defined so;
 *        those a table doesn't fit (Markdown, a diff, a Makefile) give a scanner of their own.
 *
 * Word lists are sorted (byte order) for a binary search, and checked so by the host tests.
 * The languages are built in (`Syntax::all()`); a file format for more is planned (§5).
 *
 * @par Thread Safety
 * Immutable: safe from any task.
 */
struct Syntax {
                /** @brief A language's own scanner: as `scan()`. */
                using Scanner = uint16_t (*)(const Syntax &syntax, StringView line, uint16_t state, StyleSpan *spans,
                                             size_t max, size_t &count);

                /** @brief `quotes` bits. */
                enum Quote : uint8_t {
                        DoubleQuoted = 1u << 0, ///< `"..."` is a string.
                        SingleString = 1u << 1, ///< `'...'` is a string (the shell's).
                        SingleChar = 1u << 2,   ///< `'.'` is a character (C's).
                        BackQuoted = 1u << 3,   ///< `` `...` `` is a string (the shell's, Markdown's code).
                };

                /** @brief `flags` bits. */
                enum Flag : uint16_t {
                        CaseInsensitive = 1u << 0, ///< Words match in any case.
                        CNumbers = 1u << 1,        ///< `0x1f`, `0b101`, `1.5e3f`, `10u`.
                        HashComment =
                                1u << 2, ///< `#` starts a comment only after a space or at the start (the shell's).
                        ShellVariables = 1u << 3, ///< `$NAME`, `${...}`, `$(...)`'s `$`.
                        Functions = 1u << 4,      ///< A word before `(` is a function.
                        JsonKeys = 1u << 5,       ///< A string before `:` is a key (Type).
                        IniSections = 1u << 6,    ///< `[section]` lines and `key=value`.
                };

                const char        *id = "";            ///< `angelscript`, `cpp`: settings and files name it.
                const char        *name = "";          ///< "AngelScript": menus show it.
                const char *const *patterns = nullptr; ///< File names it is for (`*.as`, `Makefile`), null-ended.
                const char *const *shebangs = nullptr; ///< Interpreters a `#!` line names (`angel`, `sh`), null-ended.
                const char        *lineComment = nullptr;
                const char        *blockStart = nullptr;
                const char        *blockEnd = nullptr;
                const char        *longString =
                        nullptr; ///< A string that may span lines, its own delimiter at both ends (`"""`).
                char                    escape = '\\';     ///< In strings; 0: none.
                char                    preprocessor = 0;  ///< A line starting with it is a directive (`#`).
                uint8_t                 quotes = 0;        ///< `Quote` bits.
                uint16_t                flags = 0;         ///< `Flag` bits.
                Span<const char *const> keywords;          ///< Sorted.
                Span<const char *const> types;             ///< Sorted.
                Span<const char *const> constants;         ///< Sorted.
                Scanner                 scanner = nullptr; ///< Its own, instead of the table's.
                uint8_t                 indent = 8;        ///< Spaces an indentation level is, by default.
                bool                    tabs = false;      ///< Indents with tabs (a Makefile's recipes must).

                /**
                 * @brief Colours @p line, which starts in @p state (the line before's end state;
                 *        0 for the first): up to @p max spans into @p spans, in order, not
                 *        overlapping (bytes in none are `Normal`), their number in @p count.
                 * @return The state the line ends in (in a block comment, a long string...).
                 * @hotpath
                 */
                uint16_t scan(StringView line, uint16_t state, StyleSpan *spans, size_t max, size_t &count) const;

                /** @brief Returns true if @p word is in @p list (sorted), as this language matches case. */
                bool contains(Span<const char *const> list, StringView word) const;

                /** @brief Returns the built-in languages (plain text not among them). */
                static Span<const Syntax *const> all();

                /** @brief Returns the language with id @p id, or null. */
                static const Syntax *find(StringView id);

                /**
                 * @brief Returns the language for a file named @p path (its last component matched
                 *        against each language's patterns), else the one its first line's `#!`
                 *        names, else null (plain text).
                 */
                static const Syntax *forFile(StringView path, StringView firstLine);
};

/** @brief Scanner states every language shares (others are a scanner's own, from `FirstOwnState`). */
namespace SyntaxState {
        inline constexpr uint16_t Normal = 0;
        inline constexpr uint16_t BlockComment = 1;
        inline constexpr uint16_t LongString = 2;
        inline constexpr uint16_t FirstOwnState = 16;
} // namespace SyntaxState

BMDS_NAMESPACE_END
