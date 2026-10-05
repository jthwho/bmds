/**
 * @file      terminalscreen.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/color.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memspace.h>
#include <bmds/stringview.h>
#include <bmds/vtparser.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief One character cell of a `TerminalScreen`: 12 bytes.
 *
 * A two-column character is a `wide` cell followed by a `tail` cell (whose
 * codepoint is 0). Attributes are `TextStyle::Attribute` bits, kept as the
 * program set them: the renderer applies reverse, dim and invisible.
 */
struct TerminalCell {
                uint32_t codepoint : 21 = ' ';
                uint32_t attributes : 8 = 0;
                uint32_t wide : 1 = 0; ///< The left half of a two-column character.
                uint32_t tail : 1 = 0; ///< The right half of a two-column character.
                uint32_t reserved : 1 = 0;
                Color    foreground;
                Color    background;

                bool operator==(const TerminalCell &other) const = default;
};

static_assert(sizeof(TerminalCell) == 12);

/**
 * @brief The screen of a VT/xterm terminal: a grid of cells that terminal
 *        output is applied to (`devplan/terminal.md` §3a).
 *
 * Text is decoded as UTF-8 and control sequences are carried out (the
 * xterm subset real programs use: cursor movement, erasing, inserting and
 * deleting, scroll regions, SGR colors, the alternate screen). What it
 * doesn't know is ignored. Replies (status and size reports) go to a sink
 * the owner sets.
 *
 * Rows changed since `clearDamage()` are marked dirty, so a renderer draws
 * only those. The cursor is not damage: the renderer tracks where it drew it.
 *
 * Nothing allocates after `resize()`: the cells live in one allocation in
 * the `MemSpace` given at construction.
 *
 * @par Thread Safety
 * Not thread-safe: the owner serializes writes and the renderer's reads.
 */
class TerminalScreen final : private VtHandler {
        public:
                /** @brief Receives replies to the program (status reports), for the input. */
                using ReplySink = void (*)(void *context, StringView reply);

                /** @brief Largest size `resize()` takes, either way. */
                static constexpr uint16_t MaxSide = 1024;

                /** @brief A screen of no size; `resize()` gives it one. */
                explicit TerminalScreen(MemSpace space) : _parser(*this), _space(space) {}

                ~TerminalScreen();
                TerminalScreen(const TerminalScreen &) = delete;
                TerminalScreen &operator=(const TerminalScreen &) = delete;

                /**
                 * @brief Sets the size, keeping the content's top-left. When rows
                 *        go, the top ones go first so the cursor's row stays. The
                 *        scroll region and tab stops are reset; every row is dirty.
                 * @return `Ok`, `Invalid` (0 or above `MaxSide`), or `NoMem` (the
                 *         screen is unchanged).
                 */
                Error resize(uint16_t columns, uint16_t rows);

                /** @brief Returns the width in cells. */
                uint16_t columns() const { return _columns; }

                /** @brief Returns the height in cells. */
                uint16_t rows() const { return _rows; }

                /** @brief Sets where replies go (none: they are dropped). */
                void setReplySink(ReplySink sink, void *context) {
                        _replySink = sink;
                        _replyContext = context;
                        return;
                }

                /** @brief Applies terminal output. @nonblocking */
                void write(StringView text) {
                        if (_cells != nullptr) _parser.feed(text);
                        return;
                }

                /** @brief Full reset (RIS): modes, style, both screens cleared, the primary shown. */
                void reset();

                /** @brief Returns row @p row (`columns()` cells) of the screen shown. */
                const TerminalCell *row(uint16_t row) const { return rowOf(_screen, row); }

                /** @brief Returns the cursor's column (0-based). */
                uint16_t cursorColumn() const { return _cursor.column; }

                /** @brief Returns the cursor's row (0-based). */
                uint16_t cursorRow() const { return _cursor.row; }

                /** @brief Returns true if the cursor is shown (DECTCEM). */
                bool isCursorVisible() const { return _cursorVisible; }

                /** @brief Returns true if the alternate screen is shown. */
                bool isAlternateScreen() const { return _screen == 1; }

