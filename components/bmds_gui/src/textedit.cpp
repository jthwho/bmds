/**
 * @file      textedit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A multi-line text editor (devplan/editor.md §4).
 */

#include <bmds/textedit.h>

#include <new>
#include <bmds/bitmapfont.h>
#include <bmds/glyphrenderer.h>
#include <bmds/guiapplication.h>
#include <bmds/guiclipboard.h>
#include <bmds/guiwindow.h>
#include <bmds/memory.h>
#include <bmds/system.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;
        using Kind = TextDocument::EditKind;

        constexpr uint16_t CellWidth = 8; // unscii-16's
        constexpr uint16_t LineHeight = 16; // unscii-16's: the editors draw in its cells
        constexpr uint16_t DefaultColumns = 80;
        constexpr uint16_t DefaultRows = 25;
        constexpr uint32_t EdgeMs = 50;     // a drag past an edge scrolls this often
        constexpr uint8_t  SlashKey = 0x38; // HID's `/`

        constexpr uint8_t letter(char ch) {
                return static_cast<uint8_t>(HidKey::A + (ch - 'a'));
        }

        GuiClipboard *clipboardOf(const Widget &w) {
                GuiWindow *window = w.window();
                return window != nullptr ? window->application().clipboard() : nullptr;
        }

        uint32_t nowMs() {
                return static_cast<uint32_t>(System::uptimeUs() / 1000);
        }

        // A control character's picture (U+2400 and on), so it shows.
        uint32_t shown(uint32_t cp) {
                if (cp < 0x20) return 0x2400 + cp;
                if (cp == 0x7f) return 0x2421;
                return cp;
        }

} // namespace

TextEdit::TextEdit(Widget *parent) : ScrollArea(parent) {
        (void)_document->addObserver(observe, this);
        setFocusable(true);
        setAcceptsDrops(true);
        setLineSteps(CellWidth, LineHeight);
        measureAll();
}

TextEdit::~TextEdit() {
        stopBlink();
        if (_edgeTimer >= 0 && window() != nullptr) window()->application().stopTimer(_edgeTimer);
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard != nullptr) clipboard->cancel(this);
        _document->removeObserver(observe, this);
        Memory::free(_cells);
}

// ---- The document

void TextEdit::setDocument(TextDocument *document) {
        if (document == nullptr) document = &_own;
        if (document == _document) return;
        _document->removeObserver(observe, this);
        _document = document;
        (void)_document->addObserver(observe, this);
        _cursor = TextCursor(*_document);
        measureAll();
        scrollTo(0, 0);
        cursorChanged();
        update();
        return;
}

Error TextEdit::setText(StringView bytes) {
        const Error err = _document->load(bytes);
        _cursor.setPosition(TextPosition{});
        measureAll();
        scrollTo(0, 0);
        cursorChanged();
        update();
        return err;
}

void TextEdit::observe(void *arg, const TextDocument::Change &change) {
        auto *self = static_cast<TextEdit *>(arg);
        // The widest line grows with what came in (it shrinks only when measured again).
        for (uint32_t i = 0; i < change.insertedLines; ++i) {
                const uint32_t line = change.firstLine + i;
                const uint32_t w = self->_document->columnOf(line, UINT32_MAX, self->tabWidth());
                if (w > self->_widest) self->_widest = w;
        }
        self->_cursor.clamp();
        self->updateContentSize();
        if (change.removedLines == 1 && change.insertedLines == 1)
                self->updateLine(change.firstLine);
        else
                self->update();
        self->documentChanged(change);
        self->_changedInEdit = true;
        if (self->_changed != nullptr) self->_changed(self->_changedArg, *self);
        return;
}

// ---- Geometry

Rect TextEdit::textArea() const {
        Rect           v = viewport();
        const uint16_t g = gutterWidth();
        if (g >= v.width) return Rect{uint16_t(v.right()), v.y, 0, v.height};
        return Rect{static_cast<uint16_t>(v.x + g), v.y, static_cast<uint16_t>(v.width - g), v.height};
}

// Column 0's x: past the margin, on a 4-pixel column (where text must start), less the scroll
// (4-pixel steps, so every column stays on one).
int TextEdit::textLeft() const {
        return int((textArea().x + Margin + 3) & ~3u) - int(scrollX());
}

void TextEdit::measureAll() {
        _widest = 0;
        for (uint32_t i = 0; i < _document->lineCount(); ++i) {
                const uint32_t w = _document->columnOf(i, UINT32_MAX, tabWidth());
                if (w > _widest) _widest = w;
        }
        updateContentSize();
        return;
}

