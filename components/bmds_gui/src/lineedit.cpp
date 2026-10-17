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

        constexpr uint16_t DefaultWidth = 160;
        constexpr uint8_t  A = HidKey::A;

        constexpr uint8_t letter(char ch) {
                return static_cast<uint8_t>(A + (ch - 'a'));
        }

        // A masked field's text as drawn: a star a byte (so offsets and widths match the text's).
        constexpr size_t Stars = LineEditModel::MaxBytes;
        constexpr auto   starText = [] {
                struct Text {
                                char c[Stars + 1];
                };
                Text t{};
                for (size_t i = 0; i < Stars; ++i) t.c[i] = '*';
                return t;
        }();

        GuiClipboard *clipboardOf(const Widget &w) {
                GuiWindow *window = w.window();
                return window != nullptr ? window->application().clipboard() : nullptr;
        }

} // namespace

LineEdit::LineEdit(Widget *parent) : Widget(parent) {
        setFocusable(true); // a click or Tab gives it the keys (gui.md §4r)
        setAcceptsDrops(true);
}

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

void LineEdit::setSelection(size_t start, size_t end) {
        _model.setCursor(start, false);
        _model.setCursor(end, true);
        changed();
        return;
}

void LineEdit::setFramed(bool framed) {
        _framed = framed;
        changed();
        return;
}

void LineEdit::setColors(Role background, Role text) {
        _background = background;
        _foreground = text;
        update();
        return;
}

void LineEdit::setCentered(bool centered) {
        _centered = centered;
        changed();
        return;
}

void LineEdit::setWrapping(bool wrapping) {
        _wrapping = wrapping;
        _first = 0;
        changed();
        return;
}

GuiSize LineEdit::sizeHint() const {
        return GuiSize{DefaultWidth, GuiStyle::ButtonHeight};
}

Rect LineEdit::textArea() const {
        const Rect     in = _framed ? Bevel::inside(geometry(), Bevel::Style::Field) : geometry();
        const uint16_t x = static_cast<uint16_t>((in.x + Pad + 3) & ~3u); // text on a 4-pixel column
        const uint16_t w = in.right() > x + Pad ? static_cast<uint16_t>(in.right() - x - Pad) : 0;
        return Rect{x, in.y, w, in.height};
}

// The lines shown: wrapped as `TextWrap` breaks them, else the one from the first byte shown.
size_t LineEdit::lines(Line *out) const {
        const Font      &font = Fonts::unscii16;
        const StringView text = drawn();
        const Rect       area = textArea();
        const auto       place = [&](size_t start, size_t end) {
                const uint32_t w = font.textWidth(text.mid(start, end - start));
                const uint16_t x = _centered && w < area.width
                                                 ? static_cast<uint16_t>(area.x + (((area.width - w) / 2) & ~3u))
                                                 : area.x;
                return Line{start, end, x};
        };
        if (!_wrapping || area.width < font.width()) {
                out[0] = _first == 0 ? place(0, text.size()) : Line{_first, text.size(), area.x};
                return 1;
        }
        StringView   views[MaxLines];
        const size_t n = TextWrap::wrap(text, static_cast<uint16_t>(area.width / font.width()),
                                        Span<StringView>(views, MaxLines));
        for (size_t i = 0; i < n; ++i) {
                const size_t start = static_cast<size_t>(views[i].data() - text.data());
                out[i] = place(start, start + views[i].size());
        }
        if (n == 0) out[0] = place(0, 0); // nothing but spaces, or nothing
        return n > 0 ? n : 1;
}

size_t LineEdit::lineCount() const {
        Line ls[MaxLines];
        return lines(ls);
}

// The line @p offset is on: the last starting at or before it (a space dropped at a break is
// the line before's).
size_t LineEdit::lineOf(const Line *ls, size_t count, size_t offset) const {
        size_t i = 0;
        while (i + 1 < count && ls[i + 1].start <= offset) ++i;
        return i;
}

