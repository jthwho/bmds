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
#include <bmds/bitmapfont.h>
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

void GuiStyle::drawArrow(Painter &painter, const Rect &rect, Arrow way, uint32_t color) {
        const int  cx = rect.x + rect.width / 2, cy = rect.y + rect.height / 2;
        const bool vertical = way == Arrow::Up || way == Arrow::Down;
        Rect       rows[4];
        for (int i = 0; i < 4; ++i) {
                const int half = way == Arrow::Up || way == Arrow::Left ? i : 3 - i; // the row's half width
                rows[i] = vertical ? Rect{uint16_t(cx - half), uint16_t(cy - 2 + i), uint16_t(2 * half + 1), 1}
                                   : Rect{uint16_t(cx - 2 + i), uint16_t(cy - half), 1, uint16_t(2 * half + 1)};
        }
        (void)painter.fill(Span<const Rect>(rows), color);
        return;
}

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
        const bool    sized = rect.width != _rect.width || rect.height != _rect.height;
        const int32_t dx = int32_t(rect.x) - _rect.x, dy = int32_t(rect.y) - _rect.y;
        _rect = rect;
        update();
        if (sized) {
                resized(); // laid out again, children and all
        } else {
                // Moved only: the children (in the window's coordinates, as it is) go with it, or a
                // layout moved down a window's resize would leave its contents behind.
                for (Widget *c = _first; c != nullptr; c = c->_next) {
                        const Rect   &g = c->_rect;
                        const int32_t x = int32_t(g.x) + dx, y = int32_t(g.y) + dy;
                        c->setGeometry(Rect{uint16_t(x < 0 ? 0 : x), uint16_t(y < 0 ? 0 : y), g.width, g.height});
                }
        }
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

bool Widget::hasFocus() const {
        const GuiWindow *w = window();
        return w != nullptr && w->focusWidget() == this;
}

void Widget::setFocus() {
        GuiWindow *w = window();
        if (w != nullptr) w->setFocusWidget(this);
        return;
}

void Widget::update() {
        GuiWindow *w = window();
        if (w != nullptr && !_rect.isEmpty()) w->invalidate(_rect);
        return;
}

namespace {

        bool hits(const Rect &r, int x, int y) {
                return x >= r.x && y >= r.y && uint32_t(x) < r.right() && uint32_t(y) < r.bottom();
        }

} // namespace

Widget *Widget::widgetAt(int x, int y) {
        for (Widget *c = _first; c != nullptr; c = c->_next) {
                if (!c->_visible || !hits(c->_rect, x, y)) continue;
                Rect clip;
                if (childClip(*c, clip) && !hits(clip, x, y)) continue;
                return c->widgetAt(x, y);
        }
        return this;
}

