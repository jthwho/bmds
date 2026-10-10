/**
 * @file      tooltip.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Tooltips (devplan/gui.md §4s).
 */

#include <cstring>
#include <bmds/font.h>
#include <bmds/guiapplication.h>
#include <bmds/tooltip.h>
#include <bmds/windowstack.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t Pad = 4;    // the text from the border, across
        constexpr uint16_t PadY = 2;   // and down
        constexpr uint16_t Below = 20; // under the pointer (its arrow's height)
        constexpr uint16_t Align = WindowStack::Align;

        GuiWindow::Config toolTipConfig() {
                GuiWindow::Config c;
                c.title = "Tooltip";
                c.decorations = WindowFrame::None;
                c.level = WindowManager::Level::Tooltip;
                c.focusable = false;
                c.focusOnPress = false;
                c.kind = "tooltip";
                return c;
        }

        bool isInside(const Widget *widget, const Widget *container) {
                for (const Widget *w = widget; w != nullptr; w = w->parent())
                        if (w == container) return true;
                return false;
        }

} // namespace

ToolTip::ToolTip(GuiApplication &app) : GuiWindow(app, toolTipConfig()) {}

ToolTip::~ToolTip() {
        application().stopTimer(_timer);
}

void ToolTip::hover(Widget *widget, int screenX, int screenY) {
        if (widget == _target) return; // still on it (shown, waiting, or hidden for it)
        hide();
        _target = nullptr;
        _hidden = false;
        if (widget == nullptr || widget->toolTip().isEmpty()) return;
        _target = widget;
        _x = screenX;
        _y = screenY;
        const Result<int> timer = application().startTimer(DelayMs, false, due, this);
        _timer = timer.isOk() ? timer.value() : -1;
        return;
}

void ToolTip::hide() {
        application().stopTimer(_timer);
        _timer = -1;
        if (isShown()) close();
        if (_target != nullptr) _hidden = true; // until the pointer goes to another widget
        return;
}

void ToolTip::changed(Widget *widget) {
        if (widget == _target && isShown()) open();
        return;
}

void ToolTip::forget(Widget *widget) {
        if (_target == nullptr || !isInside(_target, widget)) return;
        hide();
        _target = nullptr;
        _hidden = false;
        return;
}

void ToolTip::due(void *arg) {
        auto *self = static_cast<ToolTip *>(arg);
        self->_timer = -1;
        if (self->_target != nullptr && !self->_hidden) self->open();
        return;
}

// Its text from the widget, in a window fitted to it below the pointer (above it near the
// screen's bottom), on the window manager's columns; a line at each \n.
void ToolTip::open() {
        const StringView text = _target->toolTip();
        size_t           n = text.size() < MaxText ? text.size() : MaxText;
        while (n > 0 && n < text.size() && (static_cast<uint8_t>(text[n]) & 0xc0u) == 0x80u) --n;
        std::memcpy(_text, text.data(), n);
        _text[n] = '\0';
        if (n == 0) {
                if (isShown()) close();
                return;
        }
        const Rect screen = application().windowManager().screen();
        uint32_t   widest = 0;
        size_t     lines = 0;
        StringView rest(_text);
        while (!rest.isEmpty() && lines < MaxLines) {
                const StringView line = nextLine(rest);
                const uint32_t   lw = Fonts::unscii16.textWidth(line);
                widest = lw > widest ? lw : widest;
                ++lines;
        }
        uint32_t w = widest + 2 * Pad + 2;
        w = (w + Align - 1) / Align * Align;
        const uint16_t h = uint16_t(lines * GuiStyle::TextHeight + 2 * PadY + 2);
        if (w > screen.width) w = screen.width;
        int x = _x;
        int y = _y + Below;
        if (y + h > screen.bottom()) y = _y - h - 4;
        if (x + int(w) > screen.right()) x = screen.right() - int(w);
        if (x < screen.x) x = screen.x;
        if (y < screen.y) y = screen.y;
        x -= x % Align;
        const Rect r{uint16_t(x), uint16_t(y), uint16_t(w), h};
        if (isShown() && geometry() == r) {
                update(); // as big: drawn again where it is (a live tooltip, without a blink)
                return;
        }
        if (isShown()) close(); // its size changed
        (void)show(r);
        update();
        return;
}

StringView ToolTip::nextLine(StringView &rest) {
        size_t end = 0;
        while (end < rest.size() && rest[end] != '\n') ++end;
        const StringView line = rest.left(end);
        rest = end < rest.size() ? rest.mid(end + 1) : StringView();
        return line;
}

void ToolTip::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        const uint32_t       fg = p.color(Role::ToolTipText);
        const uint32_t       bg = p.color(Role::ToolTip);
        const Rect           border[] = {{g.x, g.y, g.width, 1},
                                         {g.x, uint16_t(g.bottom() - 1), g.width, 1},
                                         {g.x, uint16_t(g.y + 1), 1, uint16_t(g.height - 2)},
                                         {uint16_t(g.right() - 1), uint16_t(g.y + 1), 1, uint16_t(g.height - 2)}};
        (void)painter.fill(Span<const Rect>(border), fg);
        (void)painter.fill(Rect{uint16_t(g.x + 1), uint16_t(g.y + 1), uint16_t(g.width - 2), uint16_t(g.height - 2)},
                           bg);
        StringView rest(_text);
        int        y = g.y + 1 + PadY;
        for (size_t i = 0; i < MaxLines && !rest.isEmpty(); ++i, y += GuiStyle::TextHeight)
                (void)painter.text(g.x + Pad, y, nextLine(rest), Fonts::unscii16, fg, bg, uint16_t(g.width - 2 - Pad));
        return;
}

BMDS_NAMESPACE_END
