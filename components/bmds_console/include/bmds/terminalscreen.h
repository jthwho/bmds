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
#include <bmds/base64.h>
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
 * A row whose text went on in the next row (autowrap) has `wrapped` set on
 * its last cell, so a selection joins them (`devplan/clipboard.md` §4d);
 * writing or erasing that cell clears it.
 */
struct TerminalCell {
                uint32_t codepoint : 21 = ' ';
                uint32_t attributes : 8 = 0;
                uint32_t wide : 1 = 0; ///< The left half of a two-column character.
                uint32_t tail : 1 = 0; ///< The right half of a two-column character.
                uint32_t wrapped : 1 = 0; ///< A row's last cell: the line goes on in the next row (autowrap).
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
 * A history (scrollback) is kept when asked for (`setHistory()`, a second
 * allocation): rows that scroll off the top of the primary screen, and those
 * a resize drops from its top, are copied in without their trailing blank
 * cells, so a short line costs a few cells, not a row (the screen's rows stay
 * few and in the cache). It holds up to so many lines or `HistoryCellsPerLine`
 * times as many cells (`MaxSide` at least: a line is never cut), the oldest
 * lost first; a line keeps its length across resizes. `CSI 3 J` empties it (`devplan/windows.md` §4i).
 *
 * @par Thread Safety
 * Not thread-safe: the owner serializes writes and the renderer's reads.
 */
class TerminalScreen final : private VtHandler {
        public:
                /** @brief Receives replies to the program (status reports), for the input. */
                using ReplySink = void (*)(void *context, StringView reply);

                /** @brief What a program's OSC 52 does (`devplan/clipboard.md` C11). */
                enum class ClipboardEvent : uint8_t {
                        Begin,  ///< It sets @p targets' contents: `Data` follows, then `End` or `Cancel`.
                        Data,   ///< The next decoded bytes.
                        End,    ///< All of it (none: they are emptied).
                        Cancel, ///< Cut off, or not base64: what came is dropped.
                        Query,  ///< It asks for @p targets' contents (`?`).
                };

                /**
                 * @brief Takes OSC 52 (`ESC ] 52 ; TARGETS ; BASE64 ST`), decoded as it
                 *        arrives: @p targets (`c` the clipboard, `p` the primary
                 *        selection, ...; may be empty) with `Begin` and `Query`,
                 *        @p data with `Data`. Called within `write()`.
                 */
                using ClipboardSink = void (*)(void *context, ClipboardEvent event, StringView targets, ByteSpan data);

                /** @brief The history's cells for each line it may hold (an average line's room). */
                static constexpr size_t HistoryCellsPerLine = 64;

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

                /**
                 * @brief Keeps up to @p lines of history (0: none), emptied.
                 * @return `Ok` or `NoMem` (no history then).
                 */
                Error setHistory(uint16_t lines);

                /** @brief Returns the history lines kept. */
                size_t historyCount() const { return _historyCount; }

                /**
                 * @brief Returns history line @p i (0: the oldest kept): @p length
                 *        cells, the rest of the row blank (any width).
                 */
                const TerminalCell *historyRow(size_t i, uint16_t &length) const {
                        const HistoryLine &line = _historyLines[(_historyHead + i) % _historyRoom];
                        length = line.length;
                        return _historyCells + line.start;
                }

                /** @brief Returns the lines ever added to the history (wraps): a view's anchor. */
                uint32_t historyAdded() const { return _historyAdded; }

                /** @brief Returns the width in cells. */
                uint16_t columns() const { return _columns; }

                /** @brief Returns the height in cells. */
                uint16_t rows() const { return _rows; }

                /** @brief Sets where OSC 52 goes (none: it is ignored). */
                void setClipboardSink(ClipboardSink sink, void *context) {
                        _clipboardSink = sink;
                        _clipboardContext = context;
                        return;
                }

                /** @brief The most bytes of an OSC 52's targets kept. */
                static constexpr size_t MaxClipboardTargets = 8;

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

                /** @brief The longest title kept, in bytes. */
                static constexpr size_t MaxTitle = 63;

                /** @brief Returns the title the program set (`OSC 0` or `OSC 2`; empty: none). */
                StringView title() const { return StringView(_title, _titleLength); }

                /** @brief Returns a number that changes whenever the title does. */
                uint32_t titleGeneration() const { return _titleGeneration; }

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

                /** @brief Marks row @p row clean (a renderer took it). */
                void clearRowDamage(uint16_t row) {
                        if (row < _rows) _dirty[row / 32] &= ~(1u << (row % 32));
                        return;
                }

        private:
                // Where a history line's cells are.
                struct HistoryLine {
                                uint32_t start;
                                uint16_t length;
                };

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
                void oscDispatch(StringView data) override;
                bool oscStream(uint32_t number) override;
                void oscData(StringView piece) override;
                void oscEnd(bool complete) override;
                void setTitle(StringView title);

                TerminalCell       *rowOf(uint8_t screen, uint16_t row);
                const TerminalCell *rowOf(uint8_t screen, uint16_t row) const;
                TerminalCell       *line(uint16_t row) { return rowOf(_screen, row); }
                TerminalCell        blank() const;
                void                put(uint32_t codepoint, int width);
                void                markWrapped(uint16_t row);
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
                void                pushHistory(const TerminalCell *row, uint16_t end);
                // Where the stored row shown as @p row ends: no written cell past it.
                uint16_t &endOf(uint8_t screen, uint16_t row) {
                        return _rowEnd[size_t(screen) * _rows + _rowMap[screen][row]];
                }
                void dropOldestLine();

                VtParser      _parser;
                MemSpace      _space;
                uint16_t      _columns = 0;
                uint16_t      _rows = 0;
                void         *_block = nullptr; // the one allocation
                TerminalCell *_cells = nullptr; // both screens, rows stored in any order
                uint16_t     *_rowMap[2] = {};  // per screen: shown row → stored row
                uint32_t     *_tabs = nullptr;  // a bit per column
                uint32_t     *_dirty = nullptr; // a bit per row
                uint16_t     *_rowEnd = nullptr; // per stored row: past it, default blank cells only
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
                void         *_historyBlock = nullptr; // the history's allocation
                HistoryLine  *_historyLines = nullptr; // a ring of _historyRoom lines, the oldest at the head
                TerminalCell *_historyCells = nullptr; // their cells, written in turn, wrapping
                uint32_t      _historyCellRoom = 0;
                uint32_t      _historyWrite = 0; // where the next line's cells go
                uint16_t      _historyRoom = 0;
                uint16_t      _historyHead = 0;
                uint16_t      _historyCount = 0;
                uint32_t      _historyAdded = 0;
                char          _title[MaxTitle + 1] = {};
                uint8_t       _titleLength = 0;
                uint32_t      _titleGeneration = 0;
                ReplySink     _replySink = nullptr;
                void         *_replyContext = nullptr;
                // OSC 52 as it streams: its targets, then its data or a query.
                enum class Clip : uint8_t {
                        Targets,
                        First,
                        Data,
                        Query,
                        Ignore
                };
                ClipboardSink _clipboardSink = nullptr;
                void         *_clipboardContext = nullptr;
                Base64Decoder _clipDecoder;
                Clip          _clip = Clip::Ignore;
                char          _clipTargets[MaxClipboardTargets] = {};
                uint8_t       _clipTargetLength = 0;
};

BMDS_NAMESPACE_END