void TextEdit::updateContentSize() {
        // The gutter is outside the scrolled part: its width is added so the text's end shows.
        const uint32_t width = gutterWidth() + Margin + (_widest + 2) * CellWidth;
        const uint32_t height = _document->lineCount() * uint32_t(LineHeight);
        if (width != contentWidth() || height != contentHeight()) setContentSize(width, height);
        return;
}

TextPosition TextEdit::positionAt(int x, int y) const {
        const Rect v = viewport();
        int64_t    row = (int64_t(y) - v.y + int64_t(scrollY())) / LineHeight;
        if (int64_t(y) < v.y) row = (int64_t(y) - v.y + int64_t(scrollY()) - (LineHeight - 1)) / LineHeight;
        if (row < 0) return TextPosition{};
        if (row >= int64_t(_document->lineCount())) return _document->end();
        const int      dx = x - textLeft();
        const uint32_t column = dx <= 0 ? 0 : uint32_t((dx + CellWidth / 2) / CellWidth);
        const auto     line = static_cast<uint32_t>(row);
        // byteAtColumn rounds to the nearer edge of the character the column is in; the column
        // here is already a boundary, so the character starting there (or ending) is right.
        return TextPosition{line, _document->byteAtColumn(line, column, tabWidth())};
}

GuiSize TextEdit::sizeHint() const {
        return GuiSize{static_cast<uint16_t>(DefaultColumns * CellWidth + 2 * Margin + 16),
                       static_cast<uint16_t>(DefaultRows * LineHeight)};
}

void TextEdit::resized() {
        ScrollArea::resized();
        updateContentSize();
        return;
}

void TextEdit::scrolled() {
        return;
}

WindowManager::Pointer TextEdit::pointerAt(int x, int y) const {
        const Rect a = textArea();
        if (x >= int(a.x) && x < int(a.right()) && y >= int(a.y) && y < int(a.bottom()))
                return WindowManager::Pointer::Text;
        return WindowManager::Pointer::Arrow;
}

void TextEdit::updateLine(uint32_t line) {
        GuiWindow *w = window();
        if (w == nullptr) return;
        const Rect v = viewport();
        const int  y = windowY(line * uint32_t(LineHeight));
        if (y + LineHeight <= v.y || y >= v.bottom()) return;
        const int top = y < v.y ? v.y : y;
        const int bottom = y + LineHeight > v.bottom() ? v.bottom() : y + LineHeight;
        w->invalidate(Rect{v.x, uint16_t(top), v.width, uint16_t(bottom - top)});
        return;
}

void TextEdit::relayoutContent() {
        updateContentSize();
        update();
        return;
}

int TextEdit::columnX(uint32_t column) const {
        return textLeft() + int(column * CellWidth);
}

uint16_t TextEdit::cellWidth() {
        return CellWidth;
}

void TextEdit::updateGutter() {
        GuiWindow *w = window();
        const Rect v = viewport();
        if (w != nullptr && gutterWidth() != 0) w->invalidate(Rect{v.x, v.y, gutterWidth(), v.height});
        return;
}

// ---- The cursor

void TextEdit::setSelection(const TextSelection &selection) {
        _cursor.setSelection(selection);
        _document->breakMerge();
        cursorChanged();
        return;
}

void TextEdit::goToLine(uint32_t line) {
        if (line >= _document->lineCount()) line = _document->lineCount() - 1;
        _cursor.setPosition(TextPosition{line, 0});
        _document->breakMerge();
        // Near the middle, unless it shows already.
        const Rect     v = viewport();
        const uint32_t y = line * uint32_t(LineHeight);
        if (y < scrollY() || y + LineHeight > scrollY() + v.height)
                scrollTo(scrollX(), int64_t(y) - int64_t(v.height / 2));
        cursorChanged();
        return;
}

Error TextEdit::selectedText(String &out) const {
        return _document->text(_cursor.range(), out);
}

void TextEdit::revealCursor() {
        const TextPosition p = _cursor.position();
        const uint32_t     column = _document->columnOf(p.line, p.byte, tabWidth());
        const uint32_t     x = column * CellWidth;
        const Rect         area = textArea();
        uint32_t           sx = scrollX();
        if (x < sx) sx = x > CellWidth * 4 ? x - CellWidth * 4 : 0;
        if (x + CellWidth + Margin * 2 > sx + area.width)
                sx = x + CellWidth * 4 + Margin * 2 > area.width ? x + CellWidth * 4 + Margin * 2 - area.width : 0;
        const uint32_t y = p.line * uint32_t(LineHeight);
        uint32_t       sy = scrollY();
        if (y < sy) sy = y;
        if (y + LineHeight > sy + area.height) sy = y + LineHeight > area.height ? y + LineHeight - area.height : 0;
        if (sx != scrollX() || sy != scrollY()) scrollTo(sx, sy);
        return;
}

