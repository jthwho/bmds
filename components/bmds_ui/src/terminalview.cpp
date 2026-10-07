/**
 * @file      terminalview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A terminal screen's rows as cells to draw (devplan/windows.md §4e), in a
 * terminal's colors (devplan/gui.md §4l).
 */

#include <bmds/terminalview.h>

BMDS_NAMESPACE_BEGIN

uint32_t TerminalColors::rgb(Color color, uint32_t fallback) const {
        if (color.isDefault()) return fallback;
        if (color.kind() == Color::Kind::Indexed && color.index() < 16) return ansi[color.index()];
        const Color c = color.toRgb();
        return uint32_t(c.red()) << 16 | uint32_t(c.green()) << 8 | c.blue();
}

Result<uint32_t> TerminalColors::parse(StringView value) {
        StringView v = value.trimmed();
        if (v.startsWith('#')) v = v.mid(1);
        if (v.size() != 6) return makeError<uint32_t>(Error::Invalid);
        uint32_t rgb = 0;
        for (const char ch : v) {
                const int digit = ch >= '0' && ch <= '9'   ? ch - '0'
                                  : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                  : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                           : -1;
                if (digit < 0) return makeError<uint32_t>(Error::Invalid);
                rgb = rgb << 4 | uint32_t(digit);
        }
        return makeResult(rgb);
}

uint32_t *TerminalColors::find(StringView key) {
        if (key.equalsIgnoreCase("Text")) return &text;
        if (key.equalsIgnoreCase("Background")) return &background;
        if (key.equalsIgnoreCase("Cursor")) return &cursor;
        if (key.size() <= 5 || key.size() > 7 || !key.left(5).equalsIgnoreCase("Color")) return nullptr;
        const StringView n = key.mid(5);
        if (n.size() == 2 && n[0] == '0') return nullptr; // Color0 to Color15, as written
        unsigned index = 0;
        for (const char ch : n) {
                if (ch < '0' || ch > '9') return nullptr;
                index = index * 10 + unsigned(ch - '0');
        }
        return index < 16 ? &ansi[index] : nullptr;
}

Error TerminalColors::set(StringView key, StringView value) {
        uint32_t *to = find(key);
        if (to == nullptr) return Error::NotFound;
        auto [rgb, err] = parse(value);
        if (err.isError()) return err;
        *to = rgb;
        return Error();
}

namespace TerminalView {

        size_t row(const TerminalScreen &screen, uint16_t row, const TerminalColors &colors, int cursor, TextCell *out,
                   size_t max) {
                return TerminalView::cells(screen.row(row), screen.columns(), screen.columns(), colors, cursor, out,
                                           max);
        }

        const TerminalCell *viewRow(const TerminalScreen &screen, size_t offset, uint16_t row, uint16_t &length) {
                const size_t history = screen.historyCount();
                const size_t at = history - (offset < history ? offset : history) + row; // in history, then screen
                if (at < history) return screen.historyRow(at, length);
                length = screen.columns();
                return screen.row(static_cast<uint16_t>(at - history));
        }

        int viewCursorRow(const TerminalScreen &screen, size_t offset) {
                const size_t history = screen.historyCount();
                const size_t shown = screen.cursorRow() + (offset < history ? offset : history);
                return screen.isCursorVisible() && shown < screen.rows() ? int(shown) : -1;
        }

        size_t cells(const TerminalCell *cells, uint16_t length, uint16_t columns, const TerminalColors &colors,
                     int cursor, TextCell *out, size_t max) {
                const uint32_t     text = colors.text;
                const uint32_t     back = colors.background;
                const TerminalCell blank;
                // The cursor on a wide character's tail: on the character.
                if (cursor > 0 && cursor < length && cursor < columns && cells[cursor].tail != 0) --cursor;
                size_t n = 0;
                for (uint16_t c = 0; c < columns && n < max; ++c) {
                        const bool          cut = c + 1 == columns && c < length && cells[c].wide != 0;
                        const TerminalCell &cell = c < length && !cut ? cells[c] : blank;
                        if (cell.tail != 0) continue;
                        const uint8_t a = static_cast<uint8_t>(cell.attributes);
                        uint32_t      fg = colors.rgb(cell.foreground, text);
                        uint32_t      bg = colors.rgb(cell.background, back);
                        if ((a & TextStyle::Reverse) != 0) {
                                const uint32_t t = fg;
                                fg = bg;
                                bg = t;
                        }
                        if ((a & TextStyle::Dim) != 0) fg = (fg >> 1) & 0x7f7f7f;
                        if ((a & TextStyle::Invisible) != 0) fg = bg;
                        if (c == cursor) {
                                fg = bg;
                                bg = colors.cursor;
                        }
                        TextCell &t = out[n++];
                        t.codepoint = cell.codepoint;
                        t.fg = fg;
                        t.bg = bg;
                        t.flags = static_cast<uint8_t>(((a & TextStyle::Bold) != 0 ? TextCell::Bold : 0) |
                                                       ((a & TextStyle::Underline) != 0 ? TextCell::Underline : 0));
                }
                return n;
        }

} // namespace TerminalView

BMDS_NAMESPACE_END
