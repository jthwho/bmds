/**
 * @file      desktopwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel's desktop window and its icons (devplan/gui.md §4m).
 */

#include <cstring>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/memory.h>
#include <bmds/system.h>

#include "desktopwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t Pad = 4;        // around the grid, and in a cell
        constexpr uint16_t LineWidth = 72; // a name's line at most (9 characters)
        constexpr uint16_t LineHeight = 16;
        constexpr uint16_t TextTop = Pad + DesktopWindow::IconSide + 4;

        // @p n bytes of @p text into @p out, NUL-ended.
        void copyLine(char (&out)[DesktopWindow::LineBytes], const char *text, size_t n) {
                if (n >= sizeof(out)) n = sizeof(out) - 1;
                std::memcpy(out, text, n);
                out[n] = '\0';
                return;
        }

        // @p name's lines: the first broken at its last space that fits (or cut, one long word),
        // the second what is left; if that doesn't fit either, cut and ended with an ellipsis.
        void breakName(StringView all, char (&text)[2][DesktopWindow::LineBytes], uint8_t &lines) {
                constexpr char Ellipsis[] = "\xe2\x80\xa6"; // U+2026
                const Font    &font = Fonts::unscii16;
                const size_t   fit = font.fitting(all, LineWidth);
                lines = all.isEmpty() ? 0 : 1;
                if (fit >= all.size()) {
                        copyLine(text[0], all.data(), all.size());
                        return;
                }
                size_t cut = fit;
                if (all[fit] != ' ') {
                        size_t space = fit;
                        while (space > 0 && all[space] != ' ') --space;
                        if (space > 0) cut = space;
                }
                copyLine(text[0], all.data(), cut);
                size_t next = cut;
                while (next < all.size() && all[next] == ' ') ++next;
                if (next >= all.size()) return;
                lines = 2;
                const StringView rest = all.mid(next);
                if (font.textWidth(rest) <= LineWidth) {
                        copyLine(text[1], rest.data(), rest.size());
                        return;
                }
                const size_t kept = font.fitting(rest, LineWidth - font.textWidth(StringView(Ellipsis)));
                copyLine(text[1], rest.data(), kept);
                const size_t at = std::strlen(text[1]); // at most 8 characters: room for the ellipsis
                std::memcpy(text[1] + at, Ellipsis, sizeof(Ellipsis));
                return;
        }

        // @p rgb into a reference picture's B, G, R pixel at @p p.
        void put(uint8_t *p, uint32_t rgb) {
                p[0] = static_cast<uint8_t>(rgb);
                p[1] = static_cast<uint8_t>(rgb >> 8);
                p[2] = static_cast<uint8_t>(rgb >> 16);
                return;
        }

        void fillReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height, const Rect &r,
                           uint32_t rgb) {
                for (uint32_t y = r.y; y < r.bottom() && y < height; ++y)
                        for (uint32_t x = r.x; x < r.right() && x < width; ++x) put(pixels + y * stride + x * 3, rgb);
                return;
        }

} // namespace

DesktopWindow::~DesktopWindow() {
        stopRename();
        freeItems();
}

Rect DesktopWindow::nameRect(size_t item) const {
        if (item >= _count) return Rect();
        const Rect  s = screenRect();
        const Item &it = _items[item];
        return Rect{uint16_t(s.x + it.cell.x), uint16_t(s.y + it.cell.y + TextTop - 3), CellWidth,
                    GuiStyle::ButtonHeight};
}

// A rename asked for in @p ms unless something else happens first.
void DesktopWindow::startRename(int item, uint32_t ms) {
        stopRename();
        if (_renameAsked == nullptr || item < 0) return;
        auto [timer, err] = application().startTimer(ms, false, renameDue, this);
        if (err.isError()) return;
        _renameTimer = timer;
        _renameItem = item;
        return;
}

void DesktopWindow::stopRename() {
        if (_renameTimer >= 0) application().stopTimer(_renameTimer);
        _renameTimer = -1;
        _renameItem = -1;
        return;
}

