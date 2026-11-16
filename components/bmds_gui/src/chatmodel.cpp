/**
 * @file      chatmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A ChatView's lines, pure (devplan/chatview.md §2): IRC's codes parsed into runs, links found,
 * lines wrapped in a measure of the caller's, points hit, and the lines kept with their rows.
 */

#include <cstring>
#include <new>
#include <utility>
#include <bmds/chatmodel.h>
#include <bmds/memory.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace ChatText {

        namespace {

                // mIRC's 16, then the 83 extended colours (16-98) clients agree on.
                constexpr uint32_t Palette[PaletteSize] = {
                        0xffffff, 0x000000, 0x00007f, 0x009300, 0xff0000, 0x7f0000, 0x9c009c, 0xfc7f00, 0xffff00,
                        0x00fc00, 0x009393, 0x00ffff, 0x0000fc, 0xff00ff, 0x7f7f7f, 0xd2d2d2, 0x470000, 0x472100,
                        0x474700, 0x324700, 0x004700, 0x00472c, 0x004747, 0x002747, 0x000047, 0x2e0047, 0x470047,
                        0x47002a, 0x740000, 0x743a00, 0x747400, 0x517400, 0x007400, 0x007449, 0x007474, 0x004074,
                        0x000074, 0x4b0074, 0x740074, 0x740045, 0xb50000, 0xb56300, 0xb5b500, 0x7db500, 0x00b500,
                        0x00b571, 0x00b5b5, 0x0063b5, 0x0000b5, 0x7500b5, 0xb500b5, 0xb5006b, 0xff0000, 0xff8c00,
                        0xffff00, 0xb2ff00, 0x00ff00, 0x00ffa0, 0x00ffff, 0x008cff, 0x0000ff, 0xa500ff, 0xff00ff,
                        0xff0098, 0xff5959, 0xffb459, 0xffff71, 0xcfff60, 0x6fff6f, 0x65ffc9, 0x6dffff, 0x59b4ff,
                        0x5959ff, 0xc459ff, 0xff66ff, 0xff59bc, 0xff9c9c, 0xffd39c, 0xffff9c, 0xe2ff9c, 0x9cff9c,
                        0x9cffdb, 0x9cffff, 0x9cd3ff, 0x9c9cff, 0xdc9cff, 0xff9cff, 0xff94d3, 0x000000, 0x131313,
                        0x282828, 0x363636, 0x4d4d4d, 0x656565, 0x818181, 0x9f9f9f, 0xbcbcbc, 0xe2e2e2, 0xffffff,
                };

                bool isDigit(char c) {
                        return c >= '0' && c <= '9';
                }

                int hexValue(char c) {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                }

                // Six hex digits at @p at of @p s: their value, or false.
                bool readRgb(StringView s, size_t at, uint32_t &out) {
                        if (at + 6 > s.size()) return false;
                        uint32_t v = 0;
                        for (size_t i = 0; i < 6; ++i) {
                                const int h = hexValue(s[at + i]);
                                if (h < 0) return false;
                                v = (v << 4) | uint32_t(h);
                        }
                        out = v;
                        return true;
                }

                // One or two digits at @p at: a colour index (99 and up: none), how many read.
                size_t readIndex(StringView s, size_t at, Colour &out) {
                        size_t n = 0;
                        int    v = 0;
                        while (n < 2 && at + n < s.size() && isDigit(s[at + n])) v = v * 10 + (s[at + n++] - '0');
                        if (n == 0) return 0;
                        out = v < int(PaletteSize) ? Colour{Colour::Kind::Index, uint8_t(v), 0} : Colour{};
                        return n;
                }

                // Where a link may start: the text's start, or after one of these.
                bool opensWord(char c) {
                        return c == ' ' || c == '(' || c == '<' || c == '[' || c == '"' || c == '\'';
                }

                // What ends a link.
                bool endsLink(char c) {
                        return static_cast<unsigned char>(c) <= 0x20 || c == '<' || c == '>' || c == '"' || c == '`';
                }

                bool startsWithNoCase(StringView s, size_t at, const char *prefix) {
                        const size_t n = std::strlen(prefix);
                        if (at + n > s.size()) return false;
                        for (size_t i = 0; i < n; ++i) {
                                char c = s[at + i];
                                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                                if (c != prefix[i]) return false;
                        }
                        return true;
                }

                void findLinks(Parsed &p) {
                        static const char *const Schemes[] = {"https://", "http://", "ircs://", "irc://", "www."};
                        const StringView         t = p.text.view();
                        for (size_t i = 0; i < t.size() && p.linkCount < MaxLinks;) {
                                if (i > 0 && !opensWord(t[i - 1])) {
                                        ++i;
                                        continue;
                                }
                                size_t scheme = 0;
                                for (const char *s : Schemes)
                                        if (startsWithNoCase(t, i, s)) {
                                                scheme = std::strlen(s);
                                                break;
                                        }
                                if (scheme == 0) {
                                        ++i;
                                        continue;
                                }
                                size_t end = i + scheme;
                                while (end < t.size() && !endsLink(t[end])) ++end;
                                // Trailing punctuation isn't the link's, nor a bracket it didn't open.
                                for (;;) {
                                        if (end <= i + scheme) break;
                                        const char c = t[end - 1];
                                        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' ||
                                            c == '\'') {
                                                --end;
                                                continue;
                                        }
                                        if (c == ')' || c == ']') {
                                                const char open = c == ')' ? '(' : '[';
                                                int        depth = 0;
                                                for (size_t k = i; k < end; ++k)
                                                        depth += t[k] == open ? 1 : t[k] == c ? -1 : 0;
                                                if (depth < 0) {
                                                        --end;
                                                        continue;
                                                }
                                        }
                                        break;
                                }
                                if (end > i + scheme) {
                                        p.links[p.linkCount++] = Link{uint32_t(i), uint32_t(end - i)};
                                        i = end;
                                } else {
                                        i += scheme;
                                }
                        }
                        return;
                }

                // The runs cut where links start and end, each part told its link.
                void splitAtLinks(Parsed &p) {
                        if (p.linkCount == 0) return;
                        Run    out[MaxRuns];
                        size_t n = 0;
                        for (size_t r = 0; r < p.runCount; ++r) {
                                const Run     &run = p.runs[r];
                                uint32_t       at = run.start;
                                const uint32_t end = run.start + run.length;
                                while (at < end) {
                                        int      link = -1;
                                        uint32_t stop = end;
                                        for (size_t l = 0; l < p.linkCount; ++l) {
                                                const Link &k = p.links[l];
                                                if (at >= k.start && at < k.start + k.length) {
                                                        link = int(l);
                                                        stop = k.start + k.length < end ? k.start + k.length : end;
                                                        break;
                                                }
                                                if (k.start > at && k.start < stop) stop = k.start;
                                        }
                                        if (n == MaxRuns) {
                                                out[n - 1].length =
                                                        end - out[n - 1].start; // full: the last takes the rest
                                                break;
                                        }
                                        Run part = run;
                                        part.start = at;
                                        part.length = stop - at;
                                        part.link = int16_t(link);
                                        out[n++] = part;
                                        at = stop;
                                }
                        }
                        std::memcpy(p.runs, out, n * sizeof(Run));
                        p.runCount = n;
                        return;
                }

        } // namespace

        uint32_t paletteColour(uint8_t index) {
                return index < PaletteSize ? Palette[index] : 0;
        }

        Error parse(StringView line, Parsed &out) {
                out.text.clear();
                out.gutter = NoGutter;
                out.runCount = 0;
                out.linkCount = 0;
                if (Error err = out.text.reserve(line.size()); err.isError()) return err;
                Style    cur;
                uint32_t runStart = 0;
                // A new style from here: the run so far closed if it has text (none past MaxRuns).
                auto restyle = [&](const Style &next) {
                        if (next == cur) return;
                        const uint32_t size = uint32_t(out.text.size());
                        if (size > runStart) {
                                if (out.runCount + 1 >= MaxRuns) return;
                                out.runs[out.runCount++] = Run{runStart, size - runStart, cur, -1};
                                runStart = size;
                        }
                        cur = next;
                        return;
                };
                size_t i = 0;
                while (i < line.size()) {
                        const char c = line[i];
                        Style      s = cur;
                        switch (c) {
                                case '\x02': s.flags ^= Bold; break;
                                case '\x1d': s.flags ^= Italic; break;
                                case '\x1f': s.flags ^= Underline; break;
                                case '\x1e': s.flags ^= Strike; break;
                                case '\x11': s.flags ^= Monospace; break;
                                case '\x16': s.flags ^= Reverse; break;
                                case '\x0f': s = Style{}; break;
                                case '\x03': {
                                        Colour       fg;
                                        const size_t n = readIndex(line, i + 1, fg);
                                        if (n == 0) {
                                                s.fg = s.bg = Colour{};
                                                break;
                                        }
                                        s.fg = fg;
                                        i += n;
                                        if (i + 2 < line.size() && line[i + 1] == ',' && isDigit(line[i + 2])) {
                                                Colour bg;
                                                i += 1 + readIndex(line, i + 2, bg);
                                                s.bg = bg;
                                        }
                                        break;
                                }
                                case '\x04': {
                                        uint32_t rgb = 0;
                                        if (!readRgb(line, i + 1, rgb)) {
                                                s.fg = s.bg = Colour{};
                                                break;
                                        }
                                        s.fg = Colour{Colour::Kind::Rgb, 0, rgb};
                                        i += 6;
                                        if (i + 1 < line.size() && line[i + 1] == ',' && readRgb(line, i + 2, rgb)) {
                                                s.bg = Colour{Colour::Kind::Rgb, 0, rgb};
                                                i += 7;
                                        }
                                        break;
                                }
                                case '\t':
                                        if (out.gutter == NoGutter) {
                                                out.gutter = uint32_t(out.text.size());
                                        } else if (Error err = out.text.append(" "); err.isError()) {
                                                return err;
                                        }
                                        ++i;
                                        continue;
                                default:
                                        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
                                                ++i; // other control characters: nothing
                                                continue;
                                        }
                                        if (Error err = out.text.append(line.mid(i, 1)); err.isError()) return err;
                                        ++i;
                                        continue;
                        }
                        restyle(s);
                        ++i;
                }
                const uint32_t size = uint32_t(out.text.size());
                if (size > runStart) out.runs[out.runCount++] = Run{runStart, size - runStart, cur, -1};
                findLinks(out);
                splitAtLinks(out);
                return Error::Ok;
        }

        Error plain(StringView line, String &out) {
                out.clear();
                Parsed p;
                if (Error err = parse(line, p); err.isError()) return err;
                const StringView t = p.text.view();
                if (p.gutter == NoGutter) return out.append(t);
                Error err = out.append(t.left(p.gutter));
                if (err.isOk()) err = out.append("\t");
                if (err.isOk()) err = out.append(t.mid(p.gutter));
                return err;
        }

        int linkAt(const Parsed &line, uint32_t offset) {
                for (size_t l = 0; l < line.linkCount; ++l)
                        if (offset >= line.links[l].start && offset < line.links[l].start + line.links[l].length)
                                return int(l);
                return -1;
        }

        namespace {

                // Wrapping's work on one line.
                struct Wrapper {
                                const Parsed  &p;
                                const Measure &m;
                                Layout        *out;
                                size_t         rows = 0;

                                StringView slice(uint32_t a, uint32_t b) const { return p.text.view().mid(a, b - a); }

                                // The run holding byte @p at (the last if past the end).
                                size_t runAt(uint32_t at) const {
                                        for (size_t r = 0; r < p.runCount; ++r)
                                                if (at < p.runs[r].start + p.runs[r].length) return r;
                                        return p.runCount == 0 ? 0 : p.runCount - 1;
                                }

                                // [a, b)'s width, run by run.
                                uint32_t width(uint32_t a, uint32_t b) const {
                                        uint32_t w = 0;
                                        for (size_t r = runAt(a); a < b && r < p.runCount; ++r) {
                                                const uint32_t end = p.runs[r].start + p.runs[r].length;
                                                const uint32_t stop = end < b ? end : b;
                                                if (stop > a)
                                                        w += m.width(m.arg, slice(a, stop), p.runs[r].style.flags);
                                                a = stop;
                                        }
                                        return w;
                                }

                                // How many bytes of [a, b) fit in @p most, whole characters.
                                uint32_t fit(uint32_t a, uint32_t b, uint32_t most) const {
                                        const uint32_t from = a;
                                        for (size_t r = runAt(a); a < b && r < p.runCount; ++r) {
                                                const uint32_t end = p.runs[r].start + p.runs[r].length;
                                                const uint32_t stop = end < b ? end : b;
                                                const uint32_t w =
                                                        m.width(m.arg, slice(a, stop), p.runs[r].style.flags);
                                                if (w <= most) {
                                                        most -= w;
                                                        a = stop;
                                                        continue;
                                                }
                                                return a - from +
                                                       uint32_t(m.fitting(m.arg, slice(a, stop), p.runs[r].style.flags,
                                                                          most));
                                        }
                                        return a - from;
                                }

                                void newRow() {
                                        if (out != nullptr && out->rows != nullptr && rows < out->maxRows) {
                                                out->rows[rows] = Row{uint32_t(out->pieceCount), 0};
                                                out->rowCount = rows + 1;
                                        }
                                        ++rows;
                                        return;
                                }

                                // [a, b) placed at @p x on the current row, a piece a run; the new x.
                                uint32_t place(uint32_t a, uint32_t b, uint32_t x) {
                                        for (size_t r = runAt(a); a < b && r < p.runCount; ++r) {
                                                const uint32_t end = p.runs[r].start + p.runs[r].length;
                                                const uint32_t stop = end < b ? end : b;
                                                if (stop <= a) continue;
                                                const uint32_t w =
                                                        m.width(m.arg, slice(a, stop), p.runs[r].style.flags);
                                                const bool room = out != nullptr && out->pieces != nullptr &&
                                                                  out->pieceCount < out->maxPieces &&
                                                                  rows <= out->maxRows && out->rowCount == rows;
                                                if (room) {
                                                        out->pieces[out->pieceCount++] =
                                                                Piece{a, stop - a, uint16_t(r), x};
                                                        ++out->rows[rows - 1].pieceCount;
                                                }
                                                x += w;
                                                a = stop;
                                        }
                                        return x;
                                }
                };

                // The bytes of the UTF-8 character at @p at.
                uint32_t charLength(StringView t, uint32_t at) {
                        uint32_t n = 1;
                        while (at + n < t.size() && Utf8::isContinuation(uint8_t(t[at + n]))) ++n;
                        return n;
                }

        } // namespace

        size_t wrap(const Parsed &line, const Measure &measure, uint32_t width, uint32_t textX, Layout *out) {
                if (out != nullptr) {
                        out->rowCount = 0;
                        out->pieceCount = 0;
                }
                Wrapper          w{line, measure, out};
                const StringView t = line.text.view();
                const uint32_t   size = uint32_t(t.size());
                const uint32_t   indent = (line.gutter != NoGutter && textX * 3 <= width * 2) ? textX : 0;
                w.newRow();
                uint32_t x = 0;
                uint32_t pos = 0;
                if (line.gutter != NoGutter) {
                        x = w.place(0, line.gutter, 0);
                        x = x > indent ? x : indent;
                        pos = line.gutter;
                }
                const uint32_t rowStart = indent;
                bool           rowEmpty = line.gutter == NoGutter;
                while (pos < size) {
                        uint32_t wordEnd = pos;
                        while (wordEnd < size && t[wordEnd] != ' ') ++wordEnd;
                        uint32_t spaceEnd = wordEnd;
                        while (spaceEnd < size && t[spaceEnd] == ' ') ++spaceEnd;
                        if (wordEnd > pos) {
                                const uint32_t ww = w.width(pos, wordEnd);
                                // To the next row if it fits there; a word no row holds is broken
                                // from here, rather than leaving this row (a gutter's) bare.
                                const bool fitsARow = ww <= (width > rowStart ? width - rowStart : 0);
                                if (!rowEmpty && x + ww > width && fitsARow) {
                                        w.newRow();
                                        x = rowStart;
                                        rowEmpty = true;
                                }
                                // A word wider than a row: broken where it reaches the edge.
                                while (x + w.width(pos, wordEnd) > width) {
                                        uint32_t n = w.fit(pos, wordEnd, width > x ? width - x : 0);
                                        if (n == 0 && rowEmpty) n = charLength(t, pos); // something on every row
                                        if (n > 0) {
                                                x = w.place(pos, pos + n, x);
                                                pos += n;
                                                rowEmpty = false;
                                        }
                                        if (pos >= wordEnd) break;
                                        w.newRow();
                                        x = rowStart;
                                        rowEmpty = true;
                                }
                                if (pos < wordEnd) {
                                        x = w.place(pos, wordEnd, x);
                                        rowEmpty = false;
                                }
                                pos = wordEnd;
                        }
                        if (spaceEnd > pos) {
                                const uint32_t sw = w.width(pos, spaceEnd);
                                if (x + sw <= width || rowEmpty) {
                                        x = w.place(pos, spaceEnd, x);
                                } else if (spaceEnd < size) {
                                        // The break: its spaces dropped.
                                        w.newRow();
                                        x = rowStart;
                                        rowEmpty = true;
                                }
                                pos = spaceEnd;
                        }
                }
                return w.rows;
        }

        uint32_t hit(const Parsed &line, const Measure &measure, const Layout &layout, size_t row, int32_t x) {
                if (row >= layout.rowCount) return uint32_t(line.text.size());
                const Row &r = layout.rows[row];
                if (r.pieceCount == 0) {
                        // An empty row: the next row's start, or the end.
                        for (size_t k = row + 1; k < layout.rowCount; ++k)
                                if (layout.rows[k].pieceCount > 0)
                                        return layout.pieces[layout.rows[k].firstPiece].start;
                        return uint32_t(line.text.size());
                }
                for (uint32_t k = 0; k < r.pieceCount; ++k) {
                        const Piece     &p = layout.pieces[r.firstPiece + k];
                        const StringView s = line.text.view().mid(p.start, p.length);
                        const uint8_t    flags = line.runs[p.run].style.flags;
                        if (x < int32_t(p.x)) return p.start;
                        const uint32_t w = measure.width(measure.arg, s, flags);
                        if (x < int32_t(p.x + w)) {
                                const uint32_t into = uint32_t(x) - p.x;
                                size_t         n = measure.fitting(measure.arg, s, flags, into);
                                // The nearer edge of the character under x.
                                if (n < s.size()) {
                                        const uint32_t next = n + charLength(s, uint32_t(n));
                                        const uint32_t left = measure.width(measure.arg, s.left(n), flags);
                                        const uint32_t right = measure.width(measure.arg, s.left(next), flags);
                                        if (into - left > right - into) n = next;
                                }
                                return p.start + uint32_t(n);
                        }
                }
                const Piece &last = layout.pieces[r.firstPiece + r.pieceCount - 1];
                return last.start + last.length;
        }

} // namespace ChatText