Rect TextEdit::cursorRect() const {
        const TextPosition p = _cursor.position();
        const int          x = columnX(_document->columnOf(p.line, p.byte, tabWidth()));
        const int          y = windowY(p.line * uint32_t(LineHeight));
        return Rect{uint16_t(x < 0 ? 0 : x), uint16_t(y < 0 ? 0 : y), CellWidth, LineHeight};
}

void TextEdit::cursorChanged() {
        revealCursor();
        _cursorOn = true;
        startBlink();
        update(); // the cursor, the selection and the current line
        offerPrimary();
        cursorMovedHook();
        if (_cursorMoved != nullptr) _cursorMoved(_cursorMovedArg, *this);
        return;
}

// ---- Settings

void TextEdit::setTabWidth(uint32_t width) {
        _cursor.setTabWidth(width);
        measureAll();
        update();
        return;
}

void TextEdit::setReadOnly(bool readOnly) {
        _readOnly = readOnly;
        return;
}

void TextEdit::setOverwrite(bool overwrite) {
        _overwrite = overwrite;
        updateLine(_cursor.position().line);
        if (_cursorMoved != nullptr) _cursorMoved(_cursorMovedArg, *this);
        return;
}

void TextEdit::setColors(const Colors &colors) {
        _colors = colors;
        _paletteColors = false;
        update();
        return;
}

uint32_t TextEdit::lineBackground(uint32_t line) const {
        if (!_paletteColors && line == _cursor.position().line && !_cursor.hasSelection()) return _colors.currentLine;
        return _colors.background;
}

// ---- Editing

void TextEdit::beginEdit(Kind kind, StringView typed) {
        _document->beginEdit(_cursor.selection(), kind, nowMs(), typed);
        return;
}

void TextEdit::endEdit() {
        _document->endEdit(_cursor.selection());
        return;
}

void TextEdit::replaceSelection(StringView text, Kind kind) {
        if (_readOnly) return;
        beginEdit(kind, text);
        auto r = _document->replace(_cursor.range(), text);
        _cursor.setPosition(r.value());
        endEdit();
        cursorChanged();
        return;
}

void TextEdit::typeCharacter(StringView text) {
        if (_readOnly) return;
        beginEdit(Kind::Typing, text);
        TextRange range = _cursor.range();
        // Overwrite takes the character at the cursor (not a line's end).
        if (_overwrite && range.isEmpty() && _document->byteAt(range.start) != '\n' && range.start != _document->end())
                range.end = _document->nextCharacter(range.start);
        auto r = _document->replace(range, text);
        _cursor.setPosition(r.value());
        endEdit();
        cursorChanged();
        typed(text);
        return;
}

void TextEdit::backspace(bool word) {
        if (_readOnly) return;
        if (_cursor.hasSelection()) {
                replaceSelection(StringView(), Kind::Delete);
                return;
        }
        const TextPosition p = _cursor.position();
        const TextPosition from = word ? _document->previousWord(p) : _document->previousCharacter(p);
        if (from == p) return;
        beginEdit(word ? Kind::Other : Kind::Delete);
        (void)_document->remove(TextRange{from, p});
        _cursor.setPosition(from);
        endEdit();
        cursorChanged();
        return;
}

void TextEdit::deleteForward(bool word) {
        if (_readOnly) return;
        if (_cursor.hasSelection()) {
                replaceSelection(StringView(), Kind::Delete);
                return;
        }
        const TextPosition p = _cursor.position();
        const TextPosition to = word ? _document->nextWord(p) : _document->nextCharacter(p);
        if (to == p) return;
        beginEdit(word ? Kind::Other : Kind::Delete);
        (void)_document->remove(TextRange{p, to});
        _cursor.setPosition(p);
        endEdit();
        cursorChanged();
        return;
}

void TextEdit::newline() {
        if (_readOnly) return;
        char   buffer[256];
        size_t n = 0;
        buffer[n++] = '\n';
        if (_autoIndent) {
                const StringView line = _document->line(_cursor.range().start.line);
                for (size_t i = 0; i < line.size() && n < sizeof(buffer) && (line[i] == ' ' || line[i] == '\t'); ++i)
                        buffer[n++] = line[i];
        }
        replaceSelection(StringView(buffer, n), Kind::Other);
        return;
}

void TextEdit::tab(bool out) {
        if (_readOnly) return;
        const TextRange range = _cursor.range();
        if (out || range.start.line != range.end.line) {
                indentLines(out);
                return;
        }
        if (!_insertSpaces) {
                typeCharacter("\t");
                return;
        }
        const uint32_t column = _document->columnOf(range.start.line, range.start.byte, tabWidth());
        const uint32_t spaces = tabWidth() - column % tabWidth();
        char           buffer[64];
        uint32_t       n = spaces < sizeof(buffer) ? spaces : sizeof(buffer);
        for (uint32_t i = 0; i < n; ++i) buffer[i] = ' ';
        replaceSelection(StringView(buffer, n), Kind::Other);
        return;
}

