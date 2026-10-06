/**
 * @file      terminalscreen.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Behavior follows xterm where VT100/ECMA-48 leave a choice.
 */

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/terminalscreen.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t TabWidth = 8;

        // The DEC Special Graphics set: 0x5f-0x7e as Unicode.
        constexpr uint16_t LineDrawing[32] = {
                0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b,
                0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c,
                0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
        };

        size_t words(size_t bits) {
                return (bits + 31) / 32;
        }

        // Appends @p value in decimal.
        void appendNumber(char *&out, unsigned value) {
                char   digits[10];
                size_t n = 0;
                do {
                        digits[n++] = static_cast<char>('0' + value % 10);
                        value /= 10;
                } while (value != 0);
                while (n > 0) *out++ = digits[--n];
                return;
        }

} // namespace

TerminalScreen::~TerminalScreen() {
        Memory::free(_block);
        Memory::free(_historyBlock);
        return;
}

Error TerminalScreen::setHistory(uint16_t lines) {
        Memory::free(_historyBlock);
        _historyBlock = nullptr;
        _historyLines = nullptr;
        _historyCells = nullptr;
        _historyRoom = 0;
        _historyCellRoom = 0;
        _historyHead = 0;
        _historyCount = 0;
        _historyWrite = 0;
        if (lines == 0) return Error::Ok;
        // Room for the longest row at least: a line is never cut.
        const size_t cells = std::max<size_t>(size_t(lines) * HistoryCellsPerLine, MaxSide);
        const size_t lineBytes = (size_t(lines) * sizeof(HistoryLine) + 3) & ~size_t(3);
        void        *block = Memory::alloc(_space, lineBytes + cells * sizeof(TerminalCell));
        if (block == nullptr) return Error::NoMem;
        _historyBlock = block;
        _historyLines = static_cast<HistoryLine *>(block);
        _historyCells = reinterpret_cast<TerminalCell *>(static_cast<uint8_t *>(block) + lineBytes);
        _historyRoom = lines;
        _historyCellRoom = static_cast<uint32_t>(cells);
        return Error::Ok;
}

void TerminalScreen::dropOldestLine() {
        _historyHead = static_cast<uint16_t>((_historyHead + 1) % _historyRoom);
        --_historyCount;
        return;
}

// A row leaving the top of the primary screen, into the history without its trailing blank
// cells: its cells in one piece after the last line's (from the start when they don't fit
// before the end), the lines whose cells they overwrite (the oldest) dropped.
void TerminalScreen::pushHistory(const TerminalCell *row, uint16_t end) {
        if (_historyRoom == 0) return;
        // Its trailing blank cells left out, compared as three words inline (operator== goes field by
        // field, and memcmp is a call a cell: both several times slower over a row of blanks).
        struct Words {
                        uint32_t w[3];
        };
        static_assert(sizeof(Words) == sizeof(TerminalCell));
        constexpr Words blank = std::bit_cast<Words>(TerminalCell());
        uint32_t        n = end; // past it, blank: only the cells written are read
        while (n > 0) {
                const Words c = std::bit_cast<Words>(row[n - 1]);
                if (c.w[0] != blank.w[0] || c.w[1] != blank.w[1] || c.w[2] != blank.w[2]) break;
                --n;
        }
        if (_historyCount == _historyRoom) dropOldestLine();
        // Drops the oldest lines through the last that @p hits (in order; the empty lines, which
        // hold no cells, go only with older ones).
        auto dropThrough = [this](auto hits) {
                int last = -1;
                for (uint16_t i = 0; i < _historyCount; ++i) {
                        const HistoryLine &line = _historyLines[(_historyHead + i) % _historyRoom];
                        if (line.length == 0) continue;
                        if (!hits(line)) break;
                        last = i;
                }
                for (int i = 0; i <= last; ++i) dropOldestLine();
        };
        uint32_t at = _historyWrite;
        if (at + n > _historyCellRoom) {
                // Wrapping: the lines after the write point (the oldest) go first.
                dropThrough([at](const HistoryLine &line) { return line.start >= at; });
                at = 0;
        }
        dropThrough([at, n](const HistoryLine &line) { return line.start < at + n && line.start + line.length > at; });
        if (n > 0) std::memcpy(static_cast<void *>(_historyCells + at), row, n * sizeof(TerminalCell));
        _historyLines[(_historyHead + _historyCount) % _historyRoom] = HistoryLine{at, static_cast<uint16_t>(n)};
        ++_historyCount;
        _historyWrite = at + n;
        ++_historyAdded;
        return;
}

