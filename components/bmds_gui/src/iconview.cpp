/**
 * @file      iconview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Items as icons with their names, flowing or on the desktop's grid (devplan/filebrowser.md §4g).
 */

#include <algorithm>
#include <cstring>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/guiwindow.h>
#include <bmds/iconview.h>
#include <bmds/lineeditmodel.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t W = IconLayout::CellWidth;
        constexpr uint16_t H = IconLayout::CellHeight;
        constexpr uint16_t TextTop = IconView::Pad + IconView::IconSide + 4;

        void copyLine(char (&out)[IconView::LineBytes], const char *text, size_t n) {
                if (n >= sizeof(out)) n = sizeof(out) - 1;
                std::memcpy(out, text, n);
                out[n] = '\0';
                return;
        }

        // A rectangle, cut to @p clip; empty if nothing is left.
        Rect cut(int x, int y, int w, int h, const Rect &clip) {
                const int l = x > clip.x ? x : clip.x, t = y > clip.y ? y : clip.y;
                const int r = x + w < int(clip.right()) ? x + w : int(clip.right());
                const int b = y + h < int(clip.bottom()) ? y + h : int(clip.bottom());
                if (r <= l || b <= t) return Rect();
                return Rect{uint16_t(l), uint16_t(t), uint16_t(r - l), uint16_t(b - t)};
        }

} // namespace

// What the items are drawn on: fills cut to the view, images and text at signed places.
class IconView::Canvas {
        public:
                virtual ~Canvas() = default;
                virtual void fill(int x, int y, int w, int h, uint32_t rgb) = 0;
                virtual void image(int x, int y, const Icon &icon, uint32_t bg) = 0;
                virtual void text(int x, int y, StringView text, uint32_t fg, uint32_t bg, uint16_t maxWidth) = 0;
                const WindowPalette *palette = nullptr;
};

namespace {

        // The window manager's painter (already clipped to the viewport).
        class PainterCanvas final : public IconView::Canvas {
                public:
                        PainterCanvas(Painter &painter, const Rect &clip) : _painter(painter), _clip(clip) {
                                palette = &painter.palette();
                        }

                        void fill(int x, int y, int w, int h, uint32_t rgb) override {
                                const Rect r = cut(x, y, w, h, _clip);
                                if (!r.isEmpty()) (void)_painter.fill(r, rgb);
                                return;
                        }

                        void image(int x, int y, const Icon &icon, uint32_t bg) override {
                                (void)_painter.image(x, y, icon, bg);
                                return;
                        }

                        void text(int x, int y, StringView text, uint32_t fg, uint32_t bg, uint16_t maxWidth) override {
                                (void)_painter.text(x, y, text, Fonts::unscii16, fg, bg, maxWidth);
                                return;
                        }

                private:
                        Painter &_painter;
                        Rect     _clip;
        };

        // A reference picture (B, G, R), drawn by the scalar kernels.
        class PictureCanvas final : public IconView::Canvas {
                public:
                        PictureCanvas(uint8_t *pixels, size_t stride, const Rect &clip)
                            : _pixels(pixels), _stride(stride), _clip(clip) {}

                        void fill(int x, int y, int w, int h, uint32_t rgb) override {
                                const Rect r = cut(x, y, w, h, _clip);
                                for (uint32_t py = r.y; py < r.bottom(); ++py)
                                        for (uint32_t px = r.x; px < r.right(); ++px) put(px, py, rgb);
                                return;
                        }

                        void image(int x, int y, const Icon &icon, uint32_t bg) override {
                                const uint16_t width = static_cast<uint16_t>(icon.width & ~3u); // as the painter
                                for (int r = 0; r < icon.height; ++r)
                                        for (int c = 0; c < width; ++c)
                                                if (_clip.contains(Rect{uint16_t(x + c), uint16_t(y + r), 1, 1}) &&
                                                    x + c >= 0 && y + r >= 0)
                                                        put(uint32_t(x + c), uint32_t(y + r),
                                                            blendOver(icon.pixels[size_t(r) * icon.width + c], bg));
                                return;
                        }

                        void text(int x, int y, StringView text, uint32_t fg, uint32_t bg, uint16_t maxWidth) override {
                                // Whole rows only (the desktop's never are cut).
                                if (x < _clip.x || y < _clip.y || y + GuiStyle::TextHeight > int(_clip.bottom()))
                                        return;
                                const uint16_t room = static_cast<uint16_t>(
                                        int(_clip.right()) - x < maxWidth ? int(_clip.right()) - x : maxWidth);
                                size_t used = 0;
                                (void)_renderer.drawText(text, Fonts::unscii16, fg, bg,
                                                         _pixels + size_t(y) * _stride + size_t(x) * 3, _stride, room,
                                                         used);
                                return;
                        }