void LineEdit::reveal() {
        const Font      &font = Fonts::unscii16;
        const StringView text = drawn();
        const size_t     cursor = _model.cursor();
        const uint16_t   width = textArea().width;
        if (_wrapping) {
                _first = 0; // it grows instead
                return;
        }
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
        changed();
        return;
}

void LineEdit::changed() {
        reveal();
        update();
        if (_wrapping) {
                const size_t n = lineCount();
                if (n != _lineCount) {
                        _lineCount = n;
                        if (_linesChanged != nullptr) _linesChanged(_linesChangedArg, *this, n);
                }
        }
        return;
}

size_t LineEdit::offsetAt(const Line &line, int x) const {
        const Font      &font = Fonts::unscii16;
        const StringView text = drawn();
        int              left = line.x;
        size_t           at = line.start;
        while (at < line.end) {
                size_t next = at;
                (void)Utf8::decode(text, next);
                const int cw = int(font.textWidth(text.mid(at, next - at)));
                if (x < left + cw / 2) return at;
                left += cw;
                at = next;
        }
        return line.end;
}

size_t LineEdit::offsetAt(int x, int y) const {
        Line         ls[MaxLines];
        const size_t n = lines(ls);
        const int    top = textArea().y;
        size_t       i = 0;
        if (_wrapping && y > top) i = static_cast<size_t>((y - top) / GuiStyle::TextHeight);
        return offsetAt(ls[i < n ? i : n - 1], x);
}

// Up or down a line, to the character nearest the cursor's column; past the first or the last
// line, to the text's start or end.
void LineEdit::moveLine(int by, bool select) {
        const Font  &font = Fonts::unscii16;
        Line         ls[MaxLines];
        const size_t n = lines(ls);
        const size_t cursor = _model.cursor();
        const size_t at = lineOf(ls, n, cursor);
        const int    to = int(at) + by;
        if (to < 0) {
                _model.home(select);
                return;
        }
        if (to >= int(n)) {
                _model.end(select);
                return;
        }
        const Line &line = ls[at];
        const int   x = int(line.x + font.textWidth(drawn().mid(line.start, cursor - line.start)));
        _model.setCursor(offsetAt(ls[to], x), select);
        return;
}

bool LineEdit::keyPressed(const KeyEvent &event) {
        const bool shift = event.hasShift();
        const bool ctrl = event.hasCtrl() && !event.hasAlt();
        bool       edited = false; // the text, not only the cursor or the selection
        switch (event.usage) {
                case HidKey::Left: _model.left(shift); break;
                case HidKey::Right: _model.right(shift); break;
                case HidKey::Home: _model.home(shift); break;
                case HidKey::End: _model.end(shift); break;
                case HidKey::Up:
                case HidKey::Down:
                        if (!_wrapping) return false;
                        moveLine(event.usage == HidKey::Up ? -1 : 1, shift);
                        break;
                case HidKey::Backspace: edited = _model.backspace(); break;
                case HidKey::Delete: edited = _model.erase(); break;
                case HidKey::Enter:
                case HidKey::KeypadEnter:
                        // Without its own use, the window's (a dialog's default button).
                        if (_accepted == nullptr) return false;
                        _accepted(_acceptedArg, *this);
                        return true; // this may be gone now
                case HidKey::Escape:
                        if (_cancelled == nullptr) return false; // a dialog's cancel
                        _cancelled(_cancelledArg, *this);
                        return true;
                default:
                        if (ctrl && event.usage == letter('a')) {
                                _model.selectAll();
                        } else if (ctrl && _masked && (event.usage == letter('c') || event.usage == letter('x'))) {
                                return true; // a masked field's text isn't copied
                        } else if (ctrl && (event.usage == letter('c') || event.usage == letter('x'))) {
                                GuiClipboard *clipboard = clipboardOf(*this);
                                if (clipboard != nullptr && _model.hasSelection())
                                        (void)clipboard->copyText(Clipboard::Selection::Clipboard, _model.selected());
                                if (event.usage == letter('x')) edited = _model.backspace();
                        } else if (ctrl && event.usage == letter('v')) {
                                GuiClipboard *clipboard = clipboardOf(*this);
                                if (clipboard != nullptr)
                                        (void)clipboard->paste(Clipboard::Selection::Clipboard, ClipboardType::Text,
                                                               pasted, this);
                                return true;
                        } else if (!ctrl && event.codepoint >= 0x20 && event.codepoint != 0x7f) {
                                char         utf8[4];
                                const size_t n = Utf8::encode(event.codepoint, utf8);
                                edited = _model.insert(StringView(utf8, n));
                        } else {
                                return false;
                        }
                        break;
        }
        changed();
        if (edited && _edited != nullptr) _edited(_editedArg, *this); // last: it may destroy this
        return true;
}