void Widget::fillOutsideChildren(Painter &painter, const Rect &area, uint32_t rgb) const {
        Region gaps;
        gaps.add(area);
        for (Widget *c = _first; c != nullptr; c = c->_next) {
                if (!c->_visible || !c->_rect.intersects(area)) continue;
                Region rest;
                for (const Rect &g : gaps.rects()) {
                        Rect   out[4];
                        size_t k = subtract(g, c->_rect, out);
                        for (size_t i = 0; i < k; ++i) rest.add(out[i]);
                }
                gaps = rest;
        }
        Rect   pieces[Region::MaxRects];
        size_t n = 0;
        for (const Rect &g : gaps.rects()) pieces[n++] = g;
        if (n > 0) (void)painter.fill(Span<const Rect>(pieces, n), rgb);
        return;
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

void Label::clearFrame() {
        if (!_framed) return;
        _framed = false;
        update();
        return;
}

void Label::setHighlighted(bool highlighted) {
        if (highlighted == _highlighted) return;
        _highlighted = highlighted;
        update();
        return;
}

void Label::setBuddy(Widget *buddy) {
        _buddy = buddy;
        update();
        return;
}

// The text as drawn: with a buddy, its marks taken out.
Mnemonic::Stripped Label::shown(char (&out)[MaxText + 1]) const {
        if (_buddy != nullptr) return Mnemonic::strip(StringView(_text), Span<char>(out, sizeof(out)));
        const size_t n = std::strlen(_text);
        std::memcpy(out, _text, n + 1);
        Mnemonic::Stripped s;
        s.length = n;
        return s;
}

uint32_t Label::mnemonic() const {
        char shownText[MaxText + 1];
        return _buddy != nullptr ? shown(shownText).key : 0;
}

void Label::mnemonicActivated() {
        if (_buddy != nullptr) _buddy->setFocus();
        return;
}

GuiSize Label::sizeHint() const {
        const uint32_t frame = _framed ? 2u * Bevel::width(_frame) : 0u;
        char           text[MaxText + 1];
        const size_t   n = shown(text).length;
        const uint16_t width =
                columnUp(Fonts::unscii16.textWidth(StringView(text, n)) + 2 * GuiStyle::ButtonPad + frame);
        return GuiSize{width > _minWidth ? width : _minWidth, GuiStyle::ButtonHeight};
}

void Label::paint(Painter &painter) {
        const Rect    &outer = geometry();
        const Rect     r = _framed ? Bevel::inside(outer, _frame) : outer;
        const uint32_t           face = painter.palette().color(Role::Face);
        const uint32_t           bg = _highlighted ? painter.palette().color(Role::Highlight) : face;
        const uint32_t           fg = painter.palette().color(_highlighted  ? Role::HighlightText
                                                              : isEnabled() ? Role::FaceText
                                                                            : Role::DisabledText);
        char                     shownText[MaxText + 1];
        const Mnemonic::Stripped m = shown(shownText);
        const StringView         text(shownText, m.length);
        const uint32_t           width = Fonts::unscii16.textWidth(text);
        uint32_t       x = r.x + GuiStyle::ButtonPad;
        if (_align == Align::Center && width < r.width) x = r.x + (r.width - width) / 2;
        if (_align == Align::Right && width + GuiStyle::ButtonPad < r.width)
                x = r.right() - GuiStyle::ButtonPad - width;
        x = column(x);
        if (x < r.x) x = columnUp(r.x);
        if (_framed) drawBevel(painter, outer, _frame);
        (void)painter.fill(r, face);
        if (_highlighted && !text.isEmpty() && x >= r.x + 4) {
                // Lit around the text only, as a selected label is.
                const uint32_t right = x + width + 4 < r.right() ? x + width + 4 : r.right();
                (void)painter.fill(Rect{uint16_t(x - 4), uint16_t(textTop(r, false) - 1), uint16_t(right - (x - 4)),
                                        uint16_t(GuiStyle::TextHeight + 2)},
                                   bg);
        }
        if (!text.isEmpty() && x < r.right()) {
                (void)painter.text(static_cast<uint16_t>(x), textTop(r, false), text, Fonts::unscii16, fg, bg,
                                   static_cast<uint16_t>(r.right() - x));
                Mnemonic::draw(painter, static_cast<uint16_t>(x), textTop(r, false), text, m.at, fg, r.right());
        }
        return;
}

// ---- Button

Button::Button(Widget *parent, StringView text) : Widget(parent) {
        setFocusable(true);
        const Mnemonic::Stripped m = Mnemonic::strip(text, Span<char>(_text, sizeof(_text)));
        _underline = m.at;
        _key = m.key;
}

void Button::setText(StringView text) {
        char                     next[MaxText + 1];
        const Mnemonic::Stripped m = Mnemonic::strip(text, Span<char>(next, sizeof(next)));
        if (std::strcmp(next, _text) == 0 && m.at == _underline) return;
        std::memcpy(_text, next, sizeof(_text));
        _underline = m.at;
        _key = m.key;
        update();
        return;
}

void Button::click() {
        if (!isEnabled()) return;
        if (_clicked != nullptr) {
                _clicked(_clickedArg, *this);
        } else {
                press(0);
        }
        return;
}

void Button::press(uint8_t which) {
        if (_pressed == nullptr) return;
        GuiWindow *w = window();
        const Rect s = w != nullptr ? w->screenRect() : Rect();
        const Rect r = geometry();
        _pressed(_pressedArg, *this, which, static_cast<uint16_t>(s.x + r.x), static_cast<uint16_t>(s.y + r.bottom()));
        return;
}

void Button::mnemonicActivated() {
        setFocus();
        click();
        return;
}

bool Button::keyPressed(const KeyEvent &event) {
        constexpr uint8_t F10 = HidKey::F1 + 9;
        if (!isEnabled() || event.hasCtrl() || event.hasAlt()) return false;
        if (event.usage == HidKey::Space || event.usage == HidKey::Enter || event.usage == HidKey::KeypadEnter) {
                click();
                return true;
        }
        if (event.usage == HidKey::Application || (event.usage == F10 && event.hasShift())) {
                if (_pressed == nullptr) return false;
                press(PointerEvent::Right);
                return true;
        }
        return false;
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
        GuiWindow           *window = this->window();
        const bool           isDefault = window != nullptr && window->defaultButton() == this;
        const Rect           r = isDefault ? Rect{uint16_t(geometry().x + 1), uint16_t(geometry().y + 1),
                                        uint16_t(geometry().width - 2), uint16_t(geometry().height - 2)}
                                           : geometry();
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
        if (isDefault) {
                // The default button's dark outline, around its bevel.
                const Rect &g = geometry();
                const Rect  outline[] = {{g.x, g.y, g.width, 1},
                                         {g.x, uint16_t(g.bottom() - 1), g.width, 1},
                                         {g.x, uint16_t(g.y + 1), 1, uint16_t(g.height - 2)},
                                         {uint16_t(g.right() - 1), uint16_t(g.y + 1), 1, uint16_t(g.height - 2)}};
                (void)painter.fill(Span<const Rect>(outline), p.color(Role::FaceDark));
        }
        if (edged) drawBevel(painter, r, style);
        (void)painter.fill(face, bg);
        if (hasFocus() && window != nullptr && window->isActive() && r.width > 4 && r.height > 4) {
                // The focus: a ring in the highlight's color where the bevel's inner line is (its
                // outer edge without one), outside the text's rows, so it never crosses the text.
                const uint16_t inset = edged && Bevel::width(style) >= 2 ? 1 : 0;
                const Rect     f{uint16_t(r.x + inset), uint16_t(r.y + inset), uint16_t(r.width - 2 * inset),
                             uint16_t(r.height - 2 * inset)};
                const Rect     ring[] = {{f.x, f.y, f.width, 1},
                                         {f.x, uint16_t(f.bottom() - 1), f.width, 1},
                                         {f.x, uint16_t(f.y + 1), 1, uint16_t(f.height - 2)},
                                         {uint16_t(f.right() - 1), uint16_t(f.y + 1), 1, uint16_t(f.height - 2)}};
                (void)painter.fill(Span<const Rect>(ring), p.color(Role::Highlight));
        }
        uint16_t       x = columnUp(r.x + GuiStyle::ButtonPad);
        const uint16_t ix = columnUp(r.x + GuiStyle::ButtonPad / 2); // the icon's column
        // Room for the icon where it goes (not where text would: a tool button on an odd column).
        if (_icon != nullptr && _icon->isValid() && _icon->width <= GuiStyle::IconSize &&
            _icon->height <= GuiStyle::IconSize && ix + _icon->width <= r.right()) {
                // The icon first, a pixel lower pressed in as the text, then the text after a gap.
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
                Mnemonic::draw(painter, x, textTop(r, in), StringView(_text), _underline, fg, x + room);
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

// ---- CheckBox

namespace {

        constexpr uint16_t CheckGap = 6; // the box to the text

        // The tick, a row each of its 7 × 7 (Windows 95's): the first pixel and the run.
        constexpr uint8_t Tick[7][2] = {{6, 1}, {5, 2}, {0, 1}, {0, 2}, {0, 5}, {1, 3}, {2, 1}};
        constexpr uint8_t TickMore[7][2] = {{0, 0}, {0, 0}, {4, 3}, {3, 3}, {0, 0}, {0, 0}, {0, 0}};

} // namespace

CheckBox::CheckBox(Widget *parent, StringView text) : Widget(parent) {
        setFocusable(true);
        setText(text);
}

void CheckBox::setText(StringView text) {
        char                     next[MaxText + 1];
        const Mnemonic::Stripped m = Mnemonic::strip(text, Span<char>(next, sizeof(next)));
        std::memcpy(_text, next, sizeof(_text));
        _underline = m.at;
        _key = m.key;
        update();
        return;
}

void CheckBox::setChecked(bool checked) {
        if (checked == _checked) return;
        _checked = checked;
        update();
        return;
}

void CheckBox::setHighlighted(bool highlighted) {
        if (highlighted == _highlighted) return;
        _highlighted = highlighted;
        update();
        return;
}

void CheckBox::toggle() {
        if (!isEnabled()) return;
        setChecked(!_checked);
        if (_toggled != nullptr) _toggled(_toggledArg, *this);
        return;
}

GuiSize CheckBox::sizeHint() const {
        return GuiSize{columnUp(BoxSide + CheckGap + Fonts::unscii16.textWidth(StringView(_text)) + GuiStyle::Column),
                       GuiStyle::ButtonHeight};
}

void CheckBox::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &r = geometry();
        const uint32_t       face = p.color(Role::Face);
        (void)painter.fill(r, face);
        // The box: a field's bevel around white (the face when disabled), the tick in the text's colour.
        const Rect box{r.x, uint16_t(r.y + (r.height > BoxSide ? (r.height - BoxSide) / 2 : 0)), BoxSide, BoxSide};
        drawBevel(painter, box, Bevel::Style::Field);
        const Rect     in = Bevel::inside(box, Bevel::Style::Field);
        const uint32_t fg = p.color(isEnabled() ? Role::FaceText : Role::DisabledText);
        (void)painter.fill(in, isEnabled() ? p.color(Role::FaceHighlight) : face);
        if (_checked) {
                Rect   runs[14];
                size_t n = 0;
                for (uint16_t row = 0; row < 7; ++row) {
                        const uint16_t y = uint16_t(in.y + 1 + row);
                        runs[n++] = Rect{uint16_t(in.x + 1 + Tick[row][0]), y, Tick[row][1], 1};
                        if (TickMore[row][1] != 0)
                                runs[n++] = Rect{uint16_t(in.x + 1 + TickMore[row][0]), y, TickMore[row][1], 1};
                }
                (void)painter.fill(Span<const Rect>(runs, n), fg);
        }
        const uint16_t x = columnUp(r.x + BoxSide + CheckGap);
        if (_text[0] == '\0' || x >= r.right()) return;
        const uint16_t   top = textTop(r, false);
        const StringView text(_text);
        const uint32_t   width = Fonts::unscii16.textWidth(text);
        const uint16_t   room = uint16_t(r.right() - x);
        const uint32_t   textBg = _highlighted ? p.color(Role::Highlight) : face;
        const uint32_t   textFg = _highlighted ? p.color(Role::HighlightText) : fg;
        if (_highlighted && x >= r.x + 4) {
                const uint32_t right = x + width + 4 < r.right() ? x + width + 4 : r.right();
                (void)painter.fill(Rect{uint16_t(x - 4), uint16_t(top - 1), uint16_t(right - (x - 4)),
                                        uint16_t(GuiStyle::TextHeight + 2)},
                                   textBg);
        }
        (void)painter.text(x, top, text, Fonts::unscii16, textFg, textBg, room);
        Mnemonic::draw(painter, x, top, text, _underline, textFg, r.right());
        GuiWindow *window = this->window();
        if (hasFocus() && window != nullptr && window->isActive() && top > r.y && x >= r.x + 2) {
                // The focus: a ring around the text, outside its rows.
                const uint16_t w = uint16_t((width < room ? width : room) + 4);
                const Rect     f{uint16_t(x - 2), uint16_t(top - 1), w, uint16_t(GuiStyle::TextHeight + 2)};
                const Rect     ring[] = {{f.x, f.y, f.width, 1},
                                         {f.x, uint16_t(f.bottom() - 1), f.width, 1},
                                         {f.x, uint16_t(f.y + 1), 1, uint16_t(f.height - 2)},
                                         {uint16_t(f.right() - 1), uint16_t(f.y + 1), 1, uint16_t(f.height - 2)}};
                (void)painter.fill(Span<const Rect>(ring), p.color(Role::Highlight));
        }
        return;
}

void CheckBox::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !isEnabled()) return;
        _down = true;
        setFocus();
        return;
}

void CheckBox::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !_down) return;
        _down = false;
        const Rect &r = geometry();
        if (event.x >= r.x && event.y >= r.y && uint32_t(event.x) < r.right() && uint32_t(event.y) < r.bottom())
                toggle();
        return;
}

bool CheckBox::keyPressed(const KeyEvent &event) {
        if (!isEnabled() || event.hasCtrl() || event.hasAlt() || event.usage != HidKey::Space) return false;
        toggle();
        return true;
}

void CheckBox::mnemonicActivated() {
        setFocus();
        toggle();
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

void BoxLayout::setDirection(Direction direction) {
        if (direction == _direction) return;
        _direction = direction;
        relayout();
        update();
        return;
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
        if (_framed) drawBevel(painter, geometry(), _frame);
        fillOutsideChildren(painter, _framed ? Bevel::inside(geometry(), _frame) : geometry(),
                            painter.palette().color(Role::Face));
        return;
}

BMDS_NAMESPACE_END
