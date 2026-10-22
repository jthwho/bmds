/**
 * @file      codeedit.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/list.h>
#include <bmds/string.h>
#include <bmds/syntax.h>
#include <bmds/textedit.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A code editor (`devplan/editor.md` §6): a `TextEdit` with a language's colours, line
 *        numbers, the bracket matching the one at the cursor, indentation that follows the
 *        brackets, comments toggled, and diagnostics shown in place.
 *
 * Colours come from a `Syntax` (null: plain text) and a `Scheme`. Each line's scanner state is
 * kept in the document (`TextDocument::lineState()`): an edit rescans from its first line only
 * as far as the states change, and the lines before the first drawn are scanned once.
 *
 * **Keys beyond `TextEdit`'s:** Enter indents a level after an opening bracket (and puts a
 * closing one at the cursor on a line of its own); a closing bracket typed first on a line
 * lines up with its opening one's line; Ctrl+/ comments or uncomments the selected lines.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class CodeEdit : public TextEdit {
        public:
                /** @brief Colours for each style and the editor's parts, 0xRRGGBB. */
                struct Scheme {
                                const char *id = "";   ///< `classic`, `qbasic`, `dark`: `editor/scheme`.
                                const char *name = ""; ///< Menus show it.
                                uint32_t    style[size_t(SyntaxStyle::Count)] = {};
                                uint8_t     bold = 0; ///< Bit per style (Heading's, Emphasis's): drawn bold.
                                uint32_t    background = 0xffffff;
                                uint32_t    currentLine = 0xffffff;
                                uint32_t    selectionText = 0xffffff;
                                uint32_t    selectionBackground = 0x000080;
                                uint32_t    cursor = 0;
                                uint32_t    gutterBackground = 0xe0e0e0;
                                uint32_t    gutterText = 0x808080;
                                uint32_t    gutterCurrent = 0;
                                uint32_t    bracket = 0xc0d8ff; ///< A matched bracket's background.
                                uint32_t    error = 0xff0000;   ///< An error's underline and mark.
                                uint32_t    warning = 0xe0a000; ///< A warning's.
                };

                /** @brief How bad a diagnostic is. */
                enum class Severity : uint8_t {
                        Error,
                        Warning,
                        Note,
                };

                /** @brief Something said about a part of the text (a compiler's error). */
                struct Diagnostic {
                                uint32_t line = 0;  ///< From 0.
                                uint32_t start = 0; ///< Its first byte.
                                uint32_t end =
                                        0; ///< Past its last (start: the rest of the word, at least one character).
                                Severity severity = Severity::Error;
                                String   message{MemSpace::Psram};
                };

                /** @brief The colour schemes built in (the first is the default). */
                static Span<const Scheme> schemes();
                /** @brief Returns the scheme with id @p id, or null. */
                static const Scheme *scheme(StringView id);

                explicit CodeEdit(Widget *parent);
                ~CodeEdit() override;

                /** @brief Colours the text as @p syntax (null: plain text). */
                void          setSyntax(const Syntax *syntax);
                const Syntax *syntax() const { return _syntax; }
                /** @brief Draws in @p scheme. */
                void          setScheme(const Scheme &scheme);
                const Scheme &currentScheme() const { return _scheme; }
                /**
                 * @brief Colours these words as types as well as the language's own (an API's
                 *        types, a script's classes): sorted, as `Syntax`'s lists.
                 */
                void setTypeNames(List<String> &&names);

                /** @brief Shows line numbers in the gutter, or not. */
                void setLineNumbers(bool on);
                bool hasLineNumbers() const { return _lineNumbers; }

                /** @brief Shows @p diagnostics (replacing those shown); none: clears them. */
                Error setDiagnostics(Span<const Diagnostic> diagnostics);
                /** @brief Returns the diagnostics shown. */
                Span<const Diagnostic> diagnostics() const { return _diagnostics.span(); }
                /** @brief Moves the cursor to the next diagnostic after it (or before it, @p backward), wrapping. @return It, or null. */
                const Diagnostic *nextDiagnostic(bool backward);
                /** @brief Returns the diagnostic at @p position, or null. */
                const Diagnostic *diagnosticAt(TextPosition position) const;

                /** @brief Comments the selected lines (or the cursor's) with the language's line comment, or uncomments them if all are. */
                void toggleComment();

                StringView toolTip() const override;
                void       mouseMoved(const WindowManager::MouseEvent &event) override;

        protected:
                size_t   styleLine(uint32_t line, StringView text, StyleRun *runs, size_t max) override;
                uint16_t gutterWidth() const override;
                void     paintGutter(Painter &painter, uint32_t line, const Rect &row) override;
                uint32_t lineBackground(uint32_t line) const override;
                bool     editKey(const KeyEvent &event) override;
                void     typed(StringView text) override;
                void     documentChanged(const TextDocument::Change &change) override;
                void     paintOver(Painter &painter, uint32_t line, int y, const Rect &area) override;
                void     cursorMovedHook() override;

        private:
                void        ensureStates(uint32_t line); // the end states of the lines before @p line known
                uint16_t    startState(uint32_t line);
                SyntaxStyle styleAt(uint32_t line, uint32_t byte); // for brackets inside strings and comments
                bool        findMatch(TextPosition at, TextPosition &match);
                void        matchBrackets();
                void        newline();
                void        lineUpClosing();
                String      indentUnit() const;
                String      indentationOf(uint32_t line) const;
                uint32_t    digits() const;
                bool        hasDiagnostic(uint32_t line, Severity &worst) const;

                size_t addTypeNames(StringView text, StyleSpan *spans, size_t count, size_t max) const;

                const Syntax    *_syntax = nullptr;
                List<String>     _typeNames{MemSpace::Psram};
                Scheme           _scheme;
                bool             _lineNumbers = true;
                uint32_t         _digits = 1;     // the line numbers' width, as laid out
                uint32_t         _validUpTo = 0;  // lines before it have their end states
                uint32_t         _resyncFrom = 0; // after an edit, from here...
                uint32_t         _resyncEnd = 0;  // ...to here the old states hold, if the start does
                TextPosition     _bracketA;
                TextPosition     _bracketB;
                bool             _bracketsShown = false;
                List<Diagnostic> _diagnostics{MemSpace::Psram};
                TextPosition     _hover; // the pointer's place, for the tooltip
                bool             _hovering = false;
};

BMDS_NAMESPACE_END
