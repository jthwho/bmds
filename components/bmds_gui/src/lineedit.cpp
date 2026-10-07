/**
 * @file      lineedit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A line of text to edit (devplan/gui.md §4q).
 */

#include <bmds/bevel.h>
#include <bmds/font.h>
#include <bmds/guiapplication.h>
#include <bmds/guiclipboard.h>
#include <bmds/guiwindow.h>
#include <bmds/lineedit.h>
#include <bmds/system.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t Pad = 4; // the text from the bevel
        constexpr uint16_t DefaultWidth = 160;
        constexpr uint8_t  A = HidKey::A;

        constexpr uint8_t letter(char ch) {
                return static_cast<uint8_t>(A + (ch - 'a'));
        }

        GuiClipboard *clipboardOf(const Widget &w) {
                GuiWindow *window = w.window();
                return window != nullptr ? window->application().clipboard() : nullptr;
        }

} // namespace

LineEdit::LineEdit(Widget *parent) : Widget(parent) {}

LineEdit::~LineEdit() {
        // A paste still coming finds no one.
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard != nullptr) clipboard->cancel(this);
}

void LineEdit::setText(StringView text) {
        _model.setText(text);
        _first = 0;
        changed();
        return;
}

void LineEdit::selectAll() {
        _model.selectAll();
        changed();
        return;
}

GuiSize LineEdit::sizeHint() const {
        return GuiSize{DefaultWidth, GuiStyle::ButtonHeight};
}

Rect LineEdit::textArea() const {
        const Rect     in = Bevel::inside(geometry(), Bevel::Style::Field);
        const uint16_t x = static_cast<uint16_t>((in.x + Pad + 3) & ~3u); // text on a 4-pixel column
        const uint16_t w = in.right() > x + Pad ? static_cast<uint16_t>(in.right() - x - Pad) : 0;
        return Rect{x, in.y, w, in.height};
}

void LineEdit::reveal() {
        const Font      &font = Fonts::unscii16;
        const StringView text = _model.text();
        const size_t     cursor = _model.cursor();
        const uint16_t   width = textArea().width;
        if (width == 0) return; // no size yet: when it gets one (resized())
        if (cursor < _first) _first = cursor;
        // On, a character at a time, until the cursor (and a pixel for it) fits.
        while (_first < cursor && font.textWidth(text.mid(_first, cursor - _first)) + 2 > width) {
                size_t at = _first;
                (void)Utf8::decode(text, at);
                _first = at;
        }
        return;
}

void LineEdit::resized() {
        _first = 0;
        reveal();
        return;
}

void LineEdit::changed() {
        reveal();
        update();
        return;
}

size_t LineEdit::offsetAt(int x) const {
        const Font      &font = Fonts::unscii16;
        const StringView text = _model.text();
        int              left = textArea().x;
        size_t           at = _first;
        while (at < text.size()) {
                size_t next = at;
                (void)Utf8::decode(text, next);
                const int cw = int(font.textWidth(text.mid(at, next - at)));
                if (x < left + cw / 2) return at;
                left += cw;
                at = next;
        }
        return text.size();
}

bool LineEdit::keyPressed(const KeyEvent &event) {
        const bool shift = event.hasShift();
        const bool ctrl = event.hasCtrl() && !event.hasAlt();
        switch (event.usage) {
                case HidKey::Left: _model.left(shift); break;
                case HidKey::Right: _model.right(shift); break;
                case HidKey::Home: _model.home(shift); break;
                case HidKey::End: _model.end(shift); break;
                case HidKey::Backspace: (void)_model.backspace(); break;
                case HidKey::Delete: (void)_model.erase(); break;
                case HidKey::Enter:
                case HidKey::KeypadEnter:
                        if (_accepted != nullptr) _accepted(_acceptedArg, *this);
                        return true; // this may be gone now
                case HidKey::Escape:
                        if (_cancelled != nullptr) _cancelled(_cancelledArg, *this);
                        return true;
                default:
                        if (ctrl && event.usage == letter('a')) {
                                _model.selectAll();
                        } else if (ctrl && (event.usage == letter('c') || event.usage == letter('x'))) {
                                GuiClipboard *clipboard = clipboardOf(*this);
                                if (clipboard != nullptr && _model.hasSelection())
                                        (void)clipboard->copyText(Clipboard::Selection::Clipboard, _model.selected());
                                if (event.usage == letter('x')) (void)_model.backspace();
                        } else if (ctrl && event.usage == letter('v')) {
                                GuiClipboard *clipboard = clipboardOf(*this);
                                if (clipboard != nullptr)
                                        (void)clipboard->paste(Clipboard::Selection::Clipboard, ClipboardType::Text,
                                                               pasted, this);
                                return true;
                        } else if (!ctrl && event.codepoint >= 0x20 && event.codepoint != 0x7f) {
                                char         utf8[4];
                                const size_t n = Utf8::encode(event.codepoint, utf8);
                                (void)_model.insert(StringView(utf8, n));
                        } else {
                                return false;
                        }
                        break;
        }
        changed();
        return true;
}