// ---- ChatLines ------------------------------------------------------------------------------

ChatLines::ChatLines(size_t maxLines) {
        (void)setMaxLines(maxLines);
}

ChatLines::~ChatLines() {
        for (size_t i = 0; i < _capacity; ++i) _lines[i].~String();
        Memory::free(_lines);
        Memory::free(_rows);
        Memory::free(_starts);
}

Error ChatLines::setMaxLines(size_t maxLines) {
        if (maxLines == 0) maxLines = 1;
        auto *lines = static_cast<String *>(Memory::alloc(MemSpace::Psram, maxLines * sizeof(String)));
        auto *rows = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, maxLines * sizeof(uint32_t)));
        auto *starts = static_cast<uint64_t *>(Memory::alloc(MemSpace::Psram, maxLines * sizeof(uint64_t)));
        if (lines == nullptr || rows == nullptr || starts == nullptr) {
                Memory::free(lines);
                Memory::free(rows);
                Memory::free(starts);
                return Error::NoMem;
        }
        for (size_t i = 0; i < maxLines; ++i) ::new (&lines[i]) String();
        // The newest that fit, moved over in order.
        while (_count > maxLines) dropOldest();
        for (size_t i = 0; i < _count; ++i) {
                const size_t from = slot(_first + i);
                lines[i] = std::move(_lines[from]);
                rows[i] = _rows[from];
                starts[i] = _starts[from];
        }
        for (size_t i = 0; i < _capacity; ++i) _lines[i].~String();
        Memory::free(_lines);
        Memory::free(_rows);
        Memory::free(_starts);
        _lines = lines;
        _rows = rows;
        _starts = starts;
        _capacity = maxLines;
        _head = 0;
        return Error::Ok;
}

