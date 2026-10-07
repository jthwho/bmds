/**
 * @file      terminalselection.h
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
#include <bmds/stringview.h>
#include <bmds/terminalscreen.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What is selected in a terminal, and its text (`devplan/clipboard.md`
 *        C10, §4d): pure, so host-tested.
 *
 * Lines are numbered from the history's start (`historyAdded()` minus the
 * history's count is its oldest line), so a selection stays on its text as
 * the text scrolls into the history. A press starts it (`begin()`), dragging
 * moves its other end (`extend()`), in a unit: characters (a point is a
 * boundary between cells, so a click selects nothing), words (the run of
 * cells of one class around the cell: letters, digits and `-./_~:@%+=?&#`;
 * spaces; or one punctuation character repeated) or lines (the whole line,
 * rows the text wrapped across included, and its line end). A block selects
 * the same columns of every line.
 *
 * Its text (`text()`): each row's cells (a wide character once), the blanks
 * at a row's end dropped, rows joined by LF unless the text wrapped from one
 * to the next (not in a block).
 *
 * @par Thread Safety
 * A value; the rows read as the caller locked them.
 */
class TerminalSelection {
        public:
                /** @brief What a press selects. */
                enum class Unit : uint8_t {
                        Character, ///< One click.
                        Word,      ///< Two.
                        Line,      ///< Three.
                };

                /** @brief A place: a line (see the class notes) and a column (a cell, or for characters the boundary before it). */
                struct Point {
                                int64_t  line = 0;
                                uint16_t column = 0;

                                constexpr bool operator==(const Point &other) const = default;
                                constexpr bool operator<(const Point &other) const {
                                        return line < other.line || (line == other.line && column < other.column);
                                }
                };

                /**
                 * @brief Gives line @p line's cells and their number (a history
                 *        line may be shorter than the screen), or null (gone).
                 */
                using Lines = const TerminalCell *(*)(void *arg, int64_t line, uint16_t &length);

                /** @brief Takes the selection's text in pieces. */
                using Sink = Error (*)(void *arg, StringView piece);

                /** @brief The terminal's width in cells (rows wider are cut). */
                void setColumns(uint16_t columns) {
                        _columns = columns;
                        return;
                }

                /** @brief Starts at @p at (a press), in @p unit, a block or not. */
                void begin(Point at, Unit unit, bool block, Lines lines, void *arg);

                /** @brief Moves its other end to @p to (dragging, or a Shift+click). */
                void extend(Point to, Lines lines, void *arg);

                /** @brief Selects nothing. */
                void clear() {
                        _active = false;
                        return;
                }

                /** @brief Returns true between `begin()` and `clear()`. */
                bool isActive() const { return _active; }

                /** @brief Returns true if nothing is selected (none, or a click). */
                bool isEmpty() const;

                /** @brief Returns its unit. */
                Unit unit() const { return _unit; }

                /** @brief Returns its first line. */
                int64_t firstLine() const { return _start.line; }

                /** @brief Returns its last line. */
                int64_t lastLine() const { return _end.line; }

                /** @brief Returns true if any of lines @p first to @p last is selected. */
                bool covers(int64_t first, int64_t last) const {
                        return !isEmpty() && first <= _end.line && last >= _start.line;
                }

                /**
                 * @brief Gives line @p line's selected columns, [@p from, @p to).
                 * @return False: none.
                 * @hotpath
                 */
                bool columns(int64_t line, uint16_t &from, uint16_t &to) const;

                /**
                 * @brief Gives its text (UTF-8) to @p sink in pieces.
                 * @return `Ok`, or the sink's error (the rest not given).
                 */
                Error text(Lines lines, void *arg, Sink sink, void *sinkArg) const;

        private:
                // The span of the unit around @p at, [from, to) as points.
                void span(Point at, Lines lines, void *arg, Point &from, Point &to) const;

                Point    _anchorFrom; // the press's span
                Point    _anchorTo;
                Point    _start; // the selection: _start to _end (the end's column excluded)
                Point    _end;
                uint16_t _columns = 0;
                Unit     _unit = Unit::Character;
                bool     _block = false;
                bool     _active = false;
};

BMDS_NAMESPACE_END