Error TerminalScreen::resize(uint16_t columns, uint16_t rows) {
        if (columns == 0 || rows == 0 || columns > MaxSide || rows > MaxSide) return Error::Invalid;
        const size_t cellBytes = size_t(columns) * rows * 2 * sizeof(TerminalCell);
        const size_t mapBytes = (size_t(rows) * 2 * sizeof(uint16_t) + 3) & ~size_t(3);
        const size_t tabBytes = words(columns) * sizeof(uint32_t);
        const size_t dirtyBytes = words(rows) * sizeof(uint32_t);
        const size_t endBytes = size_t(rows) * 2 * sizeof(uint16_t);
        void        *block = Memory::alloc(_space, cellBytes + mapBytes + tabBytes + dirtyBytes + endBytes);
        if (block == nullptr) return Error::NoMem;

        uint8_t      *bytes = static_cast<uint8_t *>(block);
        TerminalCell *cells = reinterpret_cast<TerminalCell *>(bytes);
        uint16_t     *maps = reinterpret_cast<uint16_t *>(bytes + cellBytes);
        const size_t  count = size_t(columns) * rows * 2;
        for (size_t i = 0; i < count; ++i) new (&cells[i]) TerminalCell();
        for (uint16_t r = 0; r < rows; ++r) maps[r] = maps[rows + r] = r;
        auto *ends = reinterpret_cast<uint16_t *>(bytes + cellBytes + mapBytes + tabBytes + dirtyBytes);
        for (uint32_t r = 0; r < uint32_t(rows) * 2; ++r) ends[r] = 0;

        // Each screen keeps its top-left; when rows go, the top ones go
        // first, as far as keeping its cursor's row on the screen needs.
        for (uint8_t s = 0; s < 2; ++s) {
                Cursor        &cursor = s == _screen ? _cursor : _saved[s];
                const uint16_t drop = cursor.row >= rows ? static_cast<uint16_t>(cursor.row - rows + 1) : 0;
                if (_cells != nullptr && s == 0)
                        for (uint16_t r = 0; r < drop; ++r) pushHistory(rowOf(0, r), endOf(0, r)); // off the top: kept
                if (_cells != nullptr) {
                        const uint16_t keepRows = static_cast<uint16_t>(std::min<int>(rows, _rows - drop));
                        const uint16_t keepColumns = std::min(columns, _columns);
                        for (uint16_t r = 0; r < keepRows; ++r) {
                                TerminalCell *to = cells + (size_t(s) * rows + r) * columns;
                                std::memcpy(to, rowOf(s, static_cast<uint16_t>(r + drop)),
                                            keepColumns * sizeof(TerminalCell));
                                if (to[keepColumns - 1].wide) to[keepColumns - 1] = TerminalCell();
                                ends[size_t(s) * rows + r] =
                                        std::min(endOf(s, static_cast<uint16_t>(r + drop)), keepColumns);
                        }
                }
                cursor.row = static_cast<uint16_t>(std::min<int>(cursor.row - drop, rows - 1));
                cursor.column = std::min(cursor.column, static_cast<uint16_t>(columns - 1));
                cursor.pendingWrap = false;
        }
        // The shown screen's saved cursor (DECSC) is only kept on the screen.
        Cursor &other = _saved[_screen];
        other.row = std::min(other.row, static_cast<uint16_t>(rows - 1));
        other.column = std::min(other.column, static_cast<uint16_t>(columns - 1));

        Memory::free(_block);
        _block = block;
        _cells = cells;
        _rowMap[0] = maps;
        _rowMap[1] = maps + rows;
        _tabs = reinterpret_cast<uint32_t *>(bytes + cellBytes + mapBytes);
        _dirty = reinterpret_cast<uint32_t *>(bytes + cellBytes + mapBytes + tabBytes);
        _rowEnd = ends;
        _columns = columns;
        _rows = rows;
        _top = 0;
        _bottom = static_cast<uint16_t>(rows - 1);
        resetTabs();
        markAllDirty();
        return Error::Ok;
}