void DesktopWindow::renameDue(void *arg) {
        auto     *self = static_cast<DesktopWindow *>(arg);
        const int item = self->_renameItem;
        self->_renameTimer = -1;
        self->_renameItem = -1;
        if (self->_dragging) return;
        self->_pressed = -1; // a long press: its release does nothing now
        if (item >= 0 && size_t(item) < self->_count && self->_renameAsked != nullptr)
                self->_renameAsked(self->_renameArg, size_t(item));
        return;
}

void DesktopWindow::freeItems() {
        for (size_t i = 0; i < _count; ++i) Memory::free(_items[i].pixels);
        Memory::free(_items);
        _items = nullptr;
        _count = 0;
        _selected = -1;
        return;
}

Error DesktopWindow::setItems(Span<const StringView> names, Span<const IconGrid::Cell> wanted, const Rect &area) {
        if (_count > 0) invalidate(geometry());
        freeItems();
        const size_t n = names.size() < MaxItems ? names.size() : MaxItems;
        if (n == 0) return Error();
        _items = static_cast<Item *>(Memory::alloc(MemSpace::Psram, sizeof(Item) * n));
        if (_items == nullptr) return Error::NoMem;
        // The grid: as many cells as the area holds, from its corner.
        _left = static_cast<uint16_t>((area.x + Pad + 3) & ~3u);
        _top = static_cast<uint16_t>(area.y + Pad);
        _rows = area.height > 2 * Pad ? static_cast<uint16_t>((area.height - 2 * Pad) / CellHeight) : 1;
        _columns = area.width > 2 * Pad ? static_cast<uint16_t>((area.width - 2 * Pad) / CellWidth) : 1;
        if (_rows == 0) _rows = 1;
        if (_columns == 0) _columns = 1;
        IconGrid::Cell want[MaxItems], at[MaxItems];
        for (size_t i = 0; i < n; ++i) want[i] = wanted.size() == names.size() ? wanted[i] : IconGrid::Cell();
        IconGrid::place(Span<const IconGrid::Cell>(want, n), _columns, _rows, Span<IconGrid::Cell>(at, n));
        for (size_t i = 0; i < n; ++i) {
                Item &item = _items[i];
                std::memset(static_cast<void *>(&item), 0, sizeof(item));
                breakName(names[i], item.text, item.lines);
                item.at = at[i];
                item.cell = cellRect(at[i]);
                for (uint8_t l = 0; l < item.lines; ++l) {
                        const uint32_t w = Fonts::unscii16.textWidth(StringView(item.text[l]));
                        item.textX[l] = static_cast<uint16_t>(((CellWidth - w) / 2) & ~3u);
                }
        }
        _count = n;
        invalidate(geometry());
        return Error();
}

void DesktopWindow::setItemIcon(size_t item, const Icon *icon) {
        if (item >= _count) return;
        Item &it = _items[item];
        Memory::free(it.pixels);
        it.pixels = nullptr;
        if (icon != nullptr && icon->isValid() && icon->width <= IconSide && icon->height <= IconSide) {
                const size_t n = size_t(icon->width) * icon->height;
                it.pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                if (it.pixels != nullptr) {
                        std::memcpy(it.pixels, icon->pixels, n * sizeof(uint32_t));
                        it.iconWidth = icon->width;
                        it.iconHeight = icon->height;
                }
        }
        invalidate(it.cell);
        return;
}

Rect DesktopWindow::cellRect(IconGrid::Cell at) const {
        return Rect{uint16_t(_left + at.column * CellWidth), uint16_t(_top + at.row * CellHeight), CellWidth,
                    CellHeight};
}

void DesktopWindow::moveTo(Item &item, const Rect &cell) {
        if (cell == item.cell) return;
        invalidate(item.cell);
        item.cell = cell;
        invalidate(item.cell);
        return;
}

Rect DesktopWindow::iconRect(const Item &item) const {
        // Centred in the icon's square, on a 4-pixel column.
        const uint16_t x = static_cast<uint16_t>((item.cell.x + (CellWidth - item.iconWidth) / 2) & ~3u);
        const uint16_t y = static_cast<uint16_t>(item.cell.y + Pad + (IconSide - item.iconHeight) / 2);
        return Rect{x, y, item.iconWidth, item.iconHeight};
}

