/**
 * @file      widget.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Widgets: the base, labels, buttons and box layouts (devplan/gui.md §4b).
 */

#include <cstring>
#include <bmds/bevel.h>
#include <bmds/font.h>
#include <bmds/guiwindow.h>
#include <bmds/linelayout.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        // @p text into @p out (@p size bytes with the NUL), cut on a character's boundary.
        void copyText(char *out, size_t size, StringView text) {
                size_t n = text.size() < size - 1 ? text.size() : size - 1;
                if (n < text.size())
                        while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xc0u) == 0x80u) --n;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

        uint16_t column(uint32_t x) {
                return static_cast<uint16_t>(x & ~uint32_t(GuiStyle::Column - 1));
        }

        uint16_t columnUp(uint32_t x) {
                return static_cast<uint16_t>((x + GuiStyle::Column - 1) & ~uint32_t(GuiStyle::Column - 1));
        }

        // A row of text's top in @p r, centered (@p down: a pixel lower, pressed in).
        uint16_t textTop(const Rect &r, bool down) {
                const uint32_t y = r.y + (r.height > GuiStyle::TextHeight ? (r.height - GuiStyle::TextHeight) / 2 : 0);
                return static_cast<uint16_t>(y + (down ? 1 : 0));
        }

} // namespace

// ---- Widget

Widget::Widget(Widget *parent) : _parent(parent) {
        if (parent != nullptr) parent->add(this);
}

Widget::~Widget() {
        GuiWindow *w = window();
        if (w != nullptr && static_cast<Widget *>(w) != this) w->forget(this);
        while (_first != nullptr) {
                Widget *child = _first;
                remove(child);
                child->_parent = nullptr;
                if (child->_owned) {
                        child->~Widget();
                        Memory::free(child);
                }
        }
        if (_parent != nullptr) _parent->remove(this);
}

void Widget::destroy() {
        Widget *parent = _parent;
        if (parent != nullptr) parent->update();
        if (_owned) {
                this->~Widget();
                Memory::free(this);
        }
        return;
}

void Widget::add(Widget *child) {
        if (_last != nullptr) {
                _last->_next = child;
        } else {
                _first = child;
        }
        _last = child;
        child->_next = nullptr;
        return;
}

void Widget::remove(Widget *child) {
        Widget *prev = nullptr;
        for (Widget *w = _first; w != nullptr; prev = w, w = w->_next) {
                if (w != child) continue;
                if (prev != nullptr) {
                        prev->_next = w->_next;
                } else {
                        _first = w->_next;
                }
                if (_last == w) _last = prev;
                w->_next = nullptr;
                return;
        }
        return;
}

GuiWindow *Widget::window() const {
        const Widget *w = this;
        while (w->_parent != nullptr) w = w->_parent;
        return w->_window;
}

void Widget::setGeometry(const Rect &rect) {
        if (rect == _rect) return;
        update(); // where it was
        const bool sized = rect.width != _rect.width || rect.height != _rect.height;
        _rect = rect;
        update();
        if (sized) resized();
        return;
}

void Widget::setVisible(bool visible) {
        if (visible == _visible) return;
        _visible = visible;
        update();
        return;
}

void Widget::setEnabled(bool enabled) {
        if (enabled == _enabled) return;
        _enabled = enabled;
        update();
        return;
}

void Widget::update() {
        GuiWindow *w = window();
        if (w != nullptr && !_rect.isEmpty()) w->invalidate(_rect);
        return;
}

Widget *Widget::widgetAt(int x, int y) {
        for (Widget *c = _first; c != nullptr; c = c->_next) {
                if (!c->_visible) continue;
                const Rect &r = c->_rect;
                if (x >= r.x && y >= r.y && uint32_t(x) < r.right() && uint32_t(y) < r.bottom())
                        return c->widgetAt(x, y);
        }
        return this;
}

// ---- Label

Label::Label(Widget *parent, StringView text) : Widget(parent) {
        copyText(_text, sizeof(_text), text);
}

void Label::setText(StringView text) {
        char next[MaxText + 1];
        copyText(next, sizeof(next), text);
        if (std::strcmp(next, _text) == 0) return;
        std::memcpy(_text, next, sizeof(_text));
        update();
        return;
}

void Label::setAlign(Align align) {
        _align = align;
        update();
        return;
}

void Label::setFrame(Bevel::Style style) {
        _frame = style;
        _framed = true;
        update();
        return;
}

GuiSize Label::sizeHint() const {
        const uint32_t frame = _framed ? 2u * Bevel::width(_frame) : 0u;
        return GuiSize{columnUp(Fonts::unscii16.textWidth(StringView(_text)) + 2 * GuiStyle::ButtonPad + frame),
                       GuiStyle::ButtonHeight};
}