void TerminalScreen::reset() {
        _parser.reset();
        if (_cells != nullptr) hardReset();
        return;
}

bool TerminalScreen::hasDamage() const {
        for (size_t i = 0; i < words(_rows); ++i) {
                if (_dirty[i] != 0) return true;
        }
        return false;
}

void TerminalScreen::clearDamage() {
        if (_dirty != nullptr) std::memset(_dirty, 0, words(_rows) * sizeof(uint32_t));
        return;
}

TerminalCell *TerminalScreen::rowOf(uint8_t screen, uint16_t row) {
        return _cells + (size_t(screen) * _rows + _rowMap[screen][row]) * _columns;
}

const TerminalCell *TerminalScreen::rowOf(uint8_t screen, uint16_t row) const {
        return _cells + (size_t(screen) * _rows + _rowMap[screen][row]) * _columns;
}

TerminalCell TerminalScreen::blank() const {
        TerminalCell c;
        c.background = _cursor.style.background;
        return c;
}

void TerminalScreen::markAllDirty() {
        for (uint16_t r = 0; r < _rows; ++r) markDirty(r);
        return;
}

// Text

void TerminalScreen::print(uint32_t codepoint) {
        const uint8_t set = _cursor.shift == 0 ? 1 : 2;
        if ((_cursor.charsets & set) != 0 && codepoint >= 0x5f && codepoint <= 0x7e)
                codepoint = LineDrawing[codepoint - 0x5f];
        const int width = Utf8::displayWidth(codepoint);
        if (width <= 0) return; // combining marks: dropped for now
        _last = codepoint;
        put(codepoint, width);
        return;
}

void TerminalScreen::put(uint32_t codepoint, int width) {
        if (width > _columns) return;
        if (_cursor.pendingWrap) {
                _cursor.column = 0;
                lineFeed();
        }
        if (_cursor.column + width > _columns) { // a wide character at the last column
                if (_cursor.autowrap) {
                        _cursor.column = 0;
                        lineFeed();
                } else {
                        _cursor.column = static_cast<uint16_t>(_columns - width);
                }
        }
        const uint16_t row = _cursor.row;
        const uint16_t column = _cursor.column;
        if (_insertMode) insertCells(static_cast<uint16_t>(width));
        splitWide(row, column);
        if (width == 2) splitWide(row, static_cast<uint16_t>(column + 1));

        TerminalCell *cells = line(row);
        TerminalCell  c;
        c.codepoint = codepoint;
        c.attributes = _cursor.style.attributes;
        c.foreground = _cursor.style.foreground;
        c.background = _cursor.style.background;
        c.wide = width == 2;
        cells[column] = c;
        if (width == 2) {
                c.codepoint = 0;
                c.wide = 0;
                c.tail = 1;
                cells[column + 1] = c;
        }
        uint16_t &end = endOf(_screen, row);
        end = std::max(end, static_cast<uint16_t>(column + width));
        markDirty(row);
        if (column + width >= _columns) {
                _cursor.column = static_cast<uint16_t>(_columns - 1);
                _cursor.pendingWrap = _cursor.autowrap;
        } else {
                _cursor.column = static_cast<uint16_t>(column + width);
        }
        return;
}

// Blanks both halves of a two-column character covering @p column, if any.
void TerminalScreen::splitWide(uint16_t row, uint16_t column) {
        TerminalCell *cells = line(row);
        if (cells[column].tail && column > 0) {
                cells[column - 1] = blank();
                cells[column] = blank();
        } else if (cells[column].wide && column + 1 < _columns) {
                cells[column] = blank();
                cells[column + 1] = blank();
        }
        return;
}