void LineEdit::pasted(void *arg, Error err, StringView type, StringView data) {
        (void)type;
        auto *self = static_cast<LineEdit *>(arg);
        if (err.isError()) return;
        if (!self->_model.insert(data)) return;
        self->changed();
        if (self->_edited != nullptr) self->_edited(self->_editedArg, *self);
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
                _model.setCursor(offsetAt(event.x, event.y), (event.modifiers & KeyEvent::Shift) != 0);
                _dragging = true;
        }
        changed();
        return;
}

void LineEdit::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_dragging || (event.buttons & PointerEvent::Left) == 0) return;
        _model.setCursor(offsetAt(event.x, event.y), true);
        changed();
        return;
}

void LineEdit::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button == PointerEvent::Left) _dragging = false;
        return;
}

// Bytes [@p from, @p to) of @p line at @p x (moved on past them), in @p fg on @p bg, cut at the
// text area's right.
void LineEdit::paintRun(Painter &painter, const Line &line, uint16_t y, size_t from, size_t to, uint32_t &x,
                        uint32_t fg, uint32_t bg) const {
        (void)line;
        const Rect area = textArea();
        if (to <= from || x >= area.right()) return;
        const Font      &font = Fonts::unscii16;
        const StringView piece = drawn().mid(from, to - from);
        const uint32_t   w = font.textWidth(piece);
        const uint16_t   room = static_cast<uint16_t>(area.right() - x);
        (void)painter.fill(Rect{uint16_t(x), y, uint16_t(w < room ? w : room), GuiStyle::TextHeight}, bg);
        (void)painter.text(uint16_t(x), y, piece, font, fg, bg, room);
        x += w;
        return;
}

