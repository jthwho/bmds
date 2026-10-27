/**
 * @file      furniture.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Tool bars, status bars, splitters and progress bars (devplan/filebrowser.md §4e).
 */

#include <bmds/bevel.h>
#include <bmds/furniture.h>
#include <bmds/furniturelayout.h>
#include <bmds/guiwindow.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t BarMargin = 2;

        // An etched line across a tool bar: a shadow and a highlight side by side.
        class ToolSeparator final : public Widget {
                public:
                        explicit ToolSeparator(Widget *parent) : Widget(parent) {}

                        GuiSize sizeHint() const override { return GuiSize{8, 0}; }

                        void paint(Painter &painter) override {
                                const WindowPalette &p = painter.palette();
                                const Rect          &r = geometry();
                                (void)painter.fill(r, p.color(Role::Face));
                                const uint16_t x = static_cast<uint16_t>(r.x + r.width / 2 - 1);
                                const uint16_t y = static_cast<uint16_t>(r.y + 2);
                                const uint16_t h = static_cast<uint16_t>(r.height > 4 ? r.height - 4 : 0);
                                (void)painter.fill(Rect{x, y, 1, h}, p.color(Role::FaceShadow));
                                (void)painter.fill(Rect{uint16_t(x + 1), y, 1, h}, p.color(Role::FaceHighlight));
                                return;
                        }
        };

        // A status bar's field: a sunken label of a fixed width, or stretching.
        class StatusField final : public Label {
                public:
                        StatusField(Widget *parent, uint16_t width) : Label(parent), _width(width) {
                                setFrame(Bevel::Style::Status);
                        }

                        GuiSize sizeHint() const override {
                                return _width != 0 ? GuiSize{_width, uint16_t(buttonHeight() - 2)} : GuiSize{0, 0};
                        }

                private:
                        uint16_t _width;
        };

} // namespace

// ---- ToolBar

ToolBar::ToolBar(Widget *parent) : BoxLayout(parent, Direction::Horizontal) {
        setMargins(BarMargin + 2, BarMargin, BarMargin, BarMargin); // left a column
}

Button *ToolBar::addButton(const Icon *icon, StringView text, Button::Clicked fn, void *arg) {
        auto *b = Widget::create<Button>(this, icon != nullptr ? StringView() : text);
        if (b == nullptr) return nullptr;
        b->setIcon(icon);
        b->setLook(Button::Look::Flat);
        b->setFocusable(false);
        b->setClicked(fn, arg);
        relayout();
        return b;
}

Widget *ToolBar::addSeparator() {
        auto *s = Widget::create<ToolSeparator>(this);
        if (s != nullptr) relayout();
        return s;
}

GuiSize ToolBar::sizeHint() const {
        GuiSize h = BoxLayout::sizeHint();
        h.height = static_cast<uint16_t>(buttonHeight() + 2 * BarMargin);
        return h;
}

// ---- StatusBar

StatusBar::StatusBar(Widget *parent) : BoxLayout(parent, Direction::Horizontal) {
        setMargins(0, BarMargin, 0, 0);
        setSpacing(4);
}

int StatusBar::addField(uint16_t width) {
        if (_count == MaxFields) return -1;
        auto *f = Widget::create<StatusField>(this, static_cast<uint16_t>((width + 3u) & ~3u));
        if (f == nullptr) return -1;
        if (width == 0) f->setStretch(1);
        _fields[_count] = f;
        relayout();
        return int(_count++);
}

void StatusBar::setText(size_t field, StringView text) {
        if (field < _count) _fields[field]->setText(text);
        return;
}

GuiSize StatusBar::sizeHint() const {
        GuiSize h = BoxLayout::sizeHint();
        h.height = Height;
        return h;
}

// ---- Splitter

Splitter::Splitter(Widget *parent, Orientation orientation) : Widget(parent), _orientation(orientation) {}

void Splitter::setOrientation(Orientation orientation) {
        if (orientation == _orientation) return;
        _orientation = orientation;
        place();
        update();
        return;
}

void Splitter::setWidgets(Widget *first, Widget *second) {
        _first = first;
        _second = second;
        place();
        return;
}

void Splitter::setLeast(uint16_t first, uint16_t second) {
        _leastFirst = first;
        _leastSecond = second;
        place();
        return;
}

void Splitter::setPosition(uint16_t firstSize) {
        _wanted = firstSize;
        place();
        return;
}

uint16_t Splitter::length() const {
        return _orientation == Orientation::Horizontal ? geometry().width : geometry().height;
}

Rect Splitter::handle() const {
        const Rect &r = geometry();
        if (alone() != nullptr) return Rect();
        return _orientation == Orientation::Horizontal
                       ? Rect{uint16_t(r.x + _position), r.y, SplitterLayout::Handle, r.height}
                       : Rect{r.x, uint16_t(r.y + _position), r.width, SplitterLayout::Handle};
}

void Splitter::resized() {
        place();
        return;
}

// The sides where the position puts them: the one asked for, as the room allows.
// The side shown alone when the other is hidden (all the room, no handle), or null.
Widget *Splitter::alone() const {
        if (_first != nullptr && !_first->isVisible()) return _second;
        if (_second != nullptr && !_second->isVisible()) return _first;
        return nullptr;
}

