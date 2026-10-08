/**
 * @file      scrollarea.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Scroll bars and scroll areas (devplan/filebrowser.md §4e).
 */

#include <bmds/bevel.h>
#include <bmds/guiwindow.h>
#include <bmds/list.h>
#include <bmds/scrollarea.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint32_t RepeatDelayMs = 400;
        constexpr uint32_t RepeatMs = 50;
        constexpr int      WheelLines = 3;
        constexpr size_t   LocalPieces = 64; // a clipped painter's storage on the stack; past it, PSRAM

        // A small arrow (a triangle 7 pixels across, 4 deep) in @p r's middle, pointing @p way:
        // 0 up, 1 down, 2 left, 3 right.
        void drawArrow(Painter &painter, const Rect &r, int way, uint32_t color) {
                const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
                Rect      rows[4];
                for (int i = 0; i < 4; ++i) {
                        const int half = way == 0 || way == 2 ? i : 3 - i; // the row's half width
                        rows[i] = way < 2 ? Rect{uint16_t(cx - half), uint16_t(cy - 2 + i), uint16_t(2 * half + 1), 1}
                                          : Rect{uint16_t(cx - 2 + i), uint16_t(cy - half), 1, uint16_t(2 * half + 1)};
                }
                (void)painter.fill(Span<const Rect>(rows), color);
                return;
        }

} // namespace

// ---- ScrollBar

ScrollBar::ScrollBar(Widget *parent, Orientation orientation) : Widget(parent), _orientation(orientation) {}

ScrollBar::~ScrollBar() {
        stopRepeat();
}

GuiSize ScrollBar::sizeHint() const {
        return _orientation == Orientation::Vertical ? GuiSize{Breadth, 0} : GuiSize{0, Breadth};
}

uint16_t ScrollBar::length() const {
        return _orientation == Orientation::Vertical ? geometry().height : geometry().width;
}

int ScrollBar::along(int x, int y) const {
        return _orientation == Orientation::Vertical ? y - geometry().y : x - geometry().x;
}

Rect ScrollBar::partRect(uint16_t start, uint16_t size) const {
        const Rect &r = geometry();
        return _orientation == Orientation::Vertical ? Rect{r.x, uint16_t(r.y + start), r.width, size}
                                                     : Rect{uint16_t(r.x + start), r.y, size, r.height};
}

void ScrollBar::setRange(uint32_t total, uint32_t page) {
        const ScrollModel::Geometry before = parts();
        const bool                  was = _model.isScrollable();
        _model.setRange(total, page);
        const ScrollModel::Geometry after = parts();
        if (was != _model.isScrollable() || before.thumbStart != after.thumbStart ||
            before.thumbLength != after.thumbLength)
                update();
        return;
}

void ScrollBar::setValue(int64_t value) {
        const uint16_t start = parts().thumbStart;
        if (_model.setValue(value) && parts().thumbStart != start) update();
        return;
}

void ScrollBar::paint(Painter &painter) {
        const WindowPalette        &p = painter.palette();
        const ScrollModel::Geometry g = parts();
        const bool                  vertical = _orientation == Orientation::Vertical;
        const bool                  live = isEnabled() && _model.isScrollable();
        const uint32_t              face = p.color(Role::Face);
        // The track, the pressed page darker, then the thumb.
        const Rect track = partRect(g.trackStart, g.trackLength);
        (void)painter.fill(track, p.color(Role::FaceLight));
        if ((_pressed == Part::BackPage || _pressed == Part::ForwardPage) && g.thumbLength != 0 &&
            ScrollModel::partAt(g, length(), _at) == _pressed) {
                const Rect page =
                        _pressed == Part::BackPage
                                ? partRect(g.trackStart, uint16_t(g.thumbStart - g.trackStart))
                                : partRect(uint16_t(g.thumbStart + g.thumbLength),
                                           uint16_t(g.trackStart + g.trackLength - g.thumbStart - g.thumbLength));
                (void)painter.fill(page, p.color(Role::FaceDark));
        }
        if (g.thumbLength != 0) {
                const Rect thumb = partRect(g.thumbStart, g.thumbLength);
                drawBevel(painter, thumb, Bevel::Style::Raised);
                (void)painter.fill(Bevel::inside(thumb, Bevel::Style::Raised), face);
        }
        // The arrows: a raised button each, sunk while pressed with the pointer on it.
        for (int end = 0; end < 2; ++end) {
                const Part part = end == 0 ? Part::BackArrow : Part::ForwardArrow;
                const Rect r = partRect(end == 0 ? 0 : uint16_t(length() - g.arrow), g.arrow);
                if (r.isEmpty()) continue;
                const bool         in = _pressed == part && ScrollModel::partAt(g, length(), _at) == part;
                const Bevel::Style style = in ? Bevel::Style::Sunken : Bevel::Style::Raised;
                drawBevel(painter, r, style);
                (void)painter.fill(Bevel::inside(r, style), face);
                if (r.width < 10 || r.height < 10) continue; // no room for the arrow
                const Rect at = in ? Rect{uint16_t(r.x + 1), uint16_t(r.y + 1), r.width, r.height} : r;
                drawArrow(painter, at, (vertical ? 0 : 2) + end, p.color(live ? Role::FaceText : Role::DisabledText));
        }
        return;
}