void ChatLines::dropOldest() {
        _lines[_head] = String(); // its memory back, not only emptied
        _head = (_head + 1) % _capacity;
        ++_first;
        --_count;
        return;
}

Error ChatLines::append(StringView line, uint32_t rows) {
        if (_capacity == 0) return Error::NoMem;
        if (_count == _capacity) dropOldest();
        const size_t s = (_head + _count) % _capacity;
        _lines[s].clear();
        if (Error err = _lines[s].append(line); err.isError()) return err;
        _rows[s] = rows;
        _starts[s] = _end;
        _end += rows;
        ++_count;
        return Error::Ok;
}

void ChatLines::clear() {
        while (_count > 0) dropOldest();
        return;
}

StringView ChatLines::line(uint64_t number) const {
        return has(number) ? _lines[slot(number)].view() : StringView();
}

uint32_t ChatLines::rows(uint64_t number) const {
        return has(number) ? _rows[slot(number)] : 0;
}

void ChatLines::setRows(uint64_t number, uint32_t rows) {
        if (has(number)) _rows[slot(number)] = rows;
        return;
}

void ChatLines::restack() {
        uint64_t at = startOf(_first);
        for (uint64_t n = _first; n < _first + _count; ++n) {
                _starts[slot(n)] = at;
                at += _rows[slot(n)];
        }
        _end = at;
        return;
}

uint64_t ChatLines::startOf(uint64_t number) const {
        if (!has(number)) return _end;
        return _starts[slot(number)];
}

uint64_t ChatLines::lineAtRow(uint64_t row, uint32_t &within) const {
        within = 0;
        if (_count == 0) return next();
        const uint64_t target = startOf(_first) + row;
        uint64_t       lo = _first, hi = _first + _count - 1;
        while (lo < hi) {
                const uint64_t mid = lo + (hi - lo + 1) / 2;
                if (startOf(mid) <= target)
                        lo = mid;
                else
                        hi = mid - 1;
        }
        const uint64_t into = target - startOf(lo);
        const uint32_t rows = _rows[slot(lo)];
        within = into < rows ? uint32_t(into) : (rows > 0 ? rows - 1 : 0);
        return lo;
}

BMDS_NAMESPACE_END