void Splitter::place() {
        const Rect &r = geometry();
        if (Widget *only = alone(); only != nullptr) {
                only->setGeometry(r);
                update();
                return;
        }
        const bool     across = _orientation == Orientation::Horizontal;
        const uint16_t len = length();
        _position = SplitterLayout::firstSize(_wanted, len, _leastFirst, _leastSecond, across ? 4 : 1);
        const uint16_t rest = static_cast<uint16_t>(
                len > _position + SplitterLayout::Handle ? len - _position - SplitterLayout::Handle : 0);
        const uint16_t secondAt = static_cast<uint16_t>(_position + SplitterLayout::Handle);
        if (_first != nullptr)
                _first->setGeometry(across ? Rect{r.x, r.y, _position, r.height} : Rect{r.x, r.y, r.width, _position});
        if (_second != nullptr)
                _second->setGeometry(across ? Rect{uint16_t(r.x + secondAt), r.y, rest, r.height}
                                            : Rect{r.x, uint16_t(r.y + secondAt), r.width, rest});
        update();
        return;
}

void Splitter::paint(Painter &painter) {
        // The handle, and where no side is (none set, or no room).
        (void)painter.fill(handle(), painter.palette().color(Role::Face));
        if (_first == nullptr || _second == nullptr) {
                const Rect &r = geometry();
                Region      rest;
                rest.add(r);
                for (Widget *side : {_first, _second}) {
                        if (side == nullptr) continue;
                        Region cut;
                        for (const Rect &g : rest.rects()) {
                                Rect         out[4];
                                const size_t k = subtract(g, side->geometry(), out);
                                for (size_t i = 0; i < k; ++i) cut.add(out[i]);
                        }
                        rest = cut;
                }
                for (const Rect &g : rest.rects()) (void)painter.fill(g, painter.palette().color(Role::Face));
        }
        return;
}

void Splitter::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        const Rect h = handle();
        const int  at = _orientation == Orientation::Horizontal ? event.x - h.x : event.y - h.y;
        _grab = at >= 0 && at < SplitterLayout::Handle ? at : -1;
        return;
}

void Splitter::mouseMoved(const WindowManager::MouseEvent &event) {
        if (_grab < 0) return;
        const Rect &r = geometry();
        const int   to = (_orientation == Orientation::Horizontal ? event.x - r.x : event.y - r.y) - _grab;
        const auto  wanted = static_cast<uint16_t>(to < 0 ? 0 : to > UINT16_MAX ? UINT16_MAX : to);
        if (SplitterLayout::firstSize(wanted, length(), _leastFirst, _leastSecond,
                                      _orientation == Orientation::Horizontal ? 4 : 1) == _position)
                return;
        setPosition(wanted);
        return;
}

void Splitter::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button == PointerEvent::Left) _grab = -1;
        return;
}

WindowManager::Pointer Splitter::pointerAt(int x, int y) const {
        const Rect h = handle();
        if (_grab >= 0 || (x >= h.x && y >= h.y && uint32_t(x) < h.right() && uint32_t(y) < h.bottom()))
                return _orientation == Orientation::Horizontal ? WindowManager::Pointer::Horizontal
                                                               : WindowManager::Pointer::Vertical;
        return Widget::pointerAt(x, y);
}

// ---- ProgressBar

ProgressBar::ProgressBar(Widget *parent) : Widget(parent) {}

uint16_t ProgressBar::inner() const {
        const uint16_t w = geometry().width;
        return static_cast<uint16_t>(w > 2 * 4 ? w - 2 * 4 : 0); // the bevel and 3 pixels each side
}

void ProgressBar::setMaximum(uint64_t maximum) {
        const uint16_t before = ProgressLayout::blocks(_value, _maximum, inner());
        _maximum = maximum;
        if (ProgressLayout::blocks(_value, _maximum, inner()) != before) update();
        return;
}

void ProgressBar::setValue(uint64_t value) {
        const uint16_t before = ProgressLayout::blocks(_value, _maximum, inner());
        _value = value;
        if (ProgressLayout::blocks(_value, _maximum, inner()) != before) update();
        return;
}

GuiSize ProgressBar::sizeHint() const {
        return GuiSize{160, 20};
}

void ProgressBar::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &r = geometry();
        drawBevel(painter, r, Bevel::Style::Status);
        const Rect in = Bevel::inside(r, Bevel::Style::Status);
        (void)painter.fill(in, p.color(Role::Face));
        const uint16_t n = ProgressLayout::blocks(_value, _maximum, inner());
        if (n == 0 || in.height < 4) return;
        Rect           blocks[32];
        size_t         k = 0;
        const uint16_t step = ProgressLayout::Block + ProgressLayout::Gap;
        for (uint16_t i = 0; i < n; ++i) {
                // On columns (the bar is), 2 pixels in from the top and bottom.
                blocks[k++] = Rect{uint16_t(in.x + 3 + i * step), uint16_t(in.y + 2), ProgressLayout::Block,
                                   uint16_t(in.height - 4)};
                if (k == 32 || i + 1 == n) {
                        (void)painter.fill(Span<const Rect>(blocks, k), p.color(Role::Highlight));
                        k = 0;
                }
        }
        return;
}

BMDS_NAMESPACE_END