                private:
                        void put(uint32_t x, uint32_t y, uint32_t rgb) {
                                uint8_t *p = _pixels + size_t(y) * _stride + size_t(x) * 3;
                                p[0] = static_cast<uint8_t>(rgb);
                                p[1] = static_cast<uint8_t>(rgb >> 8);
                                p[2] = static_cast<uint8_t>(rgb >> 16);
                                return;
                        }

                        uint8_t      *_pixels;
                        size_t        _stride;
                        Rect          _clip;
                        GlyphRenderer _renderer{GlyphRenderer::Kernel::Scalar};
        };

} // namespace

IconView::IconView(Widget *parent) : ItemView(parent) {
        setLineSteps(W / 2, H / 2);
}

void IconView::setArrangement(Arrangement arrangement) {
        if (arrangement == _arrangement) return;
        _arrangement = arrangement;
        relayout();
        update();
        return;
}

void IconView::setColors(WindowPalette::Role background, WindowPalette::Role text) {
        _background = background;
        _text = text;
        update();
        return;
}

// @p row's name in up to two lines, broken as `TextWrap` breaks them (as the editor that renames
// it does); past two, the second cut and ended with an ellipsis.
void IconView::lines(size_t row, Lines &out) const {
        constexpr char   Ellipsis[] = "\xe2\x80\xa6"; // U+2026
        const Font      &font = Fonts::unscii16;
        const StringView all = model() != nullptr ? model()->text(row) : StringView();
        StringView       views[3];
        const size_t     n =
                TextWrap::wrap(all, static_cast<uint16_t>(LineWidth / font.width()), Span<StringView>(views, 3));
        out.count = static_cast<uint8_t>(n < 2 ? n : 2);
        for (uint8_t l = 0; l < out.count; ++l) copyLine(out.text[l], views[l].data(), views[l].size());
        if (n >= 3) {
                // What is left from the second line on, cut to leave room for the ellipsis.
                const StringView rest = all.mid(static_cast<size_t>(views[1].data() - all.data())).trimmed();
                const size_t     kept = font.fitting(rest, LineWidth - font.textWidth(StringView(Ellipsis)));
                copyLine(out.text[1], rest.data(), kept);
                const size_t at = std::strlen(out.text[1]);
                std::memcpy(out.text[1] + at, Ellipsis, sizeof(Ellipsis));
        }
        for (uint8_t l = 0; l < out.count; ++l) {
                const uint32_t w = font.textWidth(StringView(out.text[l]));
                out.x[l] = static_cast<uint16_t>(((W - (w < W ? w : W)) / 2) & ~3u);
        }
        return;
}

ContentRect IconView::rowRect(size_t row) const {
        if (_arrangement == Arrangement::Flow)
                return ContentRect{IconLayout::flowX(row, _columns), IconLayout::flowY(row, _columns), W, H};
        const IconGrid::Cell c = cellOf(row);
        return ContentRect{Pad + uint32_t(c.column) * W, Pad + uint32_t(c.row) * H, W, H};
}

ContentRect IconView::hitRect(size_t row, uint8_t lineCount) const {
        const ContentRect c = rowRect(row);
        return ContentRect{c.x + Pad, c.y + Pad, W - 2u * Pad, uint32_t(TextTop - Pad) + lineCount * LineHeight};
}

int IconView::rowAt(uint32_t x, uint32_t y) const {
        int row = -1;
        if (_arrangement == Arrangement::Flow) {
                row = IconLayout::flowRowAt(x, y, _columns, count());
        } else {
                for (size_t r = 0; r < _cells.size() && row < 0; ++r)
                        if (rowRect(r).contains(x, y)) row = int(r);
        }
        if (row < 0) return -1;
        Lines l;
        lines(size_t(row), l);
        return hitRect(size_t(row), l.count).contains(x, y) ? row : -1;
}

bool IconView::onName(size_t row, uint32_t x, uint32_t y) const {
        (void)x;
        Lines l;
        lines(row, l);
        const uint32_t top = rowRect(row).y + TextTop;
        return y >= top && y < top + l.count * LineHeight;
}

