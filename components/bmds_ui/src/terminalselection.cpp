/**
 * @file      terminalselection.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A terminal's selection and its text (devplan/clipboard.md C10, §4d).
 */

#include <bmds/terminalselection.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The most rows one line wraps across that a line selection follows.
        constexpr int MaxWrappedRows = 1024;

        // Word classes: blank, a word's, or each punctuation character its own.
        constexpr uint32_t Blank = 0;
        constexpr uint32_t WordClass = 1;

        constexpr bool isWordChar(uint32_t cp) {
                if (cp >= 0x80) return true; // other scripts' letters
                if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')) return true;
                return StringView("-./_~:@%+=?&#").contains(static_cast<char>(cp));
        }

        uint32_t classAt(const TerminalCell *cells, uint16_t length, uint16_t i) {
                if (i >= length) return Blank;
                if (cells[i].tail != 0 && i > 0) --i; // a wide character's right half: the character
                const uint32_t cp = cells[i].codepoint;
                if (cp == ' ' || cp == 0) return Blank;
                return isWordChar(cp) ? WordClass : cp + 2;
        }

        bool wrapped(const TerminalCell *cells, uint16_t length) {
                return cells != nullptr && length > 0 && cells[length - 1].wrapped != 0;
        }

        // Gathers text and hands it out a buffer at a time.
        struct Out {
                        TerminalSelection::Sink sink;
                        void                   *arg;
                        char                    buf[256];
                        size_t                  n = 0;
                        Error                   err;

                        void put(const char *s, size_t len) {
                                for (size_t i = 0; i < len && err.isOk(); ++i) {
                                        buf[n++] = s[i];
                                        if (n == sizeof(buf)) flush();
                                }
                                return;
                        }

                        void flush() {
                                if (n > 0 && err.isOk()) err = sink(arg, StringView(buf, n));
                                n = 0;
                                return;
                        }
        };

} // namespace

void TerminalSelection::span(Point at, Lines lines, void *arg, Point &from, Point &to) const {
        from = to = at;
        if (_unit == Unit::Character || _block) return;
        uint16_t            length = 0;
        const TerminalCell *cells = lines(arg, at.line, length);
        if (length > _columns) length = _columns;
        if (_unit == Unit::Line) {
                // The whole line: back while the row before wrapped into this one, on while this one wraps.
                from = Point{at.line, 0};
                for (int i = 0; i < MaxWrappedRows; ++i) {
                        uint16_t            n = 0;
                        const TerminalCell *before = lines(arg, from.line - 1, n);
                        if (!wrapped(before, n)) break;
                        --from.line;
                }
                to = Point{at.line, _columns};
                const TerminalCell *row = cells;
                uint16_t            n = length;
                for (int i = 0; i < MaxWrappedRows && wrapped(row, n); ++i) {
                        row = lines(arg, to.line + 1, n);
                        if (row == nullptr) break;
                        ++to.line;
                }
                return;
        }
        // A word: the run of its class around the cell.
        const uint16_t cell = at.column < _columns ? at.column : static_cast<uint16_t>(_columns > 0 ? _columns - 1 : 0);
        const uint32_t kind = classAt(cells, length, cell);
        uint16_t       first = cell, past = static_cast<uint16_t>(cell + 1);
        while (first > 0 && classAt(cells, length, static_cast<uint16_t>(first - 1)) == kind) --first;
        while (past < _columns && classAt(cells, length, past) == kind) ++past;
        from = Point{at.line, first};
        to = Point{at.line, past};
        return;
}

void TerminalSelection::begin(Point at, Unit unit, bool block, Lines lines, void *arg) {
        _unit = unit;
        _block = block;
        _active = true;
        span(at, lines, arg, _anchorFrom, _anchorTo);
        _start = _anchorFrom;
        _end = _anchorTo;
        return;
}

void TerminalSelection::extend(Point to, Lines lines, void *arg) {
        if (!_active) return;
        if (_block) {
                const Point a = _anchorFrom;
                _start = Point{a.line < to.line ? a.line : to.line, a.column < to.column ? a.column : to.column};
                _end = Point{a.line < to.line ? to.line : a.line, a.column < to.column ? to.column : a.column};
                return;
        }
        Point from, past;
        span(to, lines, arg, from, past);
        _start = from < _anchorFrom ? from : _anchorFrom;
        _end = _anchorTo < past ? past : _anchorTo;
        return;
}

bool TerminalSelection::isEmpty() const {
        if (!_active) return true;
        return _block ? _start.column == _end.column : !(_start < _end);
}

bool TerminalSelection::columns(int64_t line, uint16_t &from, uint16_t &to) const {
        if (isEmpty() || line < _start.line || line > _end.line) return false;
        if (_block) {
                from = _start.column;
                to = _end.column;
        } else {
                from = line == _start.line ? _start.column : 0;
                to = line == _end.line ? _end.column : _columns;
        }
        if (to > _columns) to = _columns;
        return from < to;
}

Error TerminalSelection::text(Lines lines, void *arg, Sink sink, void *sinkArg) const {
        if (isEmpty()) return Error::Ok;
        Out out{sink, sinkArg, {}, 0, Error()};
        for (int64_t line = _start.line; line <= _end.line && out.err.isOk(); ++line) {
                uint16_t            length = 0;
                const TerminalCell *cells = lines(arg, line, length);
                if (cells == nullptr) length = 0;
                if (length > _columns) length = _columns;
                uint16_t from = 0, to = 0;
                if (columns(line, from, to)) {
                        // Blanks are held until something follows them: those at the row's end go.
                        size_t blanks = 0;
                        for (uint16_t c = from; c < to && c < length; ++c) {
                                const TerminalCell &cell = cells[c];
                                if (cell.tail != 0) continue;
                                if (cell.wide != 0 && c + 1 == _columns) break; // cut at the edge
                                if (cell.codepoint == ' ' || cell.codepoint == 0) {
                                        ++blanks;
                                        continue;
                                }
                                for (; blanks > 0; --blanks) out.put(" ", 1);
                                char         utf8[4];
                                const size_t n = Utf8::encode(cell.codepoint, utf8);
                                out.put(utf8, n);
                        }
                        // Selected in the middle of a row: its blanks were chosen.
                        if (to < length) {
                                for (; blanks > 0; --blanks) out.put(" ", 1);
                        }
                }
                const bool last = line == _end.line;
                const bool joined = !_block && !last && wrapped(cells, length);
                if ((!last && !joined) || (last && _unit == Unit::Line && !_block)) out.put("\n", 1);
        }
        out.flush();
        return out.err;
}

BMDS_NAMESPACE_END
