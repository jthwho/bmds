/**
 * @file      slider.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A slider (devplan/gui.md §4s).
 */

#include <bmds/bevel.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/slider.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t GrooveBreadth = 4;
        constexpr uint16_t DefaultLength = 100;

} // namespace

Slider::Slider(Widget *parent, Orientation orientation) : Widget(parent), _orientation(orientation) {
        _model.setInverted(orientation == Orientation::Vertical);
        setFocusable(true);
}

void Slider::setOrientation(Orientation orientation) {
        if (orientation == _orientation) return;
        _orientation = orientation;
        _model.setInverted(orientation == Orientation::Vertical); // up is more
        update();
        return;
}

void Slider::setRange(int32_t minimum, int32_t maximum) {
        _model.setRange(minimum, maximum);
        update();
        return;
}

void Slider::setValue(int64_t value) {
        if (_model.setValue(value)) update();
        return;
}

void Slider::userSet(int64_t value) {
        if (!_model.setValue(value)) return;
        update();
        if (_changed != nullptr) _changed(_changedArg, *this, _model.value());
        return;
}

GuiSize Slider::sizeHint() const {
        return _orientation == Orientation::Vertical ? GuiSize{Breadth, DefaultLength}
                                                     : GuiSize{DefaultLength, Breadth};
}

uint16_t Slider::length() const {
        return _orientation == Orientation::Vertical ? geometry().height : geometry().width;
}

int Slider::along(const WindowManager::MouseEvent &e) const {
        return _orientation == Orientation::Vertical ? e.y - geometry().y : e.x - geometry().x;
}

Rect Slider::handleRect() const {
        const Rect    &g = geometry();
        const uint16_t at = _model.handleStart(length(), HandleLength);
        if (_orientation == Orientation::Vertical) return Rect{g.x, uint16_t(g.y + at), g.width, HandleLength};
        return Rect{uint16_t(g.x + at), g.y, HandleLength, g.height};
}

void Slider::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        (void)painter.fill(g, p.color(Role::Face));
        // The groove: a sunken line down the middle, the handle's half-length in from each end.
        const uint16_t inset = HandleLength / 2;
        Rect           groove;
        if (_orientation == Orientation::Vertical) {
                groove = Rect{uint16_t(g.x + (g.width - GrooveBreadth) / 2), uint16_t(g.y + inset), GrooveBreadth,
                              uint16_t(g.height > 2 * inset ? g.height - 2 * inset : 0)};
        } else {
                groove = Rect{uint16_t(g.x + inset), uint16_t(g.y + (g.height - GrooveBreadth) / 2),
                              uint16_t(g.width > 2 * inset ? g.width - 2 * inset : 0), GrooveBreadth};
        }
        if (!groove.isEmpty()) drawBevel(painter, groove, Bevel::Style::Sunken);
        const Rect handle = handleRect();
        drawBevel(painter, handle, Bevel::Style::Raised);
        const Rect face = Bevel::inside(handle, Bevel::Style::Raised);
        const bool focused = hasFocus() && window() != nullptr && window()->isActive();
        (void)painter.fill(face, p.color(focused ? Role::Highlight : Role::Face));
        return;
}

void Slider::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        const int  at = along(event);
        const Rect h = handleRect();
        const int  start = _orientation == Orientation::Vertical ? h.y - geometry().y : h.x - geometry().x;
        _dragging = true; // a press on the groove too: told to `Released` when let go
        if (at >= start && at < start + HandleLength) {
                _grab = at - start; // taken where it was pressed
        } else {
                _grab = HandleLength / 2; // brought under the pointer
                userSet(_model.valueAt(at - _grab, length(), HandleLength));
        }
        return;
}

void Slider::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_dragging) return;
        userSet(_model.valueAt(along(event) - _grab, length(), HandleLength));
        return;
}

void Slider::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !_dragging) return;
        _dragging = false;
        if (_released != nullptr) _released(_releasedArg, *this, _model.value());
        return;
}

bool Slider::wheelTurned(int notches) {
        SliderModel m = _model;
        (void)m.step(notches); // up: toward the maximum
        userSet(m.value());
        return true;
}

bool Slider::keyPressed(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        SliderModel m = _model;
        switch (event.usage) {
                case HidKey::Up:
                case HidKey::Right: (void)m.step(1); break;
                case HidKey::Down:
                case HidKey::Left: (void)m.step(-1); break;
                case HidKey::PageUp: (void)m.stepPage(1); break;
                case HidKey::PageDown: (void)m.stepPage(-1); break;
                case HidKey::Home: (void)m.setValue(m.minimum()); break; // as Qt's
                case HidKey::End: (void)m.setValue(m.maximum()); break;
                default: return false;
        }
        userSet(m.value());
        return true;
}

BMDS_NAMESPACE_END