size_t IconView::neighbour(size_t row, int dx, int dy) const {
        if (_arrangement == Arrangement::Flow) return IconLayout::flowNeighbour(row, dx, dy, _columns, count());
        const int to = IconLayout::gridNeighbour(_cells.span(), int(row), dx, dy);
        return to >= 0 ? size_t(to) : row;
}

size_t IconView::endRow(bool last) const {
        if (_arrangement == Arrangement::Flow || _order.isEmpty()) return ItemView::endRow(last);
        return last ? _order.back() : _order.front();
}

// On the grid, a Shift range is the rectangle of cells between the two (the rows' order there is
// the names', not the places').
bool IconView::inRange(size_t row, size_t anchor, size_t to) const {
        if (_arrangement == Arrangement::Flow) return ItemView::inRange(row, anchor, to);
        return IconLayout::inGridRange(cellOf(row), cellOf(anchor), cellOf(to));
}

size_t IconView::typingOrder(size_t k) const {
        return _arrangement == Arrangement::Grid && k < _order.size() ? _order[k] : k;
}

void IconView::relayout() {
        const Rect   g = geometry();
        const size_t n = count();
        if (_arrangement == Arrangement::Flow) {
                // As many columns as fit; fewer if the rows then need the bar.
                _columns = IconLayout::columnsFor(g.width);
                if (IconLayout::flowHeight(n, _columns) > g.height && g.width > ScrollBar::Breadth)
                        _columns = IconLayout::columnsFor(uint16_t(g.width - ScrollBar::Breadth));
                setContentSize(uint32_t(_columns) * W, IconLayout::flowHeight(n, _columns));
                return;
        }
        // The grid: the cells the area holds.
        _gridRows = g.height > 2 * Pad ? static_cast<uint16_t>((g.height - 2 * Pad) / H) : 1;
        _columns = g.width > 2 * Pad ? static_cast<uint16_t>((g.width - 2 * Pad) / W) : 1;
        if (_gridRows == 0) _gridRows = 1;
        if (_columns == 0) _columns = 1;
        if (_cells.size() != n) (void)setCells(Span<const IconGrid::Cell>());
        // Down then across (Home, End and typing go that way), and the room the cells take.
        _order.clear();
        uint32_t right = 0, bottom = 0;
        if (_order.reserve(n).isOk()) {
                for (size_t r = 0; r < n; ++r) (void)_order.append(uint32_t(r));
                std::sort(_order.begin(), _order.end(), [this](uint32_t a, uint32_t b) {
                        const IconGrid::Cell x = _cells[a], y = _cells[b];
                        return x.column != y.column ? x.column < y.column : x.row < y.row;
                });
        }
        for (size_t r = 0; r < _cells.size(); ++r) {
                const ContentRect c = rowRect(r);
                if (c.x + c.width > right) right = c.x + c.width;
                if (c.y + c.height > bottom) bottom = c.y + c.height;
        }
        setContentSize(right > g.width ? right : g.width, bottom > g.height ? bottom : g.height);
        return;
}

Error IconView::setCells(Span<const IconGrid::Cell> wanted) {
        const size_t n = count();
        _cells.clear();
        List<IconGrid::Cell> want(MemSpace::Psram);
        Error                err = want.reserve(n);
        if (err.isOk()) err = _cells.reserve(n);
        if (err.isError()) return err;
        for (size_t r = 0; r < n; ++r) {
                (void)want.append(wanted.size() == n ? wanted[r] : IconGrid::Cell());
                (void)_cells.append(IconGrid::Cell());
        }
        err = IconGrid::place(want.span(), _columns, _gridRows, _cells.span());
        if (err.isError()) {
                _cells.clear();
                return err;
        }
        if (_arrangement == Arrangement::Grid) relayout();
        update();
        return Error::Ok;
}

bool IconView::placeRow(size_t row, uint16_t screenX, uint16_t screenY) {
        GuiWindow *w = window();
        if (row >= _cells.size() || w == nullptr) return false;
        const Rect s = w->screenRect();
        const int  x = int(contentX(screenX - s.x)) - Pad, y = int(contentY(screenY - s.y)) - Pad;
        if (x < 0 || y < 0 || x >= _columns * W || y >= _gridRows * H) return false;
        const IconGrid::Cell at = IconGrid::cellAt(x, y, W, H, _columns, _gridRows);
        for (size_t i = 0; i < _cells.size(); ++i)
                if (i != row && _cells[i] == at) return false;
        updateRow(row);
        _cells[row] = at;
        relayout();
        updateRow(row);
        return true;
}