void TextEdit::indentLines(bool out) {
        if (_readOnly) return;
        TextSelection   sel = _cursor.selection();
        const TextRange range = sel.range();
        uint32_t        last = range.end.line;
        // A selection ending at a line's start doesn't take that line.
        if (last > range.start.line && range.end.byte == 0) --last;
        char       unit[64];
        const auto width = tabWidth() < sizeof(unit) ? tabWidth() : uint32_t(sizeof(unit));
        size_t     unitLength = 1;
        unit[0] = '\t';
        if (_insertSpaces) {
                for (uint32_t i = 0; i < width; ++i) unit[i] = ' ';
                unitLength = width;
        }
        beginEdit();
        for (uint32_t line = range.start.line; line <= last; ++line) {
                const StringView text = _document->line(line);
                int32_t          delta = 0;
                if (!out) {
                        if (text.isEmpty()) continue; // blank lines stay blank
                        (void)_document->insert(TextPosition{line, 0}, StringView(unit, unitLength));
                        delta = int32_t(unitLength);
                } else {
                        // A tab, or up to a tab's width of spaces.
                        uint32_t n = 0;
                        if (!text.isEmpty() && text[0] == '\t')
                                n = 1;
                        else
                                while (n < text.size() && n < width && text[n] == ' ') ++n;
                        if (n == 0) continue;
                        (void)_document->remove(TextRange{{line, 0}, {line, n}});
                        delta = -int32_t(n);
                }
                for (TextPosition *p : {&sel.cursor, &sel.anchor}) {
                        if (p->line != line) continue;
                        if (delta > 0 && p->byte > 0)
                                p->byte += uint32_t(delta);
                        else if (delta < 0)
                                p->byte = p->byte > uint32_t(-delta) ? p->byte - uint32_t(-delta) : 0;
                }
        }
        _cursor.setSelection(sel);
        endEdit();
        cursorChanged();
        return;
}

void TextEdit::insertText(StringView text) {
        replaceSelection(text, Kind::Other);
        return;
}

void TextEdit::deleteSelection() {
        if (_cursor.hasSelection()) replaceSelection(StringView(), Kind::Other);
        return;
}

void TextEdit::undo() {
        auto r = _document->undo();
        if (r.isOk()) _cursor.setSelection(r.value());
        cursorChanged();
        return;
}

void TextEdit::redo() {
        auto r = _document->redo();
        if (r.isOk()) _cursor.setSelection(r.value());
        cursorChanged();
        return;
}

void TextEdit::copy() {
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard == nullptr || !_cursor.hasSelection()) return;
        String text(MemSpace::Psram);
        if (selectedText(text).isOk()) (void)clipboard->copyText(Clipboard::Selection::Clipboard, text.view());
        return;
}

void TextEdit::cut() {
        if (_readOnly) {
                copy();
                return;
        }
        copy();
        deleteSelection();
        return;
}

void TextEdit::paste() {
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard != nullptr && !_readOnly)
                (void)clipboard->paste(Clipboard::Selection::Clipboard, ClipboardType::Text, pasted, this);
        return;
}

void TextEdit::pasted(void *arg, Error err, StringView type, StringView data) {
        (void)type;
        auto *self = static_cast<TextEdit *>(arg);
        if (err.isOk()) self->insertText(data);
        return;
}

void TextEdit::selectAll() {
        _cursor.selectAll();
        _document->breakMerge();
        cursorChanged();
        return;
}

// ---- Find and replace

bool TextEdit::find(StringView needle, uint32_t flags) {
        const TextRange    sel = _cursor.range();
        const TextPosition from = (flags & TextDocument::FindBackward) != 0 ? sel.start : sel.end;
        auto               r = _document->find(needle, from, flags);
        if (r.isError()) return false;
        _cursor.setSelection(TextSelection{r.value().end, r.value().start});
        _document->breakMerge();
        cursorChanged();
        return true;
}

bool TextEdit::replace(StringView needle, StringView replacement, uint32_t flags) {
        if (_readOnly) return false;
        // The selection is replaced only if it is what is sought (as found, or typed over).
        String selected(MemSpace::Psram);
        if (_cursor.hasSelection() && selectedText(selected).isOk()) {
                const TextRange r = _cursor.range();
                auto            check = _document->find(needle, r.start,
                                                        flags & ~(TextDocument::FindBackward | TextDocument::FindWrap));
                if (check.isOk() && check.value() == r) replaceSelection(replacement, Kind::Other);
        }
        return find(needle, flags);
}