void Label::paint(Painter &painter) {
        const Rect    &outer = geometry();
        const Rect     r = _framed ? Bevel::inside(outer, _frame) : outer;
        const uint32_t bg = painter.palette().color(Role::Face);
        const uint32_t fg = painter.palette().color(isEnabled() ? Role::FaceText : Role::DisabledText);
        const uint32_t width = Fonts::unscii16.textWidth(StringView(_text));
        uint32_t       x = r.x + GuiStyle::ButtonPad;
        if (_align == Align::Center && width < r.width) x = r.x + (r.width - width) / 2;
        if (_align == Align::Right && width + GuiStyle::ButtonPad < r.width)
                x = r.right() - GuiStyle::ButtonPad - width;
        x = column(x);
        if (x < r.x) x = columnUp(r.x);
        if (_framed) drawBevel(painter, outer, _frame);
        (void)painter.fill(r, bg);
        if (_text[0] != '\0' && x < r.right())
                (void)painter.text(static_cast<uint16_t>(x), textTop(r, false), StringView(_text), Fonts::unscii16, fg,
                                   bg, static_cast<uint16_t>(r.right() - x));
        return;
}

// ---- Button

Button::Button(Widget *parent, StringView text) : Widget(parent) {
        copyText(_text, sizeof(_text), text);
}

void Button::setText(StringView text) {
        char next[MaxText + 1];
        copyText(next, sizeof(next), text);
        if (std::strcmp(next, _text) == 0) return;
        std::memcpy(_text, next, sizeof(_text));
        update();
        return;
}

void Button::setIcon(const Icon *icon) {
        if (icon == _icon) return;
        _icon = icon;
        update();
        return;
}

void Button::setLook(Look look) {
        _look = look;
        update();
        return;
}

void Button::setChecked(bool checked) {
        if (checked == _checked) return;
        _checked = checked;
        update();
        return;
}

GuiSize Button::sizeHint() const {
        const uint32_t icon = _icon != nullptr && _icon->isValid() ? GuiStyle::IconSize + GuiStyle::IconGap : 0;
        if (_text[0] == '\0' && icon != 0)
                return GuiSize{columnUp(GuiStyle::IconSize + GuiStyle::ButtonPad), GuiStyle::ButtonHeight};
        return GuiSize{columnUp(Fonts::unscii16.textWidth(StringView(_text)) + 2 * GuiStyle::ButtonPad + icon),
                       GuiStyle::ButtonHeight};
}

bool Button::inside(int x, int y) const {
        const Rect &r = geometry();
        return x >= r.x && y >= r.y && uint32_t(x) < r.right() && uint32_t(y) < r.bottom();
}

void Button::setCentered(bool centered) {
        _centered = centered;
        update();
        return;
}

void Button::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &r = geometry();
        const bool           pressed = _down && _over;
        const bool           in = pressed || _checked; // drawn pressed in
        // The edge: raised, or sunk while pressed or checked; a flat button's only under the pointer.
        bool         edged = true;
        Bevel::Style style = in ? Bevel::Style::Sunken : Bevel::Style::Raised;
        if (_look == Look::Flat) {
                edged = in || _over;
                style = in ? Bevel::Style::Status : Bevel::Style::Thin;
        }
        const Rect     face = edged ? Bevel::inside(r, style) : r;
        const uint32_t bg = _checked && !pressed ? p.color(Role::FaceLight) : p.color(Role::Face);
        const uint32_t fg = isEnabled() ? p.color(Role::FaceText) : p.color(Role::DisabledText);
        if (edged) drawBevel(painter, r, style);
        (void)painter.fill(face, bg);
        uint16_t x = columnUp(r.x + GuiStyle::ButtonPad);
        if (_icon != nullptr && _icon->isValid() && _icon->width <= GuiStyle::IconSize &&
            _icon->height <= GuiStyle::IconSize && x + _icon->width <= r.right()) {
                // The icon first, a pixel lower pressed in as the text, then the text after a gap.
                const uint16_t ix = columnUp(r.x + GuiStyle::ButtonPad / 2);
                const uint16_t iy = static_cast<uint16_t>(textTop(r, in) + (GuiStyle::TextHeight - _icon->height) / 2);
                (void)painter.image(ix, iy, *_icon, bg);
                x = columnUp(ix + _icon->width + GuiStyle::IconGap);
        }
        const uint32_t textWidth = Fonts::unscii16.textWidth(StringView(_text));
        if (_centered && _icon == nullptr && textWidth + 2 * GuiStyle::ButtonPad < r.width)
                x = column(r.x + (r.width - textWidth) / 2);
        if (_text[0] != '\0' && x + GuiStyle::ButtonPad / 2 < r.right()) {
                // Cut to the room, with the pad at the right kept; a pixel lower pressed in.
                const uint32_t room = r.right() - x - GuiStyle::ButtonPad / 2;
                (void)painter.text(x, textTop(r, in), StringView(_text), Fonts::unscii16, fg, bg,
                                   static_cast<uint16_t>(room));
        }
        return;
}

void Button::mousePressed(const WindowManager::MouseEvent &event) {
        if (!isEnabled()) return;
        if (event.button == PointerEvent::Left) {
                _down = true;
                _over = true;
                update();
        }
        if (_pressed != nullptr) {
                GuiWindow *w = window();
                const Rect s = w != nullptr ? w->screenRect() : Rect();
                _pressed(_pressedArg, *this, event.button, static_cast<uint16_t>(s.x + event.x),
                         static_cast<uint16_t>(s.y + event.y));
        }
        return;
}

