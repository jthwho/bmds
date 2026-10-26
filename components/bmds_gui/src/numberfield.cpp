/**
 * @file      numberfield.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A whole number typed or stepped (devplan/gui.md §4u).
 */

#include <cstdio>
#include <bmds/bevel.h>
#include <bmds/bitmapfont.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/lineedit.h>
#include <bmds/numberfield.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint32_t RepeatDelayMs = 400;
        constexpr uint32_t RepeatMs = 50;

} // namespace

NumberField::NumberField(Widget *parent) : Widget(parent) {
        _edit = Widget::create<LineEdit>(this);
        if (_edit != nullptr) {
                _edit->setMaxLength(12);
                _edit->setAccepted(accepted, this);
                _edit->setCancelled(cancelled, this);
                _edit->setFocusLost(accepted, this);
        }
        show();
}

NumberField::~NumberField() {
        stopRepeat();
}

void NumberField::setRange(int32_t minimum, int32_t maximum) {
        _model.setRange(minimum, maximum);
        show();
        return;
}

void NumberField::setSteps(uint32_t step, uint32_t page) {
        _model.setSteps(step, page);
        return;
}

void NumberField::setValue(int32_t value) {
        (void)_model.setValue(value);
        show();
        return;
}

void NumberField::show() {
        if (_edit == nullptr) return;
        char text[16];
        std::snprintf(text, sizeof(text), "%ld", long(_model.value()));
        if (_edit->text() != StringView(text)) _edit->setText(StringView(text));
        return;
}

void NumberField::take() {
        if (_edit == nullptr) return;
        const Result<int64_t> typed = NumberModel::parse(_edit->text());
        const bool            changed = typed.isOk() && _model.setValue(typed.value());
        show(); // as taken: kept in range, or put back
        if (changed && _changed != nullptr) _changed(_changedArg, *this, _model.value());
        return;
}

void NumberField::stepBy(int64_t count, bool page, bool tell) {
        const int32_t before = _model.value();
        if (_edit != nullptr) {
                // What was typed first, then the step from it.
                const Result<int64_t> typed = NumberModel::parse(_edit->text());
                if (typed.isOk()) (void)_model.setValue(typed.value());
        }
        (void)_model.step(count, page);
        show();
        if (tell && _model.value() != before && _changed != nullptr) _changed(_changedArg, *this, _model.value());
        return;
}

void NumberField::accepted(void *arg, LineEdit &) {
        static_cast<NumberField *>(arg)->take();
        return;
}

void NumberField::cancelled(void *arg, LineEdit &) {
        static_cast<NumberField *>(arg)->show();
        return;
}

Rect NumberField::arrows() const {
        const Rect    &r = geometry();
        const uint16_t w = r.width < ArrowWidth ? r.width : ArrowWidth;
        return Rect{uint16_t(r.right() - w), r.y, w, r.height};
}

int NumberField::arrowAt(int x, int y) const {
        const Rect a = arrows();
        if (x < a.x || y < a.y || uint32_t(x) >= a.right() || uint32_t(y) >= a.bottom()) return 0;
        return y < a.y + a.height / 2 ? 1 : -1;
}

GuiSize NumberField::sizeHint() const {
        // Room for the widest value, as the field pads and edges it.
        char           wide[16];
        const int64_t  most = _model.minimum() < 0 && -int64_t(_model.minimum()) > _model.maximum() ? _model.minimum()
                                                                                                    : _model.maximum();
        const int      n = std::snprintf(wide, sizeof(wide), "%lld", static_cast<long long>(most));
        const uint32_t text = Fonts::unscii16.textWidth(StringView(wide, n > 0 ? size_t(n) : 0));
        uint32_t       width = text + 2 * LineEdit::Pad + 2 * Bevel::width(Bevel::Style::Field) + ArrowWidth + 8;
        if (width < 48) width = 48;
        return GuiSize{static_cast<uint16_t>((width + GuiStyle::Column - 1) & ~uint32_t(GuiStyle::Column - 1)),
                       GuiStyle::ButtonHeight};
}

void NumberField::resized() {
        const Rect &r = geometry();
        if (_edit != nullptr)
                _edit->setGeometry(Rect{r.x, r.y, uint16_t(r.width > ArrowWidth ? r.width - ArrowWidth : 0), r.height});
        return;
}

void NumberField::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect           a = arrows();
        if (a.isEmpty()) return;
        const auto half = static_cast<uint16_t>(a.height / 2);
        const Rect parts[2] = {{a.x, a.y, a.width, half},
                               {a.x, uint16_t(a.y + half), a.width, uint16_t(a.height - half)}};
        for (int i = 0; i < 2; ++i) {
                const bool         down = _pressed == (i == 0 ? 1 : -1);
                const Bevel::Style style = down ? Bevel::Style::Sunken : Bevel::Style::Raised;
                drawBevel(painter, parts[i], style);
                const Rect in = Bevel::inside(parts[i], style);
                (void)painter.fill(in, p.color(Role::Face));
                Rect mark = in;
                if (down) {
                        mark.x = static_cast<uint16_t>(mark.x + 1);
                        mark.y = static_cast<uint16_t>(mark.y + 1);
                }
                GuiStyle::drawArrow(painter, mark, i == 0 ? GuiStyle::Arrow::Up : GuiStyle::Arrow::Down,
                                    p.color(isEnabled() ? Role::FaceText : Role::DisabledText));
        }
        return;
}

void NumberField::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        const int way = arrowAt(event.x, event.y);
        if (way == 0) return;
        if (_edit != nullptr) _edit->setFocus(); // the keys and the wheel step it after
        _pressed = static_cast<int8_t>(way);
        _pressValue = _model.value();
        stepBy(way, false, false);
        update();
        GuiWindow *w = window();
        if (w != nullptr) {
                auto [id, err] = w->application().startTimer(RepeatDelayMs, false, tick, this);
                _timer = err.isOk() ? id : -1;
                _repeating = false;
        }
        return;
}

void NumberField::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || _pressed == 0) return;
        mouseLeft();
        return;
}

void NumberField::mouseLeft() {
        if (_pressed == 0) return;
        _pressed = 0;
        stopRepeat();
        update();
        if (_model.value() != _pressValue && _changed != nullptr) _changed(_changedArg, *this, _model.value());
        return;
}

void NumberField::stopRepeat() {
        GuiWindow *w = _timer >= 0 ? window() : nullptr;
        if (w != nullptr) w->application().stopTimer(_timer);
        _timer = -1;
        _repeating = false;
        return;
}

void NumberField::tick(void *arg) {
        auto *self = static_cast<NumberField *>(arg);
        if (self->_pressed == 0) return;
        self->stepBy(self->_pressed, false, false);
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

bool NumberField::keyBeforeChild(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release || event.hasCtrl() || event.hasAlt()) return false;
        switch (event.usage) {
                case HidKey::Up: stepBy(1, false, true); return true;
                case HidKey::Down: stepBy(-1, false, true); return true;
                case HidKey::PageUp: stepBy(1, true, true); return true;
                case HidKey::PageDown: stepBy(-1, true, true); return true;
                default: return false;
        }
}

bool NumberField::wheelTurned(int notches) {
        if (_edit == nullptr || !_edit->hasFocus()) return false; // a page scrolls past it
        stepBy(notches, false, true);
        return true;
}

BMDS_NAMESPACE_END