// Controls

void TerminalScreen::execute(uint8_t control) {
        switch (control) {
                case 0x08: // BS
                        if (_cursor.column > 0) --_cursor.column;
                        _cursor.pendingWrap = false;
                        break;
                case 0x09: tab(1); break; // HT
                case 0x0a:                // LF
                case 0x0b:                // VT
                case 0x0c: lineFeed(); break;
                case 0x0d: // CR
                        _cursor.column = 0;
                        _cursor.pendingWrap = false;
                        break;
                case 0x0e: _cursor.shift = 1; break; // SO
                case 0x0f: _cursor.shift = 0; break; // SI
                default: break;                      // BEL and the rest
        }
        return;
}

void TerminalScreen::lineFeed() {
        _cursor.pendingWrap = false;
        if (_cursor.row == _bottom) {
                scrollUp(_top, _bottom, 1);
        } else if (_cursor.row + 1 < _rows) {
                ++_cursor.row;
        }
        return;
}

void TerminalScreen::reverseIndex() {
        _cursor.pendingWrap = false;
        if (_cursor.row == _top) {
                scrollDown(_top, _bottom, 1);
        } else if (_cursor.row > 0) {
                --_cursor.row;
        }
        return;
}

// Rows [top, bottom] move up by @p count; blank rows come in at the bottom.
void TerminalScreen::scrollUp(uint16_t top, uint16_t bottom, uint16_t count) {
        count = std::min(count, static_cast<uint16_t>(bottom - top + 1));
        if (count == 0) return;
        if (_screen == 0 && top == 0)
                for (uint16_t r = 0; r < count; ++r) pushHistory(line(r), endOf(_screen, r)); // off the top: kept
        uint16_t *map = _rowMap[_screen];
        std::rotate(map + top, map + top + count, map + bottom + 1);
        eraseRows(static_cast<uint16_t>(bottom + 1 - count), static_cast<uint16_t>(bottom + 1));
        for (uint16_t r = top; r <= bottom; ++r) markDirty(r);
        return;
}

// Rows [top, bottom] move down by @p count; blank rows come in at the top.
void TerminalScreen::scrollDown(uint16_t top, uint16_t bottom, uint16_t count) {
        count = std::min(count, static_cast<uint16_t>(bottom - top + 1));
        if (count == 0) return;
        uint16_t *map = _rowMap[_screen];
        std::rotate(map + top, map + bottom + 1 - count, map + bottom + 1);
        eraseRows(top, static_cast<uint16_t>(top + count));
        for (uint16_t r = top; r <= bottom; ++r) markDirty(r);
        return;
}

void TerminalScreen::eraseCells(uint16_t row, uint16_t from, uint16_t to) {
        if (from >= to) return;
        splitWide(row, from);
        splitWide(row, static_cast<uint16_t>(to - 1));
        TerminalCell      *cells = line(row);
        const TerminalCell b = blank();
        for (uint16_t c = from; c < to; ++c) cells[c] = b;
        uint16_t &end = endOf(_screen, row);
        if (!b.background.isDefault()) {
                end = std::max(end, to); // colored blanks are written cells
        } else if (to >= end) {
                end = std::min(end, from);
        }
        markDirty(row);
        return;
}

void TerminalScreen::eraseRows(uint16_t from, uint16_t to) {
        const TerminalCell b = blank();
        for (uint16_t r = from; r < to; ++r) {
                TerminalCell *cells = line(r);
                for (uint16_t c = 0; c < _columns; ++c) cells[c] = b;
                endOf(_screen, r) = b.background.isDefault() ? 0 : _columns;
                markDirty(r);
        }
        return;
}