size_t TextEdit::replaceAll(StringView needle, StringView replacement, uint32_t flags) {
        if (_readOnly) return 0;
        beginEdit();
        auto n = _document->replaceAll(needle, replacement, flags);
        _cursor.clamp();
        endEdit();
        cursorChanged();
        return n.value();
}

// ---- Keys

bool TextEdit::keyPressed(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        if (editKey(event)) return true;
        const bool shift = event.hasShift();
        const bool ctrl = event.hasCtrl() && !event.hasAlt();
        const bool alt = event.hasAlt();
        const Rect area = textArea();
        const auto page = static_cast<uint32_t>(area.height / LineHeight > 1 ? area.height / LineHeight - 1 : 1);
        bool       moved = true;
        switch (event.usage) {
                case HidKey::Left: ctrl ? _cursor.moveWordLeft(shift) : _cursor.moveLeft(shift); break;
                case HidKey::Right: ctrl ? _cursor.moveWordRight(shift) : _cursor.moveRight(shift); break;
                case HidKey::Up:
                        if (ctrl) {
                                scrollTo(scrollX(), int64_t(scrollY()) - LineHeight);
                                return true;
                        }
                        _cursor.moveUp(shift);
                        break;
                case HidKey::Down:
                        if (ctrl) {
                                scrollTo(scrollX(), int64_t(scrollY()) + LineHeight);
                                return true;
                        }
                        _cursor.moveDown(shift);
                        break;
                case HidKey::Home: ctrl ? _cursor.moveDocumentStart(shift) : _cursor.moveLineStart(shift); break;
                case HidKey::End: ctrl ? _cursor.moveDocumentEnd(shift) : _cursor.moveLineEnd(shift); break;
                case HidKey::PageUp:
                        _cursor.moveUp(shift, page);
                        scrollTo(scrollX(), int64_t(scrollY()) - int64_t(page) * LineHeight);
                        break;
                case HidKey::PageDown:
                        _cursor.moveDown(shift, page);
                        scrollTo(scrollX(), int64_t(scrollY()) + int64_t(page) * LineHeight);
                        break;
                default: moved = false; break;
        }
        if (moved) {
                _document->breakMerge();
                cursorChanged();
                return true;
        }
        switch (event.usage) {
                case HidKey::Backspace:
                        if (alt) return false;
                        backspace(ctrl);
                        return true;
                case HidKey::Delete:
                        if (shift && !ctrl) {
                                cut();
                                return true;
                        }
                        deleteForward(ctrl);
                        return true;
                case HidKey::Insert:
                        if (ctrl && !shift)
                                copy();
                        else if (shift && !ctrl)
                                paste();
                        else if (!ctrl && !shift)
                                setOverwrite(!_overwrite);
                        else
                                return false;
                        return true;
                case HidKey::Enter:
                case HidKey::KeypadEnter:
                        if (ctrl || alt) return false;
                        newline();
                        return true;
                case HidKey::Tab:
                        if (ctrl || alt) return false; // Ctrl+Tab: the window's (tabs)
                        tab(shift);
                        return true;
                default: break;
        }
        if (ctrl && !shift) {
                switch (event.usage) {
                        case letter('a'): selectAll(); return true;
                        case letter('c'): copy(); return true;
                        case letter('x'): cut(); return true;
                        case letter('v'): paste(); return true;
                        case letter('z'): undo(); return true;
                        case letter('y'): redo(); return true;
                        default: break;
                }
        }
        if (ctrl && shift && event.usage == letter('z')) {
                redo();
                return true;
        }
        (void)SlashKey;
        if (!ctrl && !alt && event.codepoint >= 0x20 && event.codepoint != 0x7f) {
                char         utf8[4];
                const size_t n = Utf8::encode(event.codepoint, utf8);
                typeCharacter(StringView(utf8, n));
                return true;
        }
        return false;
}

// ---- The mouse

void TextEdit::gutterClicked(uint32_t line, const WindowManager::MouseEvent &event) {
        const bool   extend = (event.modifiers & KeyEvent::Shift) != 0;
        TextPosition start{line, 0};
        TextPosition end = line + 1 < _document->lineCount() ? TextPosition{line + 1, 0} : _document->end();
        if (extend) {
                TextSelection sel = _cursor.selection();
                sel.cursor = sel.anchor <= start ? end : start;
                _cursor.setSelection(sel);
        } else {
                _cursor.setSelection(TextSelection{end, start});
        }
        _clicks = 3;
        _dragUnit = TextRange{start, end};
        _dragging = true;
        cursorChanged();
        return;
}

