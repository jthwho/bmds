/**
 * @file      scrollview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A column of widgets scrolled by whole rows (devplan/gui.md §4u).
 */

#include <bmds/formmodels.h>
#include <bmds/scrollview.h>

BMDS_NAMESPACE_BEGIN

ScrollView::ScrollView(Widget *parent) : ScrollArea(parent) {}

void ScrollView::setSpacing(uint16_t spacing) {
        _spacing = spacing;
        relayout();
        return;
}

void ScrollView::setMargins(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom) {
        _margins[0] = static_cast<uint16_t>((left + GuiStyle::Column - 1) & ~(GuiStyle::Column - 1));
        _margins[1] = top;
        _margins[2] = static_cast<uint16_t>((right + GuiStyle::Column - 1) & ~(GuiStyle::Column - 1));
        _margins[3] = bottom;
        relayout();
        return;
}

size_t ScrollView::rows(Widget *(&out)[MaxRows]) const {
        size_t n = 0;
        for (Widget *c = firstChild(); c != nullptr && n < MaxRows; c = c->next())
                if (!isBar(c) && c->isVisible()) out[n++] = c;
        return n;
}

void ScrollView::measure(uint16_t width) {
        Widget *list[MaxRows];
        _count = rows(list);
        layingOut(width);
        uint32_t y = _margins[1];
        for (size_t i = 0; i < _count; ++i) {
                _tops[i] = y;
                _stops[i] = i == 0 ? 0 : y - _margins[1]; // a row at the top keeps the margin above it
                _heights[i] = list[i]->heightForWidth(width);
                y += _heights[i] + (i + 1 < _count ? _spacing : 0);
        }
        _total = y + _margins[3];
        return;
}

void ScrollView::relayout() {
        // The bar's room changes the width, which may change the heights, which may change whether
        // the bar is needed: twice settles it.
        for (int pass = 0; pass < 2; ++pass) {
                const Rect     v = viewport();
                const uint32_t side = uint32_t(_margins[0]) + _margins[2];
                measure(static_cast<uint16_t>(v.width > side ? v.width - side : 0));
                _last = RowScroll::lastTop(Span<const uint32_t>(_stops, _count), _total, v.height);
                const uint32_t most = _count > 0 ? stop(_last) + v.height : 0;
                setContentSize(1, _total > most ? _total : most);
                if (viewport().width == v.width) break;
        }
        if (_top > _last) _top = _last;
        place();
        scrollTo(scrollX(), _count > 0 ? stop(_top) : 0);
        update();
        return;
}

void ScrollView::place() {
        Widget        *list[MaxRows];
        const size_t   n = rows(list);
        const Rect     v = viewport();
        const uint32_t side = uint32_t(_margins[0]) + _margins[2];
        const auto     x = static_cast<uint16_t>(v.x + _margins[0]);
        const auto     width = static_cast<uint16_t>(v.width > side ? v.width - side : 0);
        const uint32_t offset = _count > 0 ? stop(_top) : 0;
        for (size_t i = 0; i < n && i < _count; ++i) {
                // Above the top row: whole, out of sight below the viewport (a rectangle can't start
                // above the window).
                uint32_t       y = i < _top ? v.bottom() : v.y + _tops[i] - offset;
                const uint16_t h = _heights[i];
                if (y + h > UINT16_MAX) y = UINT16_MAX - h;
                list[i]->setGeometry(Rect{x, uint16_t(y), width, h});
        }
        return;
}

void ScrollView::scrollToRow(size_t row) {
        if (row > _last) row = _last;
        if (row == _top || _count == 0) return;
        _top = row;
        place();
        scrollTo(scrollX(), stop(row));
        return;
}

void ScrollView::reveal(const Widget &widget) {
        const Widget *row = &widget;
        while (row != nullptr && row->parent() != this) row = row->parent();
        if (row == nullptr || isBar(row)) return;
        Widget      *list[MaxRows];
        const size_t n = rows(list);
        for (size_t i = 0; i < n && i < _count; ++i) {
                if (list[i] != row) continue;
                scrollToRow(RowScroll::reveal(Span<const uint32_t>(_stops, _count), _tops[i], _tops[i] + _heights[i], i,
                                              _top, viewport().height));
                return;
        }
        return;
}

GuiSize ScrollView::sizeHint() const {
        Widget        *list[MaxRows];
        const size_t   n = rows(list);
        uint32_t       width = 0, height = 0;
        const uint32_t side = uint32_t(_margins[0]) + _margins[2];
        // Once it has a width, the rows' heights at it (wrapped help is shorter wide than at its hint).
        const uint16_t at = geometry().width > side + ScrollBar::Breadth
                                    ? static_cast<uint16_t>(geometry().width - side - ScrollBar::Breadth)
                                    : 0;
        for (size_t i = 0; i < n; ++i) {
                const GuiSize h = list[i]->sizeHint();
                if (h.width > width) width = h.width;
                height += (at > 0 ? list[i]->heightForWidth(at) : h.height) + (i + 1 < n ? _spacing : 0);
        }
        width += uint32_t(_margins[0]) + _margins[2] + ScrollBar::Breadth;
        height += uint32_t(_margins[1]) + _margins[3];
        return GuiSize{uint16_t(width > UINT16_MAX ? UINT16_MAX : width),
                       uint16_t(height > UINT16_MAX ? UINT16_MAX : height)};
}

bool ScrollView::childClip(const Widget &child, Rect &clip) const {
        if (isBar(&child)) return false;
        clip = viewport();
        return true;
}

bool ScrollView::wheelTurned(int notches) {
        if (_last == 0) return false; // all shows: the wheel is for whatever holds it
        const int64_t row = int64_t(_top) - notches;
        scrollToRow(row < 0 ? 0 : size_t(row));
        return true;
}

void ScrollView::childFocused(Widget &widget) {
        reveal(widget);
        return;
}

void ScrollView::resized() {
        ScrollArea::resized();
        relayout();
        return;
}

void ScrollView::scrolled() {
        // The bar moved it: to the nearest row, at least one the way it went.
        if (_count == 0 || scrollY() == stop(_top)) return;
        const size_t row = RowScroll::snap(Span<const uint32_t>(_stops, _count), scrollY(), _top, _last);
        _top = row;
        place();
        scrollTo(scrollX(), stop(row));
        return;
}

void ScrollView::paintContent(Painter &painter, const Rect &viewport) {
        fillOutsideChildren(painter, viewport, painter.palette().color(WindowPalette::Role::Face));
        return;
}

BMDS_NAMESPACE_END