void LineEdit::pasted(void *arg, Error err, StringView type, StringView data) {
        (void)type;
        auto *self = static_cast<LineEdit *>(arg);
        if (err.isError()) return;
        if (self->_model.insert(data)) self->changed();
        return;
}

void LineEdit::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        const uint64_t now = System::uptimeUs();
        if (now - _pressUs <= uint64_t(WindowManager::DoubleClickMs) * 1000) {
                _pressUs = 0;
                _model.selectAll();
                _dragging = false;
        } else {
                _pressUs = now;
                _model.setCursor(offsetAt(event.x), (event.modifiers & KeyEvent::Shift) != 0);
                _dragging = true;
        }
        changed();
        return;
}

void LineEdit::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_dragging || (event.buttons & PointerEvent::Left) == 0) return;
        _model.setCursor(offsetAt(event.x), true);
        changed();
        return;
}

void LineEdit::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button == PointerEvent::Left) _dragging = false;
        return;
}

void LineEdit::paint(Painter &painter) {
        const WindowPalette &palette = painter.palette();
        const Rect           outer = geometry();
        const Rect           in = Bevel::inside(outer, Bevel::Style::Field);
        const uint32_t       bg = palette.color(Role::FaceHighlight); // a field's white
        const uint32_t       fg = palette.color(isEnabled() ? Role::FaceText : Role::DisabledText);
        const uint32_t       selBg = palette.color(Role::Highlight);
        const uint32_t       selFg = palette.color(Role::HighlightText);
        drawBevel(painter, outer, Bevel::Style::Field);
        (void)painter.fill(in, bg);
        const Font      &font = Fonts::unscii16;
        const Rect       area = textArea();
        const StringView text = _model.text();
        const uint16_t   y = static_cast<uint16_t>(
                in.y + (in.height > GuiStyle::TextHeight ? (in.height - GuiStyle::TextHeight) / 2 : 0));
        // Three runs: before the selection, it, after it (each from the first byte shown).
        const size_t from = _model.selectionStart() > _first ? _model.selectionStart() : _first;
        const size_t to = _model.selectionEnd() > _first ? _model.selectionEnd() : _first;
        uint32_t     x = area.x;
        const struct {
                        size_t   start, end;
                        uint32_t fg, bg;
        } runs[] = {{_first, from, fg, bg}, {from, to, selFg, selBg}, {to, text.size(), fg, bg}};
        for (const auto &run : runs) {
                if (run.end <= run.start || x >= area.right()) continue;
                const StringView piece = text.mid(run.start, run.end - run.start);
                const uint32_t   w = font.textWidth(piece);
                const uint16_t   room = static_cast<uint16_t>(area.right() - x);
                if (run.bg != bg)
                        (void)painter.fill(Rect{uint16_t(x), y, uint16_t(w < room ? w : room), GuiStyle::TextHeight},
                                           run.bg);
                (void)painter.text(uint16_t(x), y, piece, font, run.fg, run.bg, room);
                x += w;
        }
        // The cursor: a line before the character it is at.
        if (_model.cursor() >= _first) {
                const uint32_t cx = area.x + font.textWidth(text.mid(_first, _model.cursor() - _first));
                if (cx < area.right() + 1u)
                        (void)painter.fill(Rect{uint16_t(cx > 0 ? cx - 1 : 0), y, 1, GuiStyle::TextHeight}, fg);
        }
        return;
}

BMDS_NAMESPACE_END
