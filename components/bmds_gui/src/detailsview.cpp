/**
 * @file      detailsview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Items in rows under a header of columns (devplan/filebrowser.md §4g).
 */

#include <bmds/bevel.h>
#include <bmds/detailsview.h>
#include <bmds/font.h>
#include <bmds/guiwindow.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t IconX = 4;  // the name column's icon, from the column's left
        constexpr uint16_t TextX = 24; // its text
        constexpr uint16_t CellPad = 4;
        constexpr uint32_t WarningText = 0xa05a00;  // a row's emphasis: amber, readable on the face
        constexpr uint32_t CriticalText = 0xc00000; // red

        Rect cut(int x, int y, int w, int h, const Rect &clip) {
                const int l = x > clip.x ? x : clip.x, t = y > clip.y ? y : clip.y;
                const int r = x + w < int(clip.right()) ? x + w : int(clip.right());
                const int b = y + h < int(clip.bottom()) ? y + h : int(clip.bottom());
                if (r <= l || b <= t) return Rect();
                return Rect{uint16_t(l), uint16_t(t), uint16_t(r - l), uint16_t(b - t)};
        }

} // namespace

DetailsView::DetailsView(Widget *parent) : ItemView(parent) {
        setTopMargin(HeaderHeight);
        setLineSteps(16, RowHeight);
}

void DetailsView::setHeaderVisible(bool visible) {
        setTopMargin(visible ? HeaderHeight : 0); // a header() of no height then: not drawn, never pressed
        return;
}

uint32_t DetailsView::total() const {
        return HeaderLayout::total(Span<const uint16_t>(_widths, _columns));
}

void DetailsView::setSortIndicator(int column, bool descending) {
        _sortColumn = column;
        _descending = descending;
        GuiWindow *w = window();
        if (w != nullptr) w->invalidate(header());
        return;
}

void DetailsView::setColumnWidth(size_t column, uint16_t width) {
        if (column >= _columns) return;
        _widths[column] = HeaderLayout::resized(width, 0);
        setContentSize(total(), uint32_t(count()) * RowHeight);
        update();
        return;
}

// The header is outside the viewport the scroll redraws: redrawn when the columns move across.
void DetailsView::scrolled() {
        if (scrollX() == _headerX) return;
        _headerX = scrollX();
        GuiWindow *w = window();
        if (w != nullptr) w->invalidate(header());
        return;
}

void DetailsView::relayout() {
        // The columns from the model when it has other ones.
        const size_t columns =
                model() != nullptr ? (model()->columns() < MaxColumns ? model()->columns() : MaxColumns) : 1;
        if (columns != _columns) {
                _columns = columns;
                for (size_t c = 0; c < _columns; ++c)
                        _widths[c] = HeaderLayout::resized(model() != nullptr ? model()->columnWidth(c) : 200, 0);
        }
        setContentSize(total(), uint32_t(count()) * RowHeight);
        return;
}

Rect DetailsView::header() const {
        const Rect v = viewport();
        return Rect{v.x, geometry().y, v.width, uint16_t(v.y - geometry().y)};
}

bool DetailsView::inHeader(int x, int y) const {
        const Rect h = header();
        return x >= h.x && y >= h.y && uint32_t(x) < h.right() && uint32_t(y) < h.bottom();
}

ContentRect DetailsView::rowRect(size_t row) const {
        return ContentRect{0, uint32_t(row) * RowHeight, total(), RowHeight};
}

int DetailsView::rowAt(uint32_t x, uint32_t y) const {
        const size_t row = y / RowHeight;
        return row < count() && x < total() ? int(row) : -1;
}

// A drag shows the icon and the name, not the other columns (wider than its image).
DragImage::Box DetailsView::dragBox(size_t row) const {
        DragImage::Box b = rowBox(row);
        if (model() == nullptr || _columns == 0) return b;
        const int named = TextX + int(Fonts::unscii16.textWidth(model()->text(row))) + 4;
        const int first = _widths[0];
        b.width = named < first ? named : first;
        return b;
}

bool DetailsView::onName(size_t row, uint32_t x, uint32_t y) const {
        (void)y;
        if (model() == nullptr || _columns == 0) return false;
        const uint32_t w = Fonts::unscii16.textWidth(model()->text(row));
        return x >= TextX && x < TextX + w && x < _widths[0];
}

