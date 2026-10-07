/**
 * @file      switcherlayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Alt+Tab's box: a grid of icons and a title (devplan/gui.md §4n).
 */

#include <bmds/switcherlayout.h>

BMDS_NAMESPACE_BEGIN

void SwitcherLayout::layout(size_t count, uint16_t maxWidth) {
        _count = count > 0 ? count : 1;
        // As many columns as fit (8 at most), the rows filled from the top.
        size_t fit = maxWidth > 2 * Margin ? size_t(maxWidth - 2 * Margin) / Cell : 1;
        if (fit == 0) fit = 1;
        if (fit > MaxColumns) fit = MaxColumns;
        _columns = _count < fit ? _count : fit;
        const size_t rows = (_count + _columns - 1) / _columns;
        uint32_t     width = uint32_t(_columns) * Cell + 2u * Margin;
        if (width < MinWidth) width = MinWidth;
        if (width > maxWidth) width = maxWidth;
        _width = static_cast<uint16_t>(width & ~3u);
        // The grid centred, on a 4-pixel column.
        const uint32_t grid = uint32_t(_columns) * Cell;
        _left = static_cast<uint16_t>(grid < _width ? ((_width - grid) / 2) & ~3u : 0);
        _height = static_cast<uint16_t>(Margin + rows * Cell + Gap + TitleHeight + Margin);
        return;
}

Rect SwitcherLayout::cellRect(size_t i) const {
        if (i >= _count) return Rect();
        return Rect{static_cast<uint16_t>(_left + (i % _columns) * Cell),
                    static_cast<uint16_t>(Margin + (i / _columns) * Cell), Cell, Cell};
}

Rect SwitcherLayout::iconRect(size_t i) const {
        const Rect c = cellRect(i);
        if (c.isEmpty()) return c;
        constexpr uint16_t Inset = (Cell - IconSide) / 2;
        return Rect{static_cast<uint16_t>(c.x + Inset), static_cast<uint16_t>(c.y + Inset), IconSide, IconSide};
}

Rect SwitcherLayout::titleRect() const {
        const size_t rows = (_count + _columns - 1) / _columns;
        return Rect{Margin, static_cast<uint16_t>(Margin + rows * Cell + Gap),
                    static_cast<uint16_t>(_width - 2 * Margin), TitleHeight};
}

int SwitcherLayout::cellAt(int x, int y) const {
        for (size_t i = 0; i < _count; ++i) {
                const Rect c = cellRect(i);
                if (x >= c.x && x < int(c.right()) && y >= c.y && y < int(c.bottom())) return int(i);
        }
        return -1;
}

BMDS_NAMESPACE_END