void TerminalScreen::insertCells(uint16_t count) {
        const uint16_t row = _cursor.row;
        const uint16_t column = _cursor.column;
        count = std::min(count, static_cast<uint16_t>(_columns - column));
        splitWide(row, column);
        TerminalCell *cells = line(row);
        std::memmove(cells + column + count, cells + column, (_columns - column - count) * sizeof(TerminalCell));
        const TerminalCell b = blank();
        for (uint16_t c = column; c < column + count; ++c) cells[c] = b;
        if (cells[_columns - 1].wide) cells[_columns - 1] = b; // its tail went off the end
        uint16_t &end = endOf(_screen, row);
        end = b.background.isDefault() ? std::min<uint16_t>(_columns, static_cast<uint16_t>(end + count)) : _columns;
        _cursor.pendingWrap = false;
        markDirty(row);
        return;
}

void TerminalScreen::deleteCells(uint16_t count) {
        const uint16_t row = _cursor.row;
        const uint16_t column = _cursor.column;
        count = std::min(count, static_cast<uint16_t>(_columns - column));
        splitWide(row, column);
        splitWide(row, static_cast<uint16_t>(column + count - 1));
        TerminalCell *cells = line(row);
        std::memmove(cells + column, cells + column + count, (_columns - column - count) * sizeof(TerminalCell));
        const TerminalCell b = blank();
        for (uint16_t c = static_cast<uint16_t>(_columns - count); c < _columns; ++c) cells[c] = b;
        if (!b.background.isDefault()) endOf(_screen, row) = _columns; // else the end only moves left
        _cursor.pendingWrap = false;
        markDirty(row);
        return;
}

// Cursor

void TerminalScreen::moveTo(int column, int row) {
        _cursor.column = static_cast<uint16_t>(std::clamp(column, 0, _columns - 1));
        _cursor.row = static_cast<uint16_t>(std::clamp(row, 0, _rows - 1));
        _cursor.pendingWrap = false;
        return;
}

void TerminalScreen::moveToOrigin(int column, int row) {
        if (_cursor.originMode) {
                moveTo(column, std::clamp(row + _top, int(_top), int(_bottom)));
        } else {
                moveTo(column, row);
        }
        return;
}

void TerminalScreen::saveCursor() {
        _saved[_screen] = _cursor;
        return;
}

void TerminalScreen::restoreCursor() {
        _cursor = _saved[_screen];
        _cursor.column = std::min(_cursor.column, static_cast<uint16_t>(_columns - 1));
        _cursor.row = std::min(_cursor.row, static_cast<uint16_t>(_rows - 1));
        return;
}

void TerminalScreen::switchScreen(uint8_t screen) {
        if (screen == _screen) return;
        _screen = screen;
        markAllDirty();
        return;
}

void TerminalScreen::resetTabs() {
        std::memset(_tabs, 0, words(_columns) * sizeof(uint32_t));
        for (uint16_t c = TabWidth; c < _columns; c += TabWidth) _tabs[c / 32] |= 1u << (c % 32);
        return;
}

void TerminalScreen::tab(int count) {
        int column = _cursor.column;
        for (; count > 0; --count) {
                do ++column;
                while (column < _columns - 1 && (_tabs[column / 32] & (1u << (column % 32))) == 0);
        }
        for (; count < 0; ++count) {
                do --column;
                while (column > 0 && (_tabs[column / 32] & (1u << (column % 32))) == 0);
        }
        moveTo(column, _cursor.row);
        return;
}

void TerminalScreen::reply(StringView text) {
        if (_replySink != nullptr) _replySink(_replyContext, text);
        return;
}

// Sequences

