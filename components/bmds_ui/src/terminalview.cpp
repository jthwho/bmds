/**
 * @file      terminalview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A terminal screen's rows as cells to draw (devplan/windows.md §4e).
 */

#include <bmds/terminalview.h>

BMDS_NAMESPACE_BEGIN

namespace TerminalView {

        uint32_t rgb(Color color, uint32_t fallback) {
                if (color.isDefault()) return fallback;
                const Color c = color.toRgb();
                return uint32_t(c.red()) << 16 | uint32_t(c.green()) << 8 | c.blue();
        }

        size_t row(const TerminalScreen &screen, uint16_t row, const WindowPalette &palette, int cursor, TextCell *out,
                   size_t max) {
                using Role = WindowPalette::Role;
                const uint32_t      text = palette.color(Role::TerminalText);
                const uint32_t      back = palette.color(Role::TerminalBackground);
                const TerminalCell *cells = screen.row(row);
                const uint16_t      columns = screen.columns();
                // The cursor on a wide character's tail: on the character.
                if (cursor > 0 && cursor < columns && cells[cursor].tail != 0) --cursor;
                size_t n = 0;
                for (uint16_t c = 0; c < columns && n < max; ++c) {
                        const TerminalCell &cell = cells[c];
                        if (cell.tail != 0) continue;
                        const uint8_t a = static_cast<uint8_t>(cell.attributes);
                        uint32_t      fg = rgb(cell.foreground, text);
                        uint32_t      bg = rgb(cell.background, back);
                        if ((a & TextStyle::Reverse) != 0) {
                                const uint32_t t = fg;
                                fg = bg;
                                bg = t;
                        }
                        if ((a & TextStyle::Dim) != 0) fg = (fg >> 1) & 0x7f7f7f;
                        if ((a & TextStyle::Invisible) != 0) fg = bg;
                        if (c == cursor) {
                                fg = bg;
                                bg = palette.color(Role::TerminalCursor);
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
