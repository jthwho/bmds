/**
 * @file      menulayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/menulayout.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t roundUp4(uint32_t v) {
                return (v + 3) & ~3u;
        }

        // [lo, hi - size], or lo when it doesn't fit.
        int32_t clampInto(int32_t at, uint32_t size, int32_t lo, int32_t hi) {
                if (at + int32_t(size) > hi) at = hi - int32_t(size);
                if (at < lo) at = lo;
                return at;
        }

} // namespace

Error MenuLayout::layout(Span<const Item> items, const Font &font, uint16_t minWidth) {
        _count = 0;
        if (items.size() > MaxItems) return Error::Full;
        uint32_t textWidth = 0;
        uint32_t shortcutWidth = 0;
        bool     submenus = false;
        for (const Item &item : items) {
                if ((item.flags & Separator) != 0) continue;
                const uint32_t t = font.textWidth(item.text);
                const uint32_t s = font.textWidth(item.shortcut);
                if (t > textWidth) textWidth = t;
                if (s > shortcutWidth) shortcutWidth = s;
                submenus = submenus || (item.flags & Submenu) != 0;
        }
        uint32_t width = TextX + roundUp4(textWidth);
        if (shortcutWidth != 0) width += ShortcutGap + roundUp4(shortcutWidth);
        if (submenus) width += ShortcutGap / 2 + ArrowWidth;
        width = roundUp4(width + RightPad);
        if (width < minWidth) width = roundUp4(minWidth);
        if (width > UINT16_MAX) width = UINT16_MAX & ~3u;
        _width = static_cast<uint16_t>(width);
        uint32_t y = InsetY;
        for (const Item &item : items) {
                _top[_count] = static_cast<uint16_t>(y);
                _flags[_count] = item.flags;
                // Right-aligned to the shortcut column's end, on a 4-pixel column.
                const uint32_t s = roundUp4(font.textWidth(item.shortcut));
                const uint32_t end = _width - RightPad - (submenus ? ShortcutGap / 2 + ArrowWidth : 0);
                _shortcutX[_count] = static_cast<uint16_t>(s <= end ? end - s : 0);
                y += (item.flags & Separator) != 0 ? SeparatorHeight : ItemHeight;
                ++_count;
        }
        _height = static_cast<uint16_t>(y + InsetY);
        return Error();
}

Rect MenuLayout::itemRect(size_t i) const {
        if (i >= _count) return Rect();
        const uint16_t h = (_flags[i] & Separator) != 0 ? SeparatorHeight : ItemHeight;
        return Rect{Edge, _top[i], static_cast<uint16_t>(_width - 2 * Edge), h};
}

uint16_t MenuLayout::shortcutX(size_t i) const {
        return i < _count ? _shortcutX[i] : 0;
}

int MenuLayout::itemAt(int x, int y) const {
        if (x < Edge || x >= _width - Edge) return -1;
        // Few items: a walk.
        for (size_t i = 0; i < _count; ++i) {
                const Rect r = itemRect(i);
                if (y >= r.y && uint32_t(y) < r.bottom()) return static_cast<int>(i);
        }
        return -1;
}

int MenuLayout::next(int from, int step) const {
        if (_count == 0) return -1;
        const int n = static_cast<int>(_count);
        int       i = from;
        if (i < 0 || i >= n) i = step > 0 ? -1 : n;
        for (int k = 0; k < n; ++k) {
                i = ((i + step) % n + n) % n;
                if (isSelectable(static_cast<size_t>(i))) return i;
        }
        return -1;
}

Rect MenuLayout::place(uint16_t width, uint16_t height, const Rect &anchor, const Rect &area, bool submenu) {
        const int32_t left = area.x;
        const int32_t top = area.y;
        const int32_t right = static_cast<int32_t>(area.right());
        const int32_t bottom = static_cast<int32_t>(area.bottom());
        int32_t       x;
        int32_t       y;
        if (submenu) {
                // Beside the parent item, its first item level with it.
                x = static_cast<int32_t>(anchor.right());
                if (x + width > right) x = anchor.x - int32_t(width);
                y = anchor.y - InsetY;
        } else {
                x = anchor.x;
                y = static_cast<int32_t>(anchor.bottom());
                if (y + height > bottom && anchor.y - int32_t(height) >= top) y = anchor.y - int32_t(height);
        }
        x = clampInto(x, width, left, right) & ~3;
        if (x < left) x += 4; // area.x is on a column in practice; never left of it
        y = clampInto(y, height, top, bottom);
        const uint32_t w = width < area.width ? width : area.width;
        const uint32_t h = height < area.height ? height : area.height;
        return Rect{static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(w),
                    static_cast<uint16_t>(h)};
}

BMDS_NAMESPACE_END