void ScrollBar::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !_model.isScrollable()) return;
        const ScrollModel::Geometry g = parts();
        _at = along(event.x, event.y);
        _pressed = ScrollModel::partAt(g, length(), _at);
        if (_pressed == Part::Thumb) {
                _grab = _at - g.thumbStart;
                update();
                return;
        }
        if (_pressed == Part::None) return;
        act();
        update(); // drawn pressed
        // Held: again after a delay, then often.
        GuiWindow *w = window();
        if (w != nullptr) {
                auto [id, err] = w->application().startTimer(RepeatDelayMs, false, tick, this);
                _timer = err.isOk() ? id : -1;
                _repeating = false;
        }
        return;
}

void ScrollBar::mouseMoved(const WindowManager::MouseEvent &event) {
        if (_pressed == Part::None) return;
        const int at = along(event.x, event.y);
        if (_pressed == Part::Thumb) {
                const uint32_t v = _model.valueForThumb(parts(), at - _grab);
                if (v != _model.value()) {
                        (void)_model.setValue(v);
                        update();
                        if (_changed != nullptr) _changed(_changedArg, *this, v);
                }
                _at = at;
                return;
        }
        // Off the part pressed and back: drawn out and in.
        const ScrollModel::Geometry g = parts();
        const bool                  was = ScrollModel::partAt(g, length(), _at) == _pressed;
        _at = at;
        if (was != (ScrollModel::partAt(g, length(), _at) == _pressed)) update();
        return;
}

void ScrollBar::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || _pressed == Part::None) return;
        _pressed = Part::None;
        stopRepeat();
        update();
        return;
}

// The pressed part once, if the pointer is still on it (the track's stops when the thumb
// reaches the pointer).
void ScrollBar::act() {
        if (ScrollModel::partAt(parts(), length(), _at) != _pressed) return;
        if (_model.act(_pressed)) {
                update();
                if (_changed != nullptr) _changed(_changedArg, *this, _model.value());
        }
        return;
}

void ScrollBar::stopRepeat() {
        GuiWindow *w = _timer >= 0 ? window() : nullptr;
        if (w != nullptr) w->application().stopTimer(_timer);
        _timer = -1;
        _repeating = false;
        return;
}

void ScrollBar::tick(void *arg) {
        auto *self = static_cast<ScrollBar *>(arg);
        if (self->_pressed == Part::None || self->_pressed == Part::Thumb) return;
        self->act();
        if (!self->_repeating) {
                // The delay's one-shot is done (its number free): now the repeat.
                self->_timer = -1;
                GuiWindow *w = self->window();
                if (w == nullptr) return;
                auto [id, err] = w->application().startTimer(RepeatMs, true, tick, self);
                self->_timer = err.isOk() ? id : -1;
                self->_repeating = true;
        }
        return;
}

// ---- ScrollArea

ScrollArea::ScrollArea(Widget *parent) : Widget(parent) {
        _vertical = Widget::create<ScrollBar>(this, ScrollBar::Orientation::Vertical);
        _horizontal = Widget::create<ScrollBar>(this, ScrollBar::Orientation::Horizontal);
        for (ScrollBar *bar : {_vertical, _horizontal}) {
                if (bar == nullptr) continue;
                bar->setVisible(false);
                bar->setChanged(barChanged, this);
        }
        setLineSteps(16, 16);
}

void ScrollArea::setLineSteps(uint16_t x, uint16_t y) {
        _lineX = static_cast<uint16_t>(x < 4 ? 4 : (x + 3u) & ~3u);
        _lineY = y > 0 ? y : 1;
        if (_horizontal != nullptr) _horizontal->setLineStep(_lineX);
        if (_vertical != nullptr) _vertical->setLineStep(_lineY);
        return;
}

void ScrollArea::setContentSize(uint32_t width, uint32_t height) {
        if (width == _contentWidth && height == _contentHeight) return;
        _contentWidth = width;
        _contentHeight = height;
        layoutBars();
        return;
}

Rect ScrollArea::viewport() const {
        Rect r = geometry();
        if (_vertical != nullptr && _vertical->isVisible() && r.width >= ScrollBar::Breadth)
                r.width = static_cast<uint16_t>(r.width - ScrollBar::Breadth);
        if (_horizontal != nullptr && _horizontal->isVisible() && r.height >= ScrollBar::Breadth)
                r.height = static_cast<uint16_t>(r.height - ScrollBar::Breadth);
        return r;
}

void ScrollArea::resized() {
        layoutBars();
        return;
}

