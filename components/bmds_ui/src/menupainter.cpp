/**
 * @file      menupainter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A menu drawn from its layout (devplan/gui.md §4b): the toolkit's menus and
 * the window manager's window menu look the same.
 */

#include <bmds/bevel.h>
#include <bmds/font.h>
#include <bmds/menulayout.h>
#include <bmds/painter.h>

BMDS_NAMESPACE_BEGIN

void drawMenu(Painter &painter, const MenuLayout &layout, Span<const MenuLayout::Item> items, int highlight) {
        using Role = WindowPalette::Role;
        const WindowPalette &palette = painter.palette();
        const uint32_t       face = palette.color(Role::Face);
        const uint16_t       w = layout.width();
        const uint16_t       h = layout.height();
        const uint16_t       e = MenuLayout::Edge;
        // The bevel, and the face above and below the items.
        drawBevel(painter, Rect{0, 0, w, h}, Bevel::Style::Window);
        const uint16_t last = layout.count() > 0 ? static_cast<uint16_t>(layout.itemRect(layout.count() - 1).bottom())
                                                 : MenuLayout::InsetY;
        const Rect     margins[] = {{e, e, uint16_t(w - 2 * e), uint16_t(MenuLayout::InsetY - e)},
                                    {e, last, uint16_t(w - 2 * e), uint16_t(h - e - last)}};
        (void)painter.fill(Span<const Rect>(margins), face);
        for (size_t i = 0; i < layout.count() && i < items.size(); ++i) {
                const Rect row = layout.itemRect(i);
                if (!painter.isVisible(row)) continue;
                const uint8_t flags = layout.flags(i);
                if ((flags & MenuLayout::Separator) != 0) {
                        // Etched: a shaded line over a lit one.
                        const uint16_t mid = static_cast<uint16_t>(row.y + row.height / 2 - 1);
                        const uint16_t x = static_cast<uint16_t>(row.x + 1);
                        const uint16_t width = static_cast<uint16_t>(row.width - 2);
                        const Rect     faces[] = {{row.x, row.y, row.width, uint16_t(mid - row.y)},
                                                  {row.x, uint16_t(mid + 2), row.width, uint16_t(row.bottom() - mid - 2)},
                                                  {row.x, mid, 1, 2},
                                                  {uint16_t(row.right() - 1), mid, 1, 2}};
                        (void)painter.fill(Span<const Rect>(faces), face);
                        (void)painter.fill(Rect{x, mid, width, 1}, palette.color(Role::FaceShadow));
                        (void)painter.fill(Rect{x, uint16_t(mid + 1), width, 1}, palette.color(Role::FaceHighlight));
                        continue;
                }
                const bool     lit = int(i) == highlight;
                const uint32_t bg = lit ? palette.color(Role::Highlight) : face;
                const uint32_t fg = (flags & MenuLayout::Disabled) != 0 ? palette.color(Role::DisabledText)
                                    : lit                               ? palette.color(Role::HighlightText)
                                                                        : palette.color(Role::FaceText);
                (void)painter.fill(row, bg);
                const uint16_t ty = static_cast<uint16_t>(row.y + (row.height - Fonts::unscii16.height()) / 2);
                const uint16_t room = static_cast<uint16_t>(w - MenuLayout::TextX - MenuLayout::RightPad);
                (void)painter.text(MenuLayout::TextX, ty, items[i].text, Fonts::unscii16, fg, bg, room);
                if (!items[i].shortcut.isEmpty())
                        (void)painter.text(layout.shortcutX(i), ty, items[i].shortcut, Fonts::unscii16, fg, bg,
                                           static_cast<uint16_t>(w - layout.shortcutX(i) - MenuLayout::RightPad));
                TextCell mark;
                mark.fg = fg;
                mark.bg = bg;
                const uint16_t iy = static_cast<uint16_t>(row.y + (row.height - MenuLayout::IconSize) / 2);
                if ((flags & MenuLayout::Checked) != 0) {
                        mark.codepoint = Icon::Check;
                        (void)painter.cells(MenuLayout::IconX, iy, Span<const TextCell>(&mark, 1), Fonts::icons16,
                                            MenuLayout::IconSize);
                }
                if ((flags & MenuLayout::Submenu) != 0) {
                        // The arrow's picture is in its glyph's middle columns.
                        mark.codepoint = Icon::Submenu;
                        (void)painter.cells(static_cast<uint16_t>(layout.arrowX() - 4), iy,
                                            Span<const TextCell>(&mark, 1), Fonts::icons16, MenuLayout::IconSize);
                }
        }
        return;
}

BMDS_NAMESPACE_END