void TerminalScreen::escDispatch(StringView intermediates, uint8_t final) {
        if (intermediates.size() == 1) {
                switch (intermediates[0]) {
                        case '(': // G0
                        case ')': // G1
                        {
                                const uint8_t bit = intermediates[0] == '(' ? 1 : 2;
                                if (final == '0') {
                                        _cursor.charsets |= bit;
                                } else {
                                        _cursor.charsets &= static_cast<uint8_t>(~bit);
                                }
                                break;
                        }
                        case '#':
                                if (final == '8') { // DECALN: the screen full of E
                                        _top = 0;
                                        _bottom = static_cast<uint16_t>(_rows - 1);
                                        TerminalCell e;
                                        e.codepoint = 'E';
                                        for (uint16_t r = 0; r < _rows; ++r) {
                                                TerminalCell *cells = line(r);
                                                for (uint16_t c = 0; c < _columns; ++c) cells[c] = e;
                                                endOf(_screen, r) = _columns;
                                                markDirty(r);
                                        }
                                        moveTo(0, 0);
                                }
                                break;
                        default: break;
                }
                return;
        }
        if (!intermediates.isEmpty()) return;
        switch (final) {
                case 'D': lineFeed(); break; // IND
                case 'E':                    // NEL
                        _cursor.column = 0;
                        lineFeed();
                        break;
                case 'M': reverseIndex(); break;                                            // RI
                case 'H': _tabs[_cursor.column / 32] |= 1u << (_cursor.column % 32); break; // HTS
                case '7': saveCursor(); break;                                              // DECSC
                case '8': restoreCursor(); break;                                           // DECRC
                case 'c': hardReset(); break;                                               // RIS
                case '=': _applicationKeypad = true; break;                                 // DECKPAM
                case '>': _applicationKeypad = false; break;                                // DECKPNM
                default: break;
        }
        return;
}