void TextEdit::mousePressed(const WindowManager::MouseEvent &event) {
        setFocus();
        if (event.button == PointerEvent::Middle) {
                GuiClipboard *clipboard = clipboardOf(*this);
                if (clipboard == nullptr || _readOnly) return;
                _cursor.setPosition(positionAt(event.x, event.y));
                cursorChanged();
                (void)clipboard->paste(Clipboard::Selection::Primary, ClipboardType::Text, pasted, this);
                return;
        }
        if (event.button != PointerEvent::Left) return;
        const Rect         v = viewport();
        const TextPosition at = positionAt(event.x, event.y);
        if (event.x < int(textArea().x) && event.x >= int(v.x)) {
                gutterClicked(at.line, event);
                return;
        }
        const uint64_t now = System::uptimeUs();
        _clicks = (now - _pressUs <= uint64_t(WindowManager::DoubleClickMs) * 1000) ? uint8_t(_clicks % 3 + 1) : 1;
        _pressUs = now;
        const bool extend = (event.modifiers & KeyEvent::Shift) != 0;
        if (_clicks == 1) {
                _cursor.setPosition(at, extend);
                _dragUnit = TextRange{at, at};
        } else {
                TextCursor c(*_document);
                c.setTabWidth(tabWidth());
                c.setPosition(at);
                if (_clicks == 2)
                        c.selectWord();
                else
                        c.selectLine();
                _dragUnit = c.range();
                _cursor.setSelection(TextSelection{_dragUnit.end, _dragUnit.start});
        }
        _dragging = true;
        _document->breakMerge();
        cursorChanged();
        return;
}

void TextEdit::dragTo(int x, int y) {
        const TextPosition at = positionAt(x, y);
        if (_clicks <= 1) {
                _cursor.setPosition(at, true);
        } else {
                // By words or lines: the unit first taken stays selected.
                TextCursor c(*_document);
                c.setPosition(at);
                if (_clicks == 2)
                        c.selectWord();
                else
                        c.selectLine();
                const TextRange u = c.range();
                if (at < _dragUnit.start)
                        _cursor.setSelection(TextSelection{u.start, _dragUnit.end});
                else
                        _cursor.setSelection(TextSelection{u.end, _dragUnit.start});
        }
        cursorChanged();
        return;
}

void TextEdit::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_dragging || (event.buttons & PointerEvent::Left) == 0) return;
        _dragX = event.x;
        _dragY = event.y;
        dragTo(event.x, event.y);
        // Past an edge, it goes on scrolling while held there.
        const Rect v = viewport();
        const bool outside = event.y < v.y || event.y >= v.bottom() || event.x < textArea().x || event.x >= v.right();
        GuiWindow *w = window();
        if (outside && _edgeTimer < 0 && w != nullptr) {
                auto r = w->application().startTimer(EdgeMs, true, edgeTick, this);
                if (r.isOk()) _edgeTimer = r.value();
        } else if (!outside && _edgeTimer >= 0 && w != nullptr) {
                w->application().stopTimer(_edgeTimer);
                _edgeTimer = -1;
        }
        return;
}

void TextEdit::edgeTick(void *arg) {
        auto *self = static_cast<TextEdit *>(arg);
        if (!self->_dragging) return;
        self->dragTo(self->_dragX, self->_dragY);
        return;
}

void TextEdit::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        _dragging = false;
        GuiWindow *w = window();
        if (_edgeTimer >= 0 && w != nullptr) {
                w->application().stopTimer(_edgeTimer);
                _edgeTimer = -1;
        }
        return;
}

// ---- The primary selection: offered while something is selected, made when asked.

void TextEdit::offerPrimary() {
        GuiClipboard *clipboard = clipboardOf(*this);
        // Once: the text is made when someone asks, from the selection then.
        if (clipboard == nullptr || !_cursor.hasSelection() || clipboard->owns(Clipboard::Selection::Primary)) return;
        const StringView types[] = {ClipboardType::Text};
        (void)clipboard->copy(Clipboard::Selection::Primary, Span<const StringView>(types, 1), providePrimary, this);
        return;
}

Error TextEdit::providePrimary(void *arg, StringView type, String &out) {
        (void)type;
        auto *self = static_cast<TextEdit *>(arg);
        return self->selectedText(out);
}

// ---- Focus and the cursor's blink

void TextEdit::focusIn() {
        _cursorOn = true;
        startBlink();
        updateLine(_cursor.position().line);
        return;
}

void TextEdit::focusOut() {
        stopBlink();
        updateLine(_cursor.position().line);
        return;
}