Rect IconView::nameRect(size_t row) const {
        GuiWindow *w = window();
        if (row >= count() || w == nullptr) return Rect();
        const Rect        s = w->screenRect();
        const ContentRect c = rowRect(row);
        const Rect        v = viewport();
        const int         x = int(v.x) + int(c.x) - int(scrollX()), y = int(v.y) + int(c.y) - int(scrollY());
        return Rect{uint16_t(s.x + x), uint16_t(s.y + y + TextTop), W, LineHeight};
}

bool IconView::isDragged(size_t row) const {
        return _draggingCells && selection().isSelected(row);
}

bool IconView::dragged(int dx, int dy, bool done) {
        if (_arrangement != Arrangement::Grid) return false;
        GuiWindow *w = window();
        if (!done) {
                // Each dragged row where it was drawn and where it goes (not the whole view: it flickers).
                for (int r = selection().nextSelected(0); r >= 0; r = selection().nextSelected(size_t(r) + 1)) {
                        if (_draggingCells) invalidateRow(size_t(r), _dragX, _dragY);
                        invalidateRow(size_t(r), int(unsigned(dx) & ~3u), dy);
                }
                _draggingCells = true;
                _dragX = dx;
                _dragY = dy;
                _dragX = int(unsigned(_dragX) & ~3u); // text starts on a 4-pixel column
                return true;
        }
        // Where the dragged rows were drawn, gone (they land below, or go back).
        for (int r = selection().nextSelected(0); r >= 0; r = selection().nextSelected(size_t(r) + 1)) {
                invalidateRow(size_t(r), _dragX, _dragY);
                invalidateRow(size_t(r), 0, 0);
        }
        _draggingCells = false;
        // Each selected row to the cell under its middle, moved as far; a row there unselected goes
        // to the cell its mover left. Refused (all back) if two would share a cell.
        const size_t         n = _cells.size();
        List<IconGrid::Cell> to(MemSpace::Psram);
        if (to.reserve(n).isError()) return true;
        bool any = false;
        for (size_t r = 0; r < n; ++r) {
                IconGrid::Cell c = _cells[r];
                if (selection().isSelected(r)) {
                        const ContentRect cell = rowRect(r);
                        c = IconGrid::cellAt(int(cell.x) + dx + W / 2 - Pad, int(cell.y) + dy + H / 2 - Pad, W, H,
                                             _columns, _gridRows);
                        any = any || c != _cells[r];
                }
                (void)to.append(c);
        }
        if (any) {
                // Unselected rows in the way swap into the cells the movers left.
                size_t freed = 0;
                for (size_t r = 0; r < n; ++r) {
                        if (selection().isSelected(r)) continue;
                        for (size_t m = 0; m < n; ++m) {
                                if (!selection().isSelected(m) || to[m] != _cells[r]) continue;
                                // A mover's old cell that no mover takes.
                                for (; freed < n; ++freed) {
                                        if (!selection().isSelected(freed)) continue;
                                        bool taken = false;
                                        for (size_t k = 0; k < n && !taken; ++k) taken = to[k] == _cells[freed];
                                        if (!taken) break;
                                }
                                if (freed < n) to[r] = _cells[freed++];
                                break;
                        }
                }
                // Every cell held once, or nothing moves.
                bool clash = false;
                for (size_t a = 0; a < n && !clash; ++a)
                        for (size_t b = a + 1; b < n && !clash; ++b) clash = to[a] == to[b];
                if (!clash) {
                        // Every row that moves, at its old cell and its new one.
                        for (size_t r = 0; r < n; ++r) {
                                if (to[r] == _cells[r]) continue;
                                invalidateRow(r, 0, 0);
                                _cells[r] = to[r];
                                invalidateRow(r, 0, 0);
                        }
                        relayout();
                } else {
                        any = false;
                }
        }
        (void)w;
        if (any && _moved != nullptr) _moved(_movedArg, *this);
        return true;
}

void IconView::dragCancelled() {
        for (int r = selection().nextSelected(0); r >= 0; r = selection().nextSelected(size_t(r) + 1)) {
                invalidateRow(size_t(r), _dragX, _dragY);
                invalidateRow(size_t(r), 0, 0);
        }
        _draggingCells = false;
        return;
}