Rect DesktopWindow::lineRect(const Item &item, int line) const {
        const uint16_t w = static_cast<uint16_t>(Fonts::unscii16.textWidth(StringView(item.text[line])));
        return Rect{uint16_t(item.cell.x + item.textX[line] - Pad), uint16_t(item.cell.y + TextTop + line * LineHeight),
                    uint16_t(w + 2 * Pad), LineHeight};
}

Rect DesktopWindow::hitRect(const Item &item) const {
        return Rect{uint16_t(item.cell.x + Pad), uint16_t(item.cell.y + Pad), uint16_t(CellWidth - 2 * Pad),
                    uint16_t(TextTop - Pad + item.lines * LineHeight)};
}

int DesktopWindow::itemAt(int x, int y) const {
        for (size_t i = 0; i < _count; ++i) {
                const Rect r = hitRect(_items[i]);
                if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom()) return int(i);
        }
        return -1;
}

void DesktopWindow::select(int item) {
        if (item == _selected) return;
        if (_selected >= 0) invalidate(_items[_selected].cell);
        _selected = item;
        if (_selected >= 0) invalidate(_items[_selected].cell);
        return;
}

void DesktopWindow::mousePressed(const WindowManager::MouseEvent &event) {
        stopRename(); // a second press: a double click, not a slow one
        if (event.button == PointerEvent::Right) {
                const Rect s = screenRect();
                const int  item = itemAt(event.x, event.y);
                if (item >= 0 && _itemMenuAsked != nullptr) {
                        select(item);
                        _itemMenuAsked(_itemMenuArg, size_t(item), uint16_t(s.x + event.x), uint16_t(s.y + event.y));
                        return;
                }
                if (_menuAsked != nullptr) _menuAsked(_menuArg, uint16_t(s.x + event.x), uint16_t(s.y + event.y));
                return;
        }
        if (event.button != PointerEvent::Left) return;
        const int      item = itemAt(event.x, event.y);
        const uint64_t now = System::uptimeUs();
        if (item >= 0 && item == _selected && now - _pressUs <= uint64_t(WindowManager::DoubleClickMs) * 1000) {
                _pressUs = 0; // a third press starts again
                if (_opened != nullptr) _opened(_openedArg, size_t(item));
                return;
        }
        // On the selected item's name: its release, unless a double click follows, renames.
        _onSelectedName = false;
        if (item >= 0 && item == _selected) {
                const Item &it = _items[item];
                const int   top = it.cell.y + TextTop;
                _onSelectedName = event.y >= top && event.y < top + it.lines * LineHeight;
        }
        select(item);
        _pressUs = now;
        _pressed = item;
        _dragging = false;
        if (item >= 0) {
                _pressX = event.x - _items[item].cell.x;
                _pressY = event.y - _items[item].cell.y;
                startRename(item, LongPressMs); // held still that long: a rename
        }
        return;
}

// A press on an item moving: past `DragDistance`, the item follows the pointer.
void DesktopWindow::mouseMoved(const WindowManager::MouseEvent &event) {
        if (_pressed < 0 || (event.buttons & PointerEvent::Left) == 0) return;
        Item &item = _items[_pressed];
        int   x = event.x - _pressX;
        int   y = event.y - _pressY;
        if (!_dragging) {
                const int dx = x - item.cell.x, dy = y - item.cell.y;
                if (dx * dx + dy * dy <= DragDistance * DragDistance) return;
                stopRename();
                _dragging = true;
                _pressUs = 0; // no double click after a drag
        }
        const Rect &g = geometry();
        x = x < 0 ? 0 : (x + CellWidth > g.width ? g.width - CellWidth : x);
        y = y < 0 ? 0 : (y + CellHeight > g.height ? g.height - CellHeight : y);
        moveTo(item, Rect{uint16_t(x & ~3), uint16_t(y), CellWidth, CellHeight});
        return;
}