// The bars shown as the content needs (one's room may need the other), placed, and the offset
// kept in range.
void ScrollArea::layoutBars() {
        const Rect     r = geometry();
        const uint16_t b = ScrollBar::Breadth;
        bool           needV = _contentHeight > r.height;
        bool           needH = _contentWidth > uint32_t(r.width) - (needV ? b : 0);
        if (needH && !needV) needV = _contentHeight > uint32_t(r.height) - b;
        if (_vertical == nullptr) needV = false;
        if (_horizontal == nullptr) needH = false;
        if (needV && (r.width < b || r.height < (needH ? b : 0))) needV = false;
        if (needH && r.height < b) needH = false;
        if (_vertical != nullptr) _vertical->setVisible(needV);
        if (_horizontal != nullptr) _horizontal->setVisible(needH);
        const Rect v = viewport();
        if (needV) {
                _vertical->setGeometry(Rect{uint16_t(r.right() - b), r.y, b, v.height});
                _vertical->setRange(_contentHeight, v.height);
        }
        if (needH) {
                _horizontal->setGeometry(Rect{r.x, uint16_t(r.bottom() - b), v.width, b});
                _horizontal->setRange(_contentWidth, v.width);
        }
        scrollTo(_scrollX, _scrollY);
        update();
        return;
}

void ScrollArea::scrollTo(int64_t x, int64_t y) {
        const Rect     v = viewport();
        const uint32_t mostX = _contentWidth > v.width ? (_contentWidth - v.width + 3u) & ~3u : 0;
        const uint32_t mostY = _contentHeight > v.height ? _contentHeight - v.height : 0;
        x = x < 0 ? 0 : x > mostX ? mostX : x;
        y = y < 0 ? 0 : y > mostY ? mostY : y;
        const auto nx = static_cast<uint32_t>(x) & ~3u;
        const auto ny = static_cast<uint32_t>(y);
        if (_vertical != nullptr) _vertical->setValue(ny);
        if (_horizontal != nullptr) _horizontal->setValue(nx);
        if (nx == _scrollX && ny == _scrollY) return;
        _scrollX = nx;
        _scrollY = ny;
        GuiWindow *w = window();
        if (w != nullptr) w->invalidate(v);
        scrolled();
        return;
}

void ScrollArea::ensureVisible(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        const Rect v = viewport();
        int64_t    nx = _scrollX, ny = _scrollY;
        if (int64_t(x) + width > nx + v.width) nx = int64_t(x) + width - v.width;
        if (x < nx) nx = x;
        if (int64_t(y) + height > ny + v.height) ny = int64_t(y) + height - v.height;
        if (y < ny) ny = y;
        if (nx > _scrollX) nx = (nx + 3) & ~int64_t(3); // to a column, still showing it
        scrollTo(nx, ny);
        return;
}

void ScrollArea::follow() {
        scrollTo(_horizontal != nullptr && _horizontal->isVisible() ? _horizontal->value() : _scrollX,
                 _vertical != nullptr && _vertical->isVisible() ? _vertical->value() : _scrollY);
        return;
}

void ScrollArea::barChanged(void *arg, ScrollBar &, uint32_t) {
        static_cast<ScrollArea *>(arg)->follow();
        return;
}

bool ScrollArea::wheelTurned(int notches) {
        // Up is positive: the content moves down, so the offset goes back.
        if (_vertical != nullptr && _vertical->isVisible()) {
                scrollTo(_scrollX, int64_t(_scrollY) - int64_t(notches) * WheelLines * _lineY);
                return true;
        }
        if (_horizontal != nullptr && _horizontal->isVisible()) {
                scrollTo(int64_t(_scrollX) - int64_t(notches) * WheelLines * _lineX, _scrollY);
                return true;
        }
        return false;
}

void ScrollArea::paint(Painter &painter) {
        const Rect v = viewport();
        // The corner between two bars.
        if (_vertical != nullptr && _vertical->isVisible() && _horizontal != nullptr && _horizontal->isVisible())
                (void)painter.fill(
                        Rect{uint16_t(v.right()), uint16_t(v.bottom()), ScrollBar::Breadth, ScrollBar::Breadth},
                        painter.palette().color(Role::Face));
        if (v.isEmpty() || !painter.isVisible(v)) return;
        // The content through a painter of its own, clipped to the viewport.
        const size_t need = painter.clippedStorage();
        Rect         local[LocalPieces];
        List<Rect>   more(MemSpace::Psram);
        Span<Rect>   storage(local, LocalPieces);
        if (need > LocalPieces) {
                if (more.reserve(need).isError()) return; // drawn when there is memory
                for (size_t i = 0; i < need; ++i) (void)more.append(Rect());
                storage = more.span();
        }
        Painter clipped(painter, v, storage);
        paintContent(clipped, v);
        return;
}

void ScrollArea::paintContent(Painter &painter, const Rect &viewport) {
        (void)painter.fill(viewport, painter.palette().color(Role::Face));
        return;
}

BMDS_NAMESPACE_END