size_t DetailsView::neighbour(size_t row, int dx, int dy) const {
        (void)dx;
        if (dy < 0) return row > 0 ? row - 1 : row;
        if (dy > 0) return row + 1 < count() ? row + 1 : row;
        return row;
}

Rect DetailsView::nameRect(size_t row) const {
        GuiWindow *w = window();
        if (row >= count() || w == nullptr) return Rect();
        const Rect s = w->screenRect();
        const Rect v = viewport();
        const int  x = int(v.x) + TextX - int(scrollX()) - CellPad;
        const int  y = int(v.y) + int(row) * RowHeight - int(scrollY()) + 1;
        const int  width = _widths[0] > TextX ? _widths[0] - TextX + CellPad : 64;
        return Rect{uint16_t(s.x + (x > 0 ? x : 0)), uint16_t(s.y + y), uint16_t(width), uint16_t(RowHeight - 2)};
}

void DetailsView::paint(Painter &painter) {
        ItemView::paint(painter);
        drawClipped(painter, header(), [this](Painter &clipped) { paintHeader(clipped); });
        return;
}

void DetailsView::paintHeader(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect           h = header();
        const uint32_t       face = p.color(Role::Face);
        int                  x = int(h.x) - int(scrollX());
        for (size_t c = 0; c <= _columns; ++c) {
                // Each column's button, then one filling the rest.
                const int width = c < _columns ? _widths[c] : int(h.right()) - x;
                if (width <= 0) break;
                const Rect r = cut(x, h.y, width, h.height, h);
                if (!r.isEmpty()) {
                        const bool         in = int(c) == _pressColumn && _pressOver;
                        const Bevel::Style style = in ? Bevel::Style::Sunken : Bevel::Style::Raised;
                        // The bevel of the whole button, even if part of it is scrolled off.
                        const Rect whole{uint16_t(x > 0 ? x : 0), h.y, uint16_t(x > 0 ? width : width + x), h.height};
                        drawBevel(painter, whole, style);
                        (void)painter.fill(Bevel::inside(whole, style), face);
                        if (c < _columns && model() != nullptr) {
                                const int ty = h.y + (h.height - GuiStyle::TextHeight) / 2 + (in ? 1 : 0);
                                // Short of the button's right bevel (and the sort arrow), so a cut title
                                // never draws over the edge.
                                const int room = (width - 3 * CellPad - (int(c) == _sortColumn ? 16 : 0)) &
                                                 ~int(GuiStyle::Column - 1);
                                if (room > 0)
                                        (void)painter.text(x + 2 * CellPad, ty, model()->columnTitle(c),
                                                           Fonts::unscii16, p.color(Role::FaceText), face,
                                                           uint16_t(room));
                                if (int(c) == _sortColumn)
                                        GuiStyle::drawArrow(painter,
                                                            Rect{uint16_t(x + width - 16 > 0 ? x + width - 16 : 0), h.y,
                                                                 12, h.height},
                                                            _descending ? GuiStyle::Arrow::Down : GuiStyle::Arrow::Up,
                                                            p.color(Role::FaceText));
                        }
                }
                x += width;
        }
        return;
}