// A drag dropped: the item into the cell under its middle, the one there (if any) into its old one.
void DesktopWindow::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || _pressed < 0) return;
        const int  pressed = _pressed;
        const bool dragged = _dragging;
        _pressed = -1;
        _dragging = false;
        if (!dragged) {
                stopRename(); // released before the long press
                // A slow second click on its name: renamed once a double click can't come.
                if (_onSelectedName) startRename(pressed, WindowManager::DoubleClickMs + 50);
                _onSelectedName = false;
                return;
        }
        Item                &item = _items[pressed];
        const IconGrid::Cell to =
                IconGrid::cellAt(item.cell.x + CellWidth / 2 - _left, item.cell.y + CellHeight / 2 - _top, CellWidth,
                                 CellHeight, _columns, _rows);
        const IconGrid::Cell from = item.at;
        if (to != from) {
                for (size_t i = 0; i < _count; ++i) {
                        if (int(i) == pressed || _items[i].at != to) continue;
                        _items[i].at = from;
                        moveTo(_items[i], cellRect(from));
                }
                item.at = to;
        }
        moveTo(item, cellRect(item.at));
        if (to != from && _moved != nullptr) _moved(_movedArg);
        return;
}

// The press taken away (a popup opened): a drag under way goes back to its cell.
void DesktopWindow::mouseLeft() {
        stopRename();
        if (_pressed >= 0 && _dragging) moveTo(_items[_pressed], cellRect(_items[_pressed].at));
        _pressed = -1;
        _dragging = false;
        return;
}

void DesktopWindow::paint(Painter &painter) {
        const WindowPalette &palette = painter.palette();
        const uint32_t       desktop = palette.color(Role::Desktop);
        (void)painter.fill(geometry(), desktop);
        for (size_t k = 0; k < _count; ++k) {
                const size_t i = drawnAt(k);
                const Item  &item = _items[i];
                if (!painter.isVisible(item.cell)) continue;
                if (item.pixels != nullptr) {
                        const Rect r = iconRect(item);
                        (void)painter.image(r.x, r.y, Icon{item.iconWidth, item.iconHeight, item.pixels}, desktop);
                }
                const bool     selected = int(i) == _selected;
                const uint32_t fg = palette.color(selected ? Role::HighlightText : Role::DesktopText);
                const uint32_t bg = selected ? palette.color(Role::Highlight) : desktop;
                for (uint8_t l = 0; l < item.lines; ++l) {
                        const Rect r = lineRect(item, l);
                        if (selected) (void)painter.fill(r, bg);
                        (void)painter.text(uint16_t(item.cell.x + item.textX[l]), r.y, StringView(item.text[l]),
                                           Fonts::unscii16, fg, bg, LineWidth);
                }
        }
        return;
}

void DesktopWindow::drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                  const WindowPalette &palette) {
        const uint32_t desktop = palette.color(Role::Desktop);
        fillReference(pixels, stride, width, height, Rect{0, 0, width, height}, desktop);
        GlyphRenderer renderer(GlyphRenderer::Kernel::Scalar);
        for (size_t k = 0; k < _count; ++k) {
                const size_t i = drawnAt(k);
                const Item  &item = _items[i];
                if (item.cell.right() > width || item.cell.bottom() > height) continue; // not on the screen
                if (item.pixels != nullptr) {
                        const Rect r = iconRect(item);
                        for (uint16_t y = 0; y < r.height; ++y)
                                for (uint16_t x = 0; x < r.width; ++x)
                                        put(pixels + size_t(r.y + y) * stride + size_t(r.x + x) * 3,
                                            blendOver(item.pixels[size_t(y) * r.width + x], desktop));
                }
                const bool     selected = int(i) == _selected;
                const uint32_t fg = palette.color(selected ? Role::HighlightText : Role::DesktopText);
                const uint32_t bg = selected ? palette.color(Role::Highlight) : desktop;
                for (uint8_t l = 0; l < item.lines; ++l) {
                        const Rect r = lineRect(item, l);
                        if (selected) fillReference(pixels, stride, width, height, r, bg);
                        size_t used = 0;
                        (void)renderer.drawText(StringView(item.text[l]), Fonts::unscii16, fg, bg,
                                                pixels + size_t(r.y) * stride + size_t(item.cell.x + item.textX[l]) * 3,
                                                stride, LineWidth, used);
                }
        }
        return;
}

BMDS_NAMESPACE_END