                /** @brief Returns true if the program asked for application cursor keys (DECCKM). */
                bool isApplicationCursorKeys() const { return _applicationCursorKeys; }

                /** @brief Returns true if the program asked for the application keypad (DECKPAM). */
                bool isApplicationKeypad() const { return _applicationKeypad; }

                /** @brief Returns true if the program asked for bracketed paste (mode 2004). */
                bool isBracketedPaste() const { return _bracketedPaste; }

                /** @brief Returns true if @p row changed since `clearDamage()`. */
                bool isRowDirty(uint16_t row) const {
                        return row < _rows && (_dirty[row / 32] & (1u << (row % 32))) != 0;
                }

                /** @brief Returns true if any row changed since `clearDamage()`. */
                bool hasDamage() const;

                /** @brief Marks every row clean. */
                void clearDamage();

        private:
                // The cursor and what DECSC saves with it.
                struct Cursor {
                                uint16_t  column = 0;
                                uint16_t  row = 0;
                                bool      pendingWrap = false; // at the last column, the next character wraps
                                bool      originMode = false;  // DECOM: rows count from the scroll region
                                bool      autowrap = true;     // DECAWM
                                uint8_t   charsets = 0;        // bit 0: G0 is line drawing; bit 1: G1 is
                                uint8_t   shift = 0;           // 0: G0 in use (SI), 1: G1 (SO)
                                TextStyle style;
                };

                void print(uint32_t codepoint) override;
                void execute(uint8_t control) override;
                void csiDispatch(const VtParams &params, StringView intermediates, uint8_t final) override;
                void escDispatch(StringView intermediates, uint8_t final) override;
                void oscDispatch(StringView) override {}

                TerminalCell       *rowOf(uint8_t screen, uint16_t row);
                const TerminalCell *rowOf(uint8_t screen, uint16_t row) const;
                TerminalCell       *line(uint16_t row) { return rowOf(_screen, row); }
                TerminalCell        blank() const;
                void                put(uint32_t codepoint, int width);
                void                splitWide(uint16_t row, uint16_t column);
                void                markDirty(uint16_t row) { _dirty[row / 32] |= 1u << (row % 32); }
                void                markAllDirty();
                void                lineFeed();
                void                reverseIndex();
                void                scrollUp(uint16_t top, uint16_t bottom, uint16_t count);
                void                scrollDown(uint16_t top, uint16_t bottom, uint16_t count);
                void                eraseCells(uint16_t row, uint16_t from, uint16_t to); // [from, to)
                void                eraseRows(uint16_t from, uint16_t to);                // [from, to)
                void                insertCells(uint16_t count);
                void                deleteCells(uint16_t count);
                void                moveTo(int column, int row); // clamped; origin mode applied by callers
                void                moveToOrigin(int column, int row);
                void                saveCursor();
                void                restoreCursor();
                void                switchScreen(uint8_t screen);
                void                resetTabs();
                void                tab(int count);
                void                setMode(const VtParams &params, bool isPrivate, bool on);
                void                softReset();
                void                hardReset();
                void                reply(StringView text);

                VtParser      _parser;
                MemSpace      _space;
                uint16_t      _columns = 0;
                uint16_t      _rows = 0;
                void         *_block = nullptr; // the one allocation
                TerminalCell *_cells = nullptr; // both screens, rows stored in any order
                uint16_t     *_rowMap[2] = {};  // per screen: shown row → stored row
                uint32_t     *_tabs = nullptr;  // a bit per column
                uint32_t     *_dirty = nullptr; // a bit per row
                uint8_t       _screen = 0;      // 0 primary, 1 alternate
                Cursor        _cursor;
                Cursor        _saved[2]; // DECSC, per screen
                uint16_t      _top = 0;  // the scroll region, inclusive
                uint16_t      _bottom = 0;
                uint32_t      _last = 0;             // the last character printed, for REP
                bool          _insertMode = false;   // IRM
                bool          _cursorVisible = true; // DECTCEM
                bool          _applicationCursorKeys = false;
                bool          _applicationKeypad = false;
                bool          _bracketedPaste = false;
                ReplySink     _replySink = nullptr;
                void         *_replyContext = nullptr;
};

BMDS_NAMESPACE_END