void TextEdit::startBlink() {
        GuiWindow *w = window();
        if (w == nullptr || !hasFocus()) return;
        if (_blinkTimer >= 0) w->application().stopTimer(_blinkTimer);
        auto r = w->application().startTimer(BlinkMs, true, blink, this);
        _blinkTimer = r.isOk() ? r.value() : -1;
        return;
}

void TextEdit::stopBlink() {
        GuiWindow *w = window();
        if (_blinkTimer >= 0 && w != nullptr) w->application().stopTimer(_blinkTimer);
        _blinkTimer = -1;
        return;
}

void TextEdit::blink(void *arg) {
        auto *self = static_cast<TextEdit *>(arg);
        self->_cursorOn = !self->_cursorOn;
        self->updateLine(self->_cursor.position().line);
        return;
}

// ---- Drops: text, or files as their paths, inserted where they fall.

void TextEdit::dragMoved(GuiDropEvent &event) {
        const bool text = event.hasType(ClipboardType::Text) && !_readOnly;
        event.accept(text && DropActions::allows(event.actions(), DropAction::Copy) ? DropAction::Copy
                                                                                    : DropAction::None);
        return;
}

bool TextEdit::dropped(GuiDropEvent &event) {
        if (_readOnly || !event.hasType(ClipboardType::Text) || !DropActions::allows(event.actions(), DropAction::Copy))
                return false;
        if (event.read(ClipboardType::Text, dropRead, this).isError()) return false;
        _dropDrag = event.drag();
        _dropAt = positionAt(event.x(), event.y());
        return true;
}

void TextEdit::dropRead(void *arg, Error err, StringView type, StringView data) {
        (void)type;
        auto          *self = static_cast<TextEdit *>(arg);
        GuiWindow     *w = self->window();
        const uint32_t drag = self->_dropDrag;
        self->_dropDrag = 0;
        if (err.isOk()) {
                self->_cursor.setPosition(self->_dropAt);
                self->insertText(data);
                self->setFocus();
        }
        if (w != nullptr)
                (void)w->application().windowManager().finishDrop(drag,
                                                                  err.isOk() ? DropAction::Copy : DropAction::None);
        return;
}

// ---- Drawing

void TextEdit::paintContent(Painter &painter, const Rect &viewport) {
        if (_paletteColors) {
                const WindowPalette &palette = painter.palette();
                _colors.background = palette.color(Role::FaceHighlight);
                _colors.text = palette.color(isEnabled() ? Role::FaceText : Role::DisabledText);
                _colors.selectionBackground = palette.color(Role::Highlight);
                _colors.selectionText = palette.color(Role::HighlightText);
                _colors.cursor = _colors.text;
                _colors.currentLine = _colors.background;
        }
        // A row's cells: as many as the widest window could show, made once.
        const size_t need = viewport.width / CellWidth + 4;
        if (_cellCount < need) {
                Memory::free(_cells);
                _cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Internal, sizeof(TextCell) * need));
                if (_cells == nullptr)
                        _cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Psram, sizeof(TextCell) * need));
                _cellCount = _cells != nullptr ? need : 0;
        }
        const Rect     area = textArea();
        const uint32_t first = scrollY() / LineHeight;
        const uint32_t last = (scrollY() + viewport.height + LineHeight - 1) / LineHeight;
        for (uint32_t line = first; line <= last; ++line) {
                const int y = windowY(line * uint32_t(LineHeight));
                if (y >= int(viewport.bottom())) break;
                if (line >= _document->lineCount()) {
                        // Below the text: the background.
                        const int top = y < int(viewport.y) ? viewport.y : y;
                        (void)painter.fill(Rect{area.x, uint16_t(top), area.width, uint16_t(viewport.bottom() - top)},
                                           _colors.background);
                        break;
                }
                paintLine(painter, line, y, area);
        }
        // The gutter over what scrolled under it.
        if (gutterWidth() != 0) {
                for (uint32_t line = first; line <= last && line < _document->lineCount(); ++line) {
                        const int y = windowY(line * uint32_t(LineHeight));
                        if (y >= int(viewport.bottom())) break;
                        paintGutter(painter, line, Rect{viewport.x, uint16_t(y), gutterWidth(), LineHeight});
                }
                const uint32_t below = _document->lineCount() * uint32_t(LineHeight);
                const int      y = windowY(below);
                if (y < int(viewport.bottom()))
                        paintGutter(painter, UINT32_MAX,
                                    Rect{viewport.x, uint16_t(y < int(viewport.y) ? viewport.y : y), gutterWidth(),
                                         uint16_t(viewport.bottom() - (y < int(viewport.y) ? viewport.y : y))});
        }
        return;
}