void LineEdit::paint(Painter &painter) {
        const WindowPalette &palette = painter.palette();
        const Rect           outer = geometry();
        const Rect           in = _framed ? Bevel::inside(outer, Bevel::Style::Field) : outer;
        const uint32_t       bg = palette.color(_background);
        const uint32_t       fg = palette.color(isEnabled() ? _foreground : Role::DisabledText);
        const uint32_t       selBg = palette.color(Role::Highlight);
        const uint32_t       selFg = palette.color(Role::HighlightText);
        if (_framed) drawBevel(painter, outer, Bevel::Style::Field);
        (void)painter.fill(in, bg);
        const Font      &font = Fonts::unscii16;
        const Rect       area = textArea();
        const StringView text = drawn();
        Line             ls[MaxLines];
        const size_t     n = lines(ls);
        const size_t     selStart = _model.selectionStart(), selEnd = _model.selectionEnd();
        const auto       top = [&](size_t i) {
                if (_wrapping) return static_cast<uint16_t>(area.y + i * GuiStyle::TextHeight);
                return static_cast<uint16_t>(
                        in.y + (in.height > GuiStyle::TextHeight ? (in.height - GuiStyle::TextHeight) / 2 : 0));
        };
        for (size_t i = 0; i < n; ++i) {
                const Line    &line = ls[i];
                const uint16_t y = top(i);
                if (y >= in.bottom()) break;
                // Three runs: before the selection, it, after it (each inside the line).
                const auto clamp = [&](size_t v) {
                        return v < line.start ? line.start : (v > line.end ? line.end : v);
                };
                const size_t from = clamp(selStart), to = clamp(selEnd);
                uint32_t     x = line.x;
                paintRun(painter, line, y, line.start, from, x, fg, bg);
                const uint32_t selX = x;
                paintRun(painter, line, y, from, to, x, selFg, selBg);
                if (!_framed && to > from) {
                        // Unframed, a line selected to an end is lit a little past it, as a selected label is.
                        if (from == line.start && selX >= area.x)
                                (void)painter.fill(Rect{uint16_t(selX - Pad), y, Pad, GuiStyle::TextHeight}, selBg);
                        if (to == line.end && x + Pad <= in.right())
                                (void)painter.fill(Rect{uint16_t(x), y, Pad, GuiStyle::TextHeight}, selBg);
                }
                paintRun(painter, line, y, to, line.end, x, fg, bg);
        }
        // The cursor: a line before the character it is at, while it has the keys.
        const GuiWindow *window = this->window();
        if (!hasFocus() || window == nullptr || !window->isActive()) return;
        const size_t cursor = _model.cursor();
        const size_t at = lineOf(ls, n, cursor);
        const Line  &line = ls[at];
        if (cursor >= line.start) {
                const uint32_t cx = line.x + font.textWidth(text.mid(line.start, cursor - line.start));
                const uint16_t y = top(at);
                if (cx < area.right() + 1u && y < in.bottom())
                        (void)painter.fill(Rect{uint16_t(cx > 0 ? cx - 1 : 0), y, 1, GuiStyle::TextHeight}, fg);
        }
        return;
}

StringView LineEdit::drawn() const {
        return _masked ? StringView(starText.c, _model.text().size()) : _model.text();
}

void LineEdit::setMasked(bool masked) {
        _masked = masked;
        setAcceptsDrops(!masked); // nothing dropped into a password
        changed();
        return;
}

// ---- Drops (devplan/clipboard.md §5d): text, or files as their paths, replace the text.

void LineEdit::dragMoved(GuiDropEvent &event) {
        const bool text = event.hasType(ClipboardType::Text);
        event.accept(text && DropActions::allows(event.actions(), DropAction::Copy) ? DropAction::Copy
                                                                                    : DropAction::None);
        return;
}

bool LineEdit::dropped(GuiDropEvent &event) {
        if (!event.hasType(ClipboardType::Text) || !DropActions::allows(event.actions(), DropAction::Copy))
                return false;
        // Text: a URI list comes as its paths, a line each (the clipboard's converter).
        if (event.read(ClipboardType::Text, dropRead, this).isError()) return false;
        _dropDrag = event.drag();
        return true;
}

void LineEdit::dropRead(void *arg, Error err, StringView type, StringView data) {
        (void)type;
        auto          *self = static_cast<LineEdit *>(arg);
        GuiWindow     *w = self->window();
        const uint32_t drag = self->_dropDrag;
        self->_dropDrag = 0;
        if (err.isOk()) {
                // One line: line ends as spaces, none at the end.
                char   buffer[LineEditModel::MaxBytes + 1];
                size_t n = 0;
                for (size_t i = 0; i < data.size() && n < LineEditModel::MaxBytes; ++i)
                        buffer[n++] = data[i] == '\n' || data[i] == '\r' ? ' ' : data[i];
                while (n > 0 && buffer[n - 1] == ' ') --n;
                self->setText(StringView(buffer, n));
        }
        if (w != nullptr)
                (void)w->application().windowManager().finishDrop(drag,
                                                                  err.isOk() ? DropAction::Copy : DropAction::None);
        if (err.isOk() && self->_edited != nullptr) self->_edited(self->_editedArg, *self); // last: it may destroy this
        return;
}

void LineEdit::focusOut() {
        if (_focusLost != nullptr) _focusLost(_focusLostArg, *this); // last: it may destroy this
        return;
}

BMDS_NAMESPACE_END