void DetailsView::paintRows(Painter &painter, const Rect &viewport) {
        const WindowPalette &p = painter.palette();
        const uint32_t       background = p.color(Role::FaceHighlight);
        (void)painter.fill(viewport, background);
        if (model() == nullptr || _columns == 0) return;
        const size_t first = scrollY() / RowHeight;
        const size_t last = (scrollY() + viewport.height) / RowHeight;
        const bool   focus = showsFocus();
        char         buffer[64];
        for (size_t row = first; row <= last && row < count(); ++row) {
                const int  y = int(viewport.y) + int(row) * RowHeight - int(scrollY());
                int        x = int(viewport.x) - int(scrollX());
                const int  ty = y + (RowHeight - GuiStyle::TextHeight) / 2;
                const bool selected = selection().isSelected(row);
                const ItemModel::Emphasis emphasis = model()->emphasis(row);
                const uint32_t            text = emphasis == ItemModel::Emphasis::Critical  ? CriticalText
                                                 : emphasis == ItemModel::Emphasis::Warning ? WarningText
                                                                                            : p.color(Role::FaceText);
                for (size_t c = 0; c < _columns; ++c) {
                        const int width = _widths[c];
                        if (x + width > viewport.x && x < int(viewport.right())) {
                                if (c == 0) {
                                        const Icon *icon = model()->icon(row, 16);
                                        if (icon != nullptr && icon->isValid() && icon->height <= 16)
                                                (void)painter.image(x + IconX, y + (RowHeight - icon->height) / 2,
                                                                    *icon, background);
                                        if (int(row) != editing()) {
                                                const StringView name = model()->text(row);
                                                const int        tw = int(Fonts::unscii16.textWidth(name));
                                                const int        room =
                                                        width - TextX - CellPad > 0 ? width - TextX - CellPad : 0;
                                                const int  shown = tw < room ? tw : room;
                                                const int      bx = x + TextX - 2, bw = shown + 4;
                                                const uint32_t bg = drawLabel(painter, viewport, bx, y, bw, selected,
                                                                              focus, int(row) == current(), background);
                                                const uint32_t fg =
                                                        selected && focus ? p.color(Role::HighlightText) : text;
                                                if (room > 0)
                                                        (void)painter.text(x + TextX, ty, name, Fonts::unscii16, fg, bg,
                                                                           uint16_t(room));
                                                if (!selected && focus && int(row) == current())
                                                        focusRect(painter, viewport, bx, y, bw);
                                        }
                                } else {
                                        const StringView cell = model()->columnText(row, c, Span<char>(buffer));
                                        const int        tw = int(Fonts::unscii16.textWidth(cell));
                                        const int        room = width - 2 * CellPad;
                                        if (room > 0 && !cell.isEmpty()) {
                                                int tx = x + CellPad;
                                                if (model()->columnAlign(c) == ItemModel::Align::Right && tw < room)
                                                        tx = (x + width - CellPad - tw) & ~3;
                                                (void)painter.text(tx, ty, cell, Fonts::unscii16, text, background,
                                                                   uint16_t(room));
                                        }
                                }
                        }
                        x += width;
                }
        }
        return;
}

// ---- The header's mouse

void DetailsView::mousePressed(const WindowManager::MouseEvent &event) {
        if (!inHeader(event.x, event.y)) {
                ItemView::mousePressed(event);
                return;
        }
        if (event.button != PointerEvent::Left) return;
        const int x = int(contentX(event.x));
        _resizing = HeaderLayout::edgeAt(Span<const uint16_t>(_widths, _columns), x);
        if (_resizing >= 0) {
                _resizeX = event.x;
                _resizeStart = _widths[_resizing];
                return;
        }
        _pressColumn = HeaderLayout::columnAt(Span<const uint16_t>(_widths, _columns), x);
        _pressOver = _pressColumn >= 0;
        GuiWindow *w = window();
        if (w != nullptr) w->invalidate(header());
        return;
}

void DetailsView::mouseMoved(const WindowManager::MouseEvent &event) {
        if (_resizing >= 0) {
                const uint16_t width = HeaderLayout::resized(_resizeStart, event.x - _resizeX);
                if (width != _widths[_resizing]) setColumnWidth(size_t(_resizing), width);
                return;
        }
        if (_pressColumn >= 0) {
                const bool over =
                        inHeader(event.x, event.y) && HeaderLayout::columnAt(Span<const uint16_t>(_widths, _columns),
                                                                             int(contentX(event.x))) == _pressColumn;
                if (over != _pressOver) {
                        _pressOver = over;
                        GuiWindow *w = window();
                        if (w != nullptr) w->invalidate(header());
                }
                return;
        }
        ItemView::mouseMoved(event);
        return;
}

void DetailsView::mouseReleased(const WindowManager::MouseEvent &event) {
        if (_resizing >= 0) {
                if (event.button == PointerEvent::Left) _resizing = -1;
                return;
        }
        if (_pressColumn >= 0) {
                if (event.button != PointerEvent::Left) return;
                const int  column = _pressColumn;
                const bool over = _pressOver;
                _pressColumn = -1;
                _pressOver = false;
                GuiWindow *w = window();
                if (w != nullptr) w->invalidate(header());
                if (over && _sortAsked != nullptr) _sortAsked(_sortArg, *this, size_t(column));
                return;
        }
        ItemView::mouseReleased(event);
        return;
}

WindowManager::Pointer DetailsView::pointerAt(int x, int y) const {
        if (_resizing >= 0 ||
            (inHeader(x, y) && HeaderLayout::edgeAt(Span<const uint16_t>(_widths, _columns), int(contentX(x))) >= 0))
                return WindowManager::Pointer::Horizontal;
        return ItemView::pointerAt(x, y);
}

BMDS_NAMESPACE_END