void TextEdit::paintLine(Painter &painter, uint32_t line, int y, const Rect &area) {
        const StringView text = _document->line(line);
        const uint32_t   lineBg = lineBackground(line);
        // The margin, to column 0.
        const int base = textLeft() + int(scrollX());
        (void)painter.fill(Rect{area.x, uint16_t(y), uint16_t(base - area.x), LineHeight}, lineBg);
        StyleRun     runs[MaxRuns];
        const size_t runCount = styleLine(line, text, runs, MaxRuns);
        // The selection on this line: bytes [selFrom, selTo), and its newline if it goes past.
        const TextRange sel = _cursor.range();
        uint32_t        selFrom = 0, selTo = 0;
        bool            selNewline = false;
        if (!sel.isEmpty() && sel.start.line <= line && line <= sel.end.line) {
                selFrom = sel.start.line == line ? sel.start.byte : 0;
                selTo = sel.end.line == line ? sel.end.byte : uint32_t(text.size());
                selNewline = sel.end.line > line;
        }
        // The cells from the first column that shows.
        const uint32_t firstColumn = scrollX() / CellWidth;
        const uint32_t maxCells = _cellCount;
        size_t         n = 0, run = 0;
        uint32_t       column = 0;
        int            x0 = 0; // where the first cell is drawn
        bool           started = false;
        size_t         i = 0;
        const uint32_t tab = tabWidth();
        while (i < text.size() && n < maxCells) {
                const auto byte = static_cast<uint32_t>(i);
                uint32_t   cp;
                uint32_t   width;
                if (text[i] == '\t') {
                        cp = ' ';
                        width = (column / tab + 1) * tab - column;
                        ++i;
                } else {
                        cp = Utf8::decode(text, i);
                        width = TextDocument::characterColumns(cp);
                        if (width == 0) continue; // a combining mark: not drawn on its own cell
                }
                while (run < runCount && runs[run].end <= byte) ++run;
                const bool inRun = run < runCount && runs[run].start <= byte;
                const bool selected = byte >= selFrom && byte < selTo;
                uint32_t   fg = inRun ? runs[run].text : _colors.text;
                uint32_t   bg = inRun && runs[run].background != NoColor ? runs[run].background : lineBg;
                uint8_t    flags = inRun ? runs[run].flags : 0;
                if (selected) {
                        fg = _colors.selectionText;
                        bg = _colors.selectionBackground;
                }
                if (!started && column + width > firstColumn) {
                        started = true;
                        x0 = textLeft() + int(column * CellWidth);
                }
                if (started) {
                        // A tab: its width in spaces; a wide character: one cell twice as wide.
                        const uint32_t cells = text[byte] == '\t' ? width : 1;
                        for (uint32_t k = 0; k < cells && n < maxCells; ++k) {
                                _cells[n].codepoint = shown(cp);
                                _cells[n].fg = fg;
                                _cells[n].bg = bg;
                                _cells[n].flags = flags;
                                ++n;
                        }
                }
                column += width;
        }
        if (!started) x0 = textLeft() + int(column * CellWidth);
        const int right = int(area.right());
        if (n != 0 && x0 < right) {
                // Cells may start left of the text area (scrolled by part of a character): the
                // painter clips them, and the gutter is drawn over.
                (void)painter.cells(x0, y, Span<const TextCell>(_cells, n), Fonts::unscii16, uint16_t(right - x0));
        }
        // What is past the text: a selected newline's cell, then the line's background.
        int end = textLeft() + int(column * CellWidth);
        if (end < base) end = base;
        if (selNewline && end < right) {
                (void)painter.fill(Rect{uint16_t(end), uint16_t(y), CellWidth, LineHeight},
                                   _colors.selectionBackground);
                end += CellWidth;
        }
        if (end < right)
                (void)painter.fill(Rect{uint16_t(end), uint16_t(y), uint16_t(right - end), LineHeight}, lineBg);
        paintOver(painter, line, y, area);
        // The cursor: a bar before the character it is at (a block in overwrite), while it has the keys.
        const GuiWindow *w = window();
        if (line != _cursor.position().line || !_cursorOn || !hasFocus() || w == nullptr || !w->isActive()) return;
        const uint32_t cc = _document->columnOf(line, _cursor.position().byte, tab);
        const int      cx = textLeft() + int(cc * CellWidth);
        if (cx < int(area.x) || cx >= right) return;
        if (_overwrite)
                (void)painter.fill(Rect{uint16_t(cx), uint16_t(y + LineHeight - 3), CellWidth, 3}, _colors.cursor);
        else
                (void)painter.fill(Rect{uint16_t(cx > int(area.x) ? cx - 1 : cx), uint16_t(y), 2, LineHeight},
                                   _colors.cursor);
        return;
}

BMDS_NAMESPACE_END