void Button::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !_down) return;
        _down = false;
        const bool over = inside(event.x, event.y);
        _over = over;
        update();
        if (over && isEnabled() && _clicked != nullptr) _clicked(_clickedArg, *this);
        return;
}

void Button::mouseMoved(const WindowManager::MouseEvent &event) {
        const bool over = inside(event.x, event.y);
        if (over == _over) return;
        _over = over;
        update();
        return;
}

void Button::mouseLeft() {
        if (!_over && !_down) return;
        _over = false;
        _down = false;
        update();
        return;
}

// ---- BoxLayout

BoxLayout::BoxLayout(Widget *parent, Direction direction) : Widget(parent), _direction(direction) {}

void BoxLayout::setSpacing(uint16_t spacing) {
        _spacing = spacing;
        relayout();
        return;
}

void BoxLayout::setMargins(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom) {
        _margins[0] = left;
        _margins[1] = top;
        _margins[2] = right;
        _margins[3] = bottom;
        relayout();
        return;
}

void BoxLayout::setFrame(Bevel::Style style) {
        _frame = style;
        _framed = true;
        update();
        return;
}

GuiSize BoxLayout::sizeHint() const {
        GuiSize size;
        size_t  n = 0;
        for (Widget *c = firstChild(); c != nullptr; c = c->next()) {
                if (!c->isVisible()) continue;
                const GuiSize h = c->sizeHint();
                if (_direction == Direction::Horizontal) {
                        size.width = static_cast<uint16_t>(size.width + h.width);
                        if (h.height > size.height) size.height = h.height;
                } else {
                        size.height = static_cast<uint16_t>(size.height + h.height);
                        if (h.width > size.width) size.width = h.width;
                }
                ++n;
        }
        const uint32_t gaps = n > 1 ? uint32_t(_spacing) * (n - 1) : 0;
        if (_direction == Direction::Horizontal) {
                size.width = static_cast<uint16_t>(size.width + gaps);
        } else {
                size.height = static_cast<uint16_t>(size.height + gaps);
        }
        size.width = static_cast<uint16_t>(size.width + _margins[0] + _margins[2]);
        size.height = static_cast<uint16_t>(size.height + _margins[1] + _margins[3]);
        return size;
}

void BoxLayout::relayout() {
        const Rect      &g = geometry();
        const Rect       r = g.width > _margins[0] + _margins[2] && g.height > _margins[1] + _margins[3]
                                     ? Rect{uint16_t(g.x + _margins[0]), uint16_t(g.y + _margins[1]),
                                      uint16_t(g.width - _margins[0] - _margins[2]),
                                      uint16_t(g.height - _margins[1] - _margins[3])}
                                     : Rect{g.x, g.y, 0, 0};
        LineLayout::Item items[MaxItems];
        Widget          *widgets[MaxItems];
        size_t           n = 0;
        const bool       across = _direction == Direction::Horizontal;
        for (Widget *c = firstChild(); c != nullptr && n < MaxItems; c = c->next()) {
                if (!c->isVisible()) continue;
                const GuiSize h = c->sizeHint();
                items[n].hint = across ? h.width : h.height;
                items[n].most = across ? c->maximumWidth() : UINT16_MAX;
                items[n].stretch = c->stretch();
                widgets[n++] = c;
        }
        uint16_t pos[MaxItems], size[MaxItems];
        LineLayout::arrange(Span<const LineLayout::Item>(items, n), across ? r.width : r.height, _spacing,
                            across ? GuiStyle::Column : 1, Span<uint16_t>(pos, n), Span<uint16_t>(size, n));
        for (size_t i = 0; i < n; ++i) {
                const Rect c = across ? Rect{uint16_t(r.x + pos[i]), r.y, size[i], r.height}
                                      : Rect{r.x, uint16_t(r.y + pos[i]), r.width, size[i]};
                widgets[i]->setGeometry(c); // a child that moves asks for its old and new places
        }
        return;
}

void BoxLayout::resized() {
        relayout();
        return;
}

void BoxLayout::paint(Painter &painter) {
        // Its frame, in the margins; the gaps: what's inside it less its visible children's.
        Region gaps;
        if (_framed) {
                drawBevel(painter, geometry(), _frame);
                gaps.add(Bevel::inside(geometry(), _frame));
        } else {
                gaps.add(geometry());
        }
        Rect   pieces[Region::MaxRects];
        size_t n = 0;
        for (Widget *c = firstChild(); c != nullptr; c = c->next()) {
                if (!c->isVisible() || c->geometry().isEmpty()) continue;
                Region rest;
                for (const Rect &g : gaps.rects()) {
                        Rect   out[4];
                        size_t k = subtract(g, c->geometry(), out);
                        for (size_t i = 0; i < k; ++i) rest.add(out[i]);
                }
                gaps = rest;
        }
        for (const Rect &g : gaps.rects()) pieces[n++] = g;
        if (n > 0) (void)painter.fill(Span<const Rect>(pieces, n), painter.palette().color(Role::Face));
        return;
}

BMDS_NAMESPACE_END