void TerminalScreen::csiDispatch(const VtParams &params, StringView intermediates, uint8_t final) {
        if (!intermediates.isEmpty()) {
                if (intermediates.size() == 1 && intermediates[0] == '!' && final == 'p') softReset(); // DECSTR
                return; // DECSCUSR (cursor shape) and the rest: ignored
        }
        const int n = params.get(0, 1);
        const int row = _cursor.row;
        const int column = _cursor.column;
        if (params.privateMarker != 0) {
                if (params.privateMarker == '?' && (final == 'h' || final == 'l')) setMode(params, true, final == 'h');
                if (params.privateMarker == '>' && final == 'c' && params.get(0, 0) == 0) reply("\x1b[>1;10;0c");
                return;
        }
        switch (final) {
                case '@': insertCells(static_cast<uint16_t>(std::min(n, int(_columns)))); break;               // ICH
                case 'A': moveTo(column, std::max(row - n, row >= _top ? int(_top) : 0)); break;               // CUU
                case 'B':                                                                                      // CUD
                case 'e': moveTo(column, std::min(row + n, row <= _bottom ? int(_bottom) : _rows - 1)); break; // VPR
                case 'C':                                                                                      // CUF
                case 'a': moveTo(column + n, row); break;                                                      // HPR
                case 'D': moveTo(column - n, row); break;                                                      // CUB
                case 'E': moveTo(0, std::min(row + n, row <= _bottom ? int(_bottom) : _rows - 1)); break;      // CNL
                case 'F': moveTo(0, std::max(row - n, row >= _top ? int(_top) : 0)); break;                    // CPL
                case 'G':                                                                                      // CHA
                case '`': moveTo(n - 1, row); break;                                                           // HPA
                case 'H':                                                                                      // CUP
                case 'f': moveToOrigin(params.get(1, 1) - 1, n - 1); break;                                    // HVP
                case 'I': tab(n); break;                                                                       // CHT
                case 'Z': tab(-n); break;                                                                      // CBT
                case 'J':                                                                                      // ED
                        switch (params.get(0, 0)) {
                                case 0:
                                        eraseCells(_cursor.row, _cursor.column, _columns);
                                        eraseRows(static_cast<uint16_t>(_cursor.row + 1), _rows);
                                        break;
                                case 1:
                                        eraseRows(0, _cursor.row);
                                        eraseCells(_cursor.row, 0, static_cast<uint16_t>(_cursor.column + 1));
                                        break;
                                case 2: eraseRows(0, _rows); break;
                                case 3: // the history emptied
                                        _historyHead = 0;
                                        _historyCount = 0;
                                        _historyWrite = 0;
                                        break;
                        }
                        _cursor.pendingWrap = false;
                        break;
                case 'K': // EL
                        switch (params.get(0, 0)) {
                                case 0: eraseCells(_cursor.row, _cursor.column, _columns); break;
                                case 1: eraseCells(_cursor.row, 0, static_cast<uint16_t>(_cursor.column + 1)); break;
                                case 2: eraseCells(_cursor.row, 0, _columns); break;
                                default: break;
                        }
                        _cursor.pendingWrap = false;
                        break;
                case 'L': // IL
                case 'M': // DL
                        if (row < _top || row > _bottom) break;
                        if (final == 'L') {
                                scrollDown(_cursor.row, _bottom, static_cast<uint16_t>(std::min(n, int(_rows))));
                        } else {
                                scrollUp(_cursor.row, _bottom, static_cast<uint16_t>(std::min(n, int(_rows))));
                        }
                        moveTo(0, row);
                        break;
                case 'P': deleteCells(static_cast<uint16_t>(std::min(n, int(_columns)))); break;            // DCH
                case 'S': scrollUp(_top, _bottom, static_cast<uint16_t>(std::min(n, int(_rows)))); break;   // SU
                case 'T': scrollDown(_top, _bottom, static_cast<uint16_t>(std::min(n, int(_rows)))); break; // SD
                case 'X':                                                                                   // ECH
                        eraseCells(_cursor.row, _cursor.column,
                                   static_cast<uint16_t>(std::min(column + n, int(_columns))));
                        _cursor.pendingWrap = false;
                        break;
                case 'b': // REP
                        if (_last != 0) {
                                const int width = Utf8::displayWidth(_last);
                                for (int i = 0; i < std::min(n, _columns * _rows); ++i) put(_last, width);
                        }
                        break;
                case 'c': // DA: a VT220 with ANSI color
                        if (params.get(0, 0) == 0) reply("\x1b[?62;22c");
                        break;
                case 'd': moveToOrigin(column, n - 1); break; // VPA
                case 'g':                                     // TBC
                        if (params.get(0, 0) == 0) {
                                _tabs[column / 32] &= ~(1u << (column % 32));
                        } else if (params.get(0, 0) == 3) {
                                std::memset(_tabs, 0, words(_columns) * sizeof(uint32_t));
                        }
                        break;
                case 'h': setMode(params, false, true); break;  // SM
                case 'l': setMode(params, false, false); break; // RM
                case 'm': Sgr::apply(params, _cursor.style); break;
                case 'n': // DSR
                        if (params.get(0, 0) == 5) {
                                reply("\x1b[0n");
                        } else if (params.get(0, 0) == 6) {
                                char  text[24] = "\x1b[";
                                char *out = text + 2;
                                appendNumber(out, unsigned(_cursor.row - (_cursor.originMode ? _top : 0) + 1));
                                *out++ = ';';
                                appendNumber(out, unsigned(_cursor.column + 1));
                                *out++ = 'R';
                                reply(StringView(text, size_t(out - text)));
                        }
                        break;
                case 'r': // DECSTBM
                {
                        const int top = params.get(0, 1) - 1;
                        const int bottom = std::min(int(params.get(1, _rows)), int(_rows)) - 1;
                        if (top < bottom) {
                                _top = static_cast<uint16_t>(top);
                                _bottom = static_cast<uint16_t>(bottom);
                                moveToOrigin(0, 0);
                        }
                        break;
                }
                case 's': // SCOSC
                        if (params.count == 0) saveCursor();
                        break;
                case 'u': restoreCursor(); break; // SCORC
                case 't':                         // window operations: the size report only
                        if (params.get(0, 0) == 18) {
                                char  text[24] = "\x1b[8;";
                                char *out = text + 4;
                                appendNumber(out, _rows);
                                *out++ = ';';
                                appendNumber(out, _columns);
                                *out++ = 't';
                                reply(StringView(text, size_t(out - text)));
                        }
                        break;
                default: break;
        }
        return;
}