// Row @p row's cell, moved by (@p dx, @p dy), redrawn (what of it is in the viewport).
void IconView::invalidateRow(size_t row, int dx, int dy) {
        GuiWindow *w = window();
        if (w == nullptr) return;
        const Rect        v = viewport();
        const ContentRect c = rowRect(row);
        const int         x = int(v.x) + int(c.x) - int(scrollX()) + dx;
        const int         y = int(v.y) + int(c.y) - int(scrollY()) + dy;
        const int         l = x > v.x ? x : v.x, t = y > v.y ? y : v.y;
        const int         r = x + W < int(v.right()) ? x + W : int(v.right());
        const int         b = y + H < int(v.bottom()) ? y + H : int(v.bottom());
        if (r > l && b > t) w->invalidate(Rect{uint16_t(l), uint16_t(t), uint16_t(r - l), uint16_t(b - t)});
        return;
}

void IconView::drawRow(Canvas &canvas, size_t row, int offsetX, int offsetY) const {
        const WindowPalette &p = *canvas.palette;
        const Rect           v = viewport();
        const ContentRect    c = rowRect(row);
        const int            x = int(v.x) + int(c.x) - int(scrollX()) + offsetX;
        const int            y = int(v.y) + int(c.y) - int(scrollY()) + offsetY;
        const uint32_t       background = p.color(_background);
        const Icon          *icon = model() != nullptr ? model()->icon(row, IconSide) : nullptr;
        if (icon != nullptr && icon->isValid() && icon->width <= IconSide && icon->height <= IconSide) {
                const int ix = (x + (W - icon->width) / 2) & ~3;
                canvas.image(ix, y + Pad + (IconSide - icon->height) / 2, *icon, background);
        }
        if (int(row) == editing()) return;
        Lines l;
        lines(row, l);
        const bool     selected = selection().isSelected(row);
        const bool     focus = showsFocus();
        const bool     filled = selected && focus;
        const bool     outlined = (selected && !focus) || (!selected && focus && int(row) == current());
        const uint32_t fg = p.color(filled ? Role::HighlightText : _text);
        const uint32_t bg = filled ? p.color(Role::Highlight) : background;
        for (uint8_t i = 0; i < l.count; ++i) {
                const int tw = int(Fonts::unscii16.textWidth(StringView(l.text[i])));
                const int lx = x + l.x[i] - Pad, ly = y + TextTop + i * LineHeight, lw = tw + 2 * Pad;
                if (filled) canvas.fill(lx, ly, lw, LineHeight, bg);
                canvas.text(x + l.x[i], ly, StringView(l.text[i]), fg, bg, LineWidth);
                if (outlined) {
                        // After the text (its rows would cover the top and bottom): an inactive selection
                        // in the highlight, the focus alone in the names' colour.
                        const uint32_t edge = p.color(selected ? Role::Highlight : _text);
                        canvas.fill(lx, ly, lw, 1, edge);
                        canvas.fill(lx, ly + LineHeight - 1, lw, 1, edge);
                        canvas.fill(lx, ly + 1, 1, LineHeight - 2, edge);
                        canvas.fill(lx + lw - 1, ly + 1, 1, LineHeight - 2, edge);
                }
        }
        return;
}

void IconView::drawAll(Canvas &canvas, const Rect &viewport) const {
        canvas.fill(viewport.x, viewport.y, viewport.width, viewport.height, canvas.palette->color(_background));
        const ContentRect seen{scrollX(), scrollY(), viewport.width, viewport.height};
        size_t            first = 0, end = count();
        if (_arrangement == Arrangement::Flow) {
                // Only the lines that show.
                first = size_t(seen.y / H) * _columns;
                end = size_t((seen.y + seen.height) / H + 1) * _columns;
                if (end > count()) end = count();
        }
        // Dragged rows last, over the others.
        for (int pass = 0; pass < 2; ++pass) {
                for (size_t r = first; r < end; ++r) {
                        const bool moving = isDragged(r);
                        if (moving != (pass == 1)) continue;
                        if (!moving && !rowRect(r).intersects(seen)) continue;
                        drawRow(canvas, r, moving ? _dragX : 0, moving ? _dragY : 0);
                }
        }
        return;
}

void IconView::paintRows(Painter &painter, const Rect &viewport) {
        PainterCanvas canvas(painter, viewport);
        drawAll(canvas, viewport);
        return;
}

void IconView::drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                             const WindowPalette &palette) const {
        const Rect    v = viewport().intersected(Rect{0, 0, width, height});
        PictureCanvas canvas(pixels, stride, v);
        canvas.palette = &palette;
        drawAll(canvas, v);
        return;
}

BMDS_NAMESPACE_END