void TerminalScreen::setMode(const VtParams &params, bool isPrivate, bool on) {
        for (size_t i = 0; i < params.count; ++i) {
                const uint16_t mode = params.values[i];
                if (!isPrivate) {
                        if (mode == 4) _insertMode = on; // IRM
                        continue;
                }
                switch (mode) {
                        case 1: _applicationCursorKeys = on; break; // DECCKM
                        case 6:                                     // DECOM
                                _cursor.originMode = on;
                                moveToOrigin(0, 0);
                                break;
                        case 7: // DECAWM
                                _cursor.autowrap = on;
                                if (!on) _cursor.pendingWrap = false;
                                break;
                        case 25: _cursorVisible = on; break; // DECTCEM
                        case 47: switchScreen(on ? 1 : 0); break;
                        case 1047:
                                if (!on && _screen == 1) eraseRows(0, _rows);
                                switchScreen(on ? 1 : 0);
                                break;
                        case 1048:
                                if (on) {
                                        saveCursor();
                                } else {
                                        restoreCursor();
                                }
                                break;
                        case 1049:
                                if (on && _screen == 0) {
                                        saveCursor();
                                        switchScreen(1);
                                        eraseRows(0, _rows);
                                } else if (!on && _screen == 1) {
                                        switchScreen(0);
                                        restoreCursor();
                                }
                                break;
                        case 2004: _bracketedPaste = on; break;
                        default: break;
                }
        }
        return;
}

// DECSTR: modes and the style to their defaults; the screen and the cursor's place stay.
void TerminalScreen::softReset() {
        _cursorVisible = true;
        _insertMode = false;
        _applicationCursorKeys = false;
        _applicationKeypad = false;
        _cursor.originMode = false;
        _cursor.autowrap = true;
        _cursor.pendingWrap = false;
        _cursor.charsets = 0;
        _cursor.shift = 0;
        _cursor.style = TextStyle();
        _top = 0;
        _bottom = static_cast<uint16_t>(_rows - 1);
        _saved[_screen] = Cursor();
        return;
}

// OSC 0 (the icon's name and the title) and OSC 2 (the title) set the title; the rest are ignored.
void TerminalScreen::oscDispatch(StringView data) {
        if (data.size() < 2 || data[1] != ';' || (data[0] != '0' && data[0] != '2')) return;
        setTitle(data.mid(2));
        return;
}

// The title, cut to MaxTitle bytes on a character's boundary, an incomplete last character left
// out (the parser cuts long OSC strings anywhere).
void TerminalScreen::setTitle(StringView title) {
        size_t n = title.size() < MaxTitle ? title.size() : MaxTitle;
        if (n < title.size())
                while (n > 0 && Utf8::isContinuation(static_cast<uint8_t>(title[n]))) --n;
        size_t lead = n;
        while (lead > 0 && Utf8::isContinuation(static_cast<uint8_t>(title[lead - 1]))) --lead;
        if (lead > 0) {
                const auto   b = static_cast<uint8_t>(title[lead - 1]);
                const size_t length = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : b >= 0xc0 ? 2 : 1;
                if (n - (lead - 1) < length) n = lead - 1;
        }
        if (n == _titleLength && std::memcmp(_title, title.data(), n) == 0) return;
        std::memcpy(_title, title.data(), n);
        _title[n] = '\0';
        _titleLength = static_cast<uint8_t>(n);
        ++_titleGeneration;
        return;
}

void TerminalScreen::hardReset() {
        setTitle(StringView());
        _cursor = Cursor();
        _saved[0] = _saved[1] = Cursor();
        _top = 0;
        _bottom = static_cast<uint16_t>(_rows - 1);
        _last = 0;
        _insertMode = false;
        _cursorVisible = true;
        _applicationCursorKeys = false;
        _applicationKeypad = false;
        _bracketedPaste = false;
        for (uint8_t s = 0; s < 2; ++s) {
                _screen = s;
                for (uint16_t r = 0; r < _rows; ++r) _rowMap[s][r] = r;
                eraseRows(0, _rows);
        }
        _screen = 0;
        resetTabs();
        markAllDirty();
        return;
}

BMDS_NAMESPACE_END
