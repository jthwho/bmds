/**
 * @file      chatview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ChatView (devplan/chatview.md): lines laid out by ChatText, kept by ChatLines, drawn a row
 * at a time, a font's run of pieces in one Painter::text() call.
 */

#include <bmds/chatview.h>
#include <new>
#include <bmds/guiapplication.h>
#include <bmds/guiclipboard.h>
#include <bmds/guiwindow.h>
#include <bmds/memory.h>
#include <bmds/system.h>
#include <bmds/systemfonts.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint32_t Column = 4;           // where Painter::text() starts a font's text
        constexpr uint32_t Unwrapped = 1u << 24; // the width (pixels) unwrapped lines are laid out at
        constexpr uint32_t Sub = 64;             // the layout's units a pixel: a font's pen's (Font::layout())
        constexpr uint32_t EdgeMs = 50;          // a drag past an edge scrolls this often
        constexpr int      DragSlop = 3;         // pixels a press moves before it is a drag
        constexpr uint32_t LineEnd = UINT32_MAX; // a Pos's byte: its line's end
        constexpr uint8_t  KeyA = HidKey::A;     // Ctrl+A
        constexpr uint8_t  KeyC = HidKey::A + 2; // Ctrl+C

        GuiClipboard *clipboardOf(const Widget &w) {
                GuiWindow *window = w.window();
                return window != nullptr ? window->application().clipboard() : nullptr;
        }

        int clampInt(int v, int lo, int hi) {
                return v < lo ? lo : v > hi ? hi : v;
        }

        // A layout x (1/64 pixels) as the pixel a pen there draws at.
        int pixelOf(uint32_t units) {
                return int((units + Sub / 2) / Sub);
        }

        // Fills [x0, x1) × [y0, y1) (window coordinates), cut to @p limit.
        void fillSpan(Painter &painter, int x0, int x1, int y0, int y1, uint32_t rgb, const Rect &limit) {
                x0 = clampInt(x0, limit.x, limit.right());
                x1 = clampInt(x1, limit.x, limit.right());
                y0 = clampInt(y0, limit.y, limit.bottom());
                y1 = clampInt(y1, limit.y, limit.bottom());
                if (x1 <= x0 || y1 <= y0) return;
                (void)painter.fill(Rect{uint16_t(x0), uint16_t(y0), uint16_t(x1 - x0), uint16_t(y1 - y0)}, rgb);
                return;
        }

} // namespace

ChatView::ChatView(Widget *parent) : ScrollArea(parent) {
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(Laid));
        if (memory != nullptr) _laid = ::new (memory) Laid();
        setFocusable(true);
        resetPalette();
        openFonts();
        _laidWidth = wrapWidth();
}

ChatView::~ChatView() {
        GuiWindow *w = window();
        if (_edgeTimer >= 0 && w != nullptr) w->application().stopTimer(_edgeTimer);
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard != nullptr) clipboard->forget(this); // the primary selection's provider
        if (_laid != nullptr) {
                _laid->~Laid();
                Memory::free(_laid);
        }
}

// ---- Fonts and measuring

size_t ChatView::fontIndex(uint8_t flags) {
        return ((flags & ChatText::Bold) != 0 ? 1u : 0u) | ((flags & ChatText::Italic) != 0 ? 2u : 0u) |
               ((flags & ChatText::Monospace) != 0 ? 4u : 0u);
}

// Measured in a font's pen's units (1/64 pixels), not its rounded pixels: pieces measured one
// by one then add up to where the painter, drawing a font's run of them at once, puts each.
uint32_t ChatView::measureWidth(void *arg, StringView text, uint8_t flags) {
        const int64_t advance = static_cast<const ChatView *>(arg)->fontFor(flags).layout(text, nullptr, nullptr);
        return advance <= 0 ? 0 : uint32_t(advance);
}

size_t ChatView::measureFitting(void *arg, StringView text, uint8_t flags, uint32_t most) {
        struct Fit {
                        int64_t limit;
                        size_t  bytes;
        } fit{int64_t(most), text.size()};
        (void)static_cast<const ChatView *>(arg)->fontFor(flags).layout(
                text,
                [](void *a, const Font::Placed &g) {
                        auto *f = static_cast<Fit *>(a);
                        if (g.pen + g.advance <= f->limit) return true;
                        f->bytes = g.offset;
                        return false;
                },
                &fit);
        return fit.bytes;
}

ChatText::Measure ChatView::measure() const {
        return ChatText::Measure{measureWidth, measureFitting, const_cast<ChatView *>(this), Column * Sub};
}

uint32_t ChatView::unitsOf(StringView text, uint8_t flags) const {
        return measureWidth(const_cast<ChatView *>(this), text, flags);
}

// Its font, and its bold, italic and monospace (the mono role's at its size) as it can open
// them (one it can't: the nearest it has); the row's height holding every one's baseline lined
// up.
void ChatView::openFonts() {
        const Font base = font();
        FontName   name = fontName();
        if (name.familyLength == 0 && name.role == FontName::Role::None) name.role = fontRole();
        _fonts[0] = base;
        for (size_t i = 1; i < 8; ++i) {
                FontName n = name;
                if ((i & 4) != 0) {
                        n = FontName();
                        n.role = FontName::Role::Mono;
                        n.size = static_cast<uint16_t>(base.pixelSize());
                }
                n.style = static_cast<uint8_t>(((i & 4) != 0 ? 0 : name.style) | ((i & 1) != 0 ? FontName::Bold : 0) |
                                               ((i & 2) != 0 ? FontName::Italic : 0));
                n.hasStyle = true;
                auto [f, err] = SystemFonts::library().open(n);
                _fonts[i] = err.isOk() && f.isValid() ? f : _fonts[i & (i - 1)];
        }
        int ascent = 0, below = 0;
        for (const Font &f : _fonts) {
                const Font::Metrics &m = f.metrics();
                if (m.ascent > ascent) ascent = m.ascent;
                if (m.lineHeight - m.ascent > below) below = m.lineHeight - m.ascent;
        }
        _baseline = static_cast<uint16_t>(ascent);
        _rowHeight = static_cast<uint16_t>(ascent + below > 0 ? ascent + below : 16);
        for (size_t i = 0; i < 8; ++i) _fontTop[i] = static_cast<int16_t>(ascent - _fonts[i].metrics().ascent);
        setLineSteps(16, _rowHeight);
        return;
}

void ChatView::styleChanged() {
        openFonts();
        relayout(true);
        return;
}

// The width lines wrap at: from the lines' left (a column past the margin) to the margin before
// the vertical bar, whose room is always kept (its coming and going would wrap everything again).
uint32_t ChatView::wrapWidth() const {
        if (!_wrap) return Unwrapped;
        const Rect r = geometry();
        const int  left = int((r.x + Margin + Column - 1) & ~(Column - 1));
        const int  right = int(r.right()) - ScrollBar::Breadth - Margin;
        return right - left > int(Column) ? uint32_t(right - left) : Unwrapped; // not laid out yet
}

uint32_t ChatView::textColumn() const {
        if (_gutterWidest == 0) return 0;
        const uint32_t pixels = (_gutterWidest + Sub - 1) / Sub + GutterGap;
        return (pixels + Column - 1) & ~(Column - 1);
}

int ChatView::lineLeft() const {
        return int((viewport().x + Margin + Column - 1) & ~(Column - 1)) - int(scrollX());
}

// Line @p line laid out in the scratch (kept there until another is).
const ChatView::Laid *ChatView::layOut(uint64_t line) const {
        if (_laid == nullptr || !_lines.has(line)) return nullptr;
        if (_laid->line == line) return _laid;
        _laid->line = UINT64_MAX;
        if (ChatText::parse(_lines.line(line), _laid->parsed).isError()) return nullptr;
        _laid->layout = ChatText::Layout{_laid->rows, sizeof(_laid->rows) / sizeof(_laid->rows[0]), _laid->pieces,
                                         sizeof(_laid->pieces) / sizeof(_laid->pieces[0])};
        (void)ChatText::wrap(_laid->parsed, measure(), _laidWidth * Sub, _textX * Sub, &_laid->layout);
        _laid->line = line;
        return _laid;
}

// The rows @p parsed takes, and (unwrapped) its width in @p width. The scratch's layout is used.
uint32_t ChatView::rowsOf(const ChatText::Parsed &parsed, uint32_t &width) const {
        width = 0;
        ChatText::Layout l{_laid->rows, sizeof(_laid->rows) / sizeof(_laid->rows[0]), _laid->pieces,
                           sizeof(_laid->pieces) / sizeof(_laid->pieces[0])};
        _laid->line = UINT64_MAX;
        const size_t rows = ChatText::wrap(parsed, measure(), _laidWidth * Sub, _textX * Sub, _wrap ? nullptr : &l);
        if (!_wrap && l.pieceCount > 0) {
                const ChatText::Piece &p = l.pieces[l.pieceCount - 1];
                const uint32_t         end =
                        p.x + unitsOf(parsed.text.view().mid(p.start, p.length), parsed.runs[p.run].style.flags);
                width = (end + Sub - 1) / Sub;
        }
        return rows > 0 ? uint32_t(rows) : 1;
}

// Every line wrapped again (the width, the fonts, the gutter's column changed); the gutters
// measured again too if @p measureGutters (the fonts).
void ChatView::relayout(bool measureGutters) {
        _laidWidth = wrapWidth();
        if (_laid != nullptr) {
                _laid->line = UINT64_MAX;
                const ChatText::Measure m = measure();
                if (measureGutters) {
                        _gutterWidest = 0;
                        for (uint64_t n = _lines.first(); n < _lines.next(); ++n) {
                                if (ChatText::parse(_lines.line(n), _laid->parsed).isError()) continue;
                                const uint32_t g = ChatText::gutterWidth(_laid->parsed, m);
                                if (g > _gutterWidest) _gutterWidest = g;
                        }
                }
                _textX = textColumn();
                _widest = 0;
                for (uint64_t n = _lines.first(); n < _lines.next(); ++n) {
                        uint32_t rows = 1, width = 0;
                        if (ChatText::parse(_lines.line(n), _laid->parsed).isOk()) rows = rowsOf(_laid->parsed, width);
                        _lines.setRows(n, rows);
                        if (width > _widest) _widest = width;
                }
                _lines.restack();
                _laid->line = UINT64_MAX;
        }
        updateContentSize();
        followOrKeep();
        update();
        return;
}

void ChatView::updateContentSize() {
        const uint64_t h = viewRows() * _rowHeight;
        const uint32_t w = _wrap ? 0 : _widest + 2 * Margin + Column;
        const bool     was = _internal;
        _internal = true;
        setContentSize(w, h > UINT32_MAX ? UINT32_MAX : uint32_t(h));
        _internal = was;
        return;
}

void ChatView::resized() {
        const bool was = _internal;
        _internal = true;
        ScrollArea::resized();
        _internal = was;
        if (wrapWidth() != _laidWidth)
                relayout(false);
        else
                followOrKeep();
        return;
}

GuiSize ChatView::sizeHint() const {
        return GuiSize{400, static_cast<uint16_t>(_rowHeight * 10)};
}

// ---- The lines

Error ChatView::appendOne(StringView line, bool &wider) {
        const uint64_t first = _lines.first();
        Error          err;
        if (_laid == nullptr) {
                err = _lines.append(line, 1);
        } else {
                _laid->line = UINT64_MAX;
                if (err = ChatText::parse(line, _laid->parsed); err.isError()) return err;
                const uint32_t g = ChatText::gutterWidth(_laid->parsed, measure());
                if (g > _gutterWidest) {
                        _gutterWidest = g;
                        if (textColumn() != _textX) wider = true; // everything again, after
                }
                uint32_t       width = 0;
                const uint32_t rows = wider ? 1 : rowsOf(_laid->parsed, width);
                err = _lines.append(line, rows);
                if (err.isOk() && width > _widest) _widest = width;
        }
        if (_lines.first() != first) linesDropped();
        return err;
}

Error ChatView::append(StringView line) {
        return appendMany(Span<const StringView>(&line, 1));
}

Error ChatView::appendMany(Span<const StringView> lines) {
        bool  wider = false;
        Error err;
        for (const StringView &line : lines)
                if (err = appendOne(line, wider); err.isError()) break;
        if (wider) {
                relayout(false);
        } else {
                updateContentSize();
                followOrKeep();
                update();
        }
        return err;
}

// The oldest lines went: the marker with its line, the selection cut to what is kept.
void ChatView::linesDropped() {
        const uint64_t first = _lines.first();
        if (_marker && _markerLine < first) _marker = false;
        if (_selAnchor.line < first) _selAnchor = Pos{first, 0};
        if (_selCursor.line < first) _selCursor = Pos{first, 0};
        if (_unitFrom.line < first) _unitFrom = Pos{first, 0};
        if (_unitTo.line < first) _unitTo = Pos{first, 0};
        return;
}

void ChatView::clear() {
        _lines.clear();
        if (_laid != nullptr) _laid->line = UINT64_MAX;
        _marker = false;
        _selAnchor = _selCursor = _unitFrom = _unitTo = Pos{_lines.next(), 0};
        _dragging = false;
        _gutterWidest = 0;
        _textX = 0;
        _widest = 0;
        updateContentSize();
        internalScroll(0, 0);
        setFollow(true);
        update();
        return;
}

Error ChatView::setMaxLines(size_t maxLines) {
        if (maxLines == 0 || maxLines > MostLines) return Error::Invalid;
        const uint64_t first = _lines.first();
        if (Error err = _lines.setMaxLines(maxLines); err.isError()) return err;
        if (_lines.first() != first) {
                linesDropped();
                updateContentSize();
                followOrKeep();
                update();
        }
        return Error::Ok;
}

// ---- How it shows them

void ChatView::setWrap(bool wrap) {
        if (wrap == _wrap) return;
        _wrap = wrap;
        relayout(false);
        return;
}

void ChatView::setPaletteColour(size_t index, uint32_t rgb) {
        if (index >= ChatText::PaletteSize) return;
        _palette[index] = rgb & 0xffffffu;
        update();
        return;
}

void ChatView::resetPalette() {
        for (size_t i = 0; i < ChatText::PaletteSize; ++i) _palette[i] = ChatText::paletteColour(uint8_t(i));
        update();
        return;
}

Error ChatView::setMarker(StringView words) {
        if (Error err = _markerWords.assign(words); err.isError()) return err;
        _marker = true;
        _markerLine = _lines.next();
        updateContentSize();
        followOrKeep();
        update();
        return Error::Ok;
}

void ChatView::clearMarker() {
        if (!_marker) return;
        _marker = false;
        updateContentSize();
        followOrKeep();
        update();
        return;
}

// ---- Rows: the lines' rows, the marker's among them

uint64_t ChatView::viewRows() const {
        return _lines.totalRows() + (_marker ? 1 : 0);
}

uint64_t ChatView::markerRow() const {
        return _lines.rowOf(_markerLine);
}

uint64_t ChatView::rowOfLine(uint64_t line) const {
        return _lines.rowOf(line) + (_marker && line >= _markerLine ? 1 : 0);
}

ChatView::RowKind ChatView::rowAt(uint64_t row, uint64_t &line, uint32_t &within) const {
        within = 0;
        line = _lines.next();
        if (_marker) {
                const uint64_t m = markerRow();
                if (row == m) {
                        line = _markerLine;
                        return RowKind::Marker;
                }
                if (row > m) --row;
        }
        if (row >= _lines.totalRows()) return RowKind::Past;
        line = _lines.lineAtRow(row, within);
        return RowKind::Line;
}

// ---- Scrolling: following the end, or keeping what shows at the top

void ChatView::internalScroll(int64_t x, int64_t y) {
        const bool was = _internal;
        _internal = true;
        scrollTo(x, y);
        _internal = was;
        return;
}

void ChatView::setFollow(bool follow) {
        if (follow == _follow) return;
        _follow = follow;
        if (_scrolledBack != nullptr) _scrolledBack(_scrolledArg, *this, !follow);
        return;
}

void ChatView::scrollToEnd() {
        internalScroll(scrollX(), INT64_MAX / 2);
        setFollow(true);
        return;
}

void ChatView::followOrKeep() {
        if (_follow) {
                internalScroll(scrollX(), INT64_MAX / 2);
                return;
        }
        uint64_t y = 0;
        if (_anchorMarker && _marker) {
                y = markerRow() * _rowHeight + _anchorPixel;
        } else if (_lines.has(_anchorLine)) {
                const uint32_t rows = _lines.rows(_anchorLine);
                const uint32_t row = _anchorRow < rows ? _anchorRow : (rows > 0 ? rows - 1 : 0);
                y = (rowOfLine(_anchorLine) + row) * _rowHeight + _anchorPixel;
        }
        internalScroll(scrollX(), int64_t(y > INT64_MAX / 2 ? INT64_MAX / 2 : y));
        noteAnchor();
        return;
}

// What shows at the top, kept while scrolled back.
void ChatView::noteAnchor() {
        if (_rowHeight == 0) return;
        const uint64_t top = scrollY() / _rowHeight;
        _anchorPixel = scrollY() % _rowHeight;
        uint64_t      line = 0;
        uint32_t      within = 0;
        const RowKind kind = rowAt(top, line, within);
        _anchorMarker = kind == RowKind::Marker;
        _anchorLine = line;
        _anchorRow = within;
        return;
}

void ChatView::scrolled() {
        if (_internal) return;
        // The user's: at the end it follows again, anywhere else it stays.
        noteAnchor();
        setFollow(uint64_t(scrollY()) + viewport().height >= contentHeight());
        return;
}

// ---- The selection

ChatView::Pos ChatView::posAt(int x, int y) const {
        if (_lines.count() == 0 || _rowHeight == 0) return Pos{_lines.next(), 0};
        const Rect     v = viewport();
        const uint64_t total = viewRows();
        int64_t        cy = int64_t(y) - v.y + scrollY();
        if (cy < 0) return Pos{_lines.first(), 0};
        if (uint64_t(cy) >= total * _rowHeight) return Pos{_lines.next() - 1, LineEnd};
        uint64_t      line = 0;
        uint32_t      within = 0;
        const RowKind kind = rowAt(uint64_t(cy) / _rowHeight, line, within);
        if (kind == RowKind::Past || !_lines.has(line)) return Pos{_lines.next() - 1, LineEnd};
        if (kind == RowKind::Marker) return Pos{line, 0};
        const Laid *laid = layOut(line);
        if (laid == nullptr) return Pos{line, 0};
        return Pos{line, ChatText::hit(laid->parsed, measure(), laid->layout, within, (x - lineLeft()) * int32_t(Sub))};
}

// The link under (@p x, @p y), its line in @p line; -1 for none.
int ChatView::linkAt(int x, int y, uint64_t &line) const {
        const Rect v = viewport();
        if (_rowHeight == 0 || x < v.x || x >= v.right() || y < v.y || y >= v.bottom()) return -1;
        uint32_t within = 0;
        if (rowAt((uint64_t(y - v.y) + scrollY()) / _rowHeight, line, within) != RowKind::Line) return -1;
        const Laid *laid = layOut(line);
        if (laid == nullptr || within >= laid->layout.rowCount) return -1;
        const ChatText::Row &r = laid->layout.rows[within];
        const int64_t        rx = int64_t(x - lineLeft()) * Sub;
        for (uint32_t k = 0; k < r.pieceCount; ++k) {
                const ChatText::Piece &p = laid->layout.pieces[r.firstPiece + k];
                const ChatText::Run   &run = laid->parsed.runs[p.run];
                const uint32_t         w = unitsOf(laid->parsed.text.view().mid(p.start, p.length), run.style.flags);
                if (rx >= int64_t(p.x) && rx < int64_t(p.x) + w) return run.link;
        }
        return -1;
}

// The word (@p clicks 2) or the line (3) at @p at.
void ChatView::selectUnit(Pos at, uint8_t clicks, Pos &from, Pos &to) const {
        from = to = at;
        const Laid *laid = layOut(at.line);
        if (laid == nullptr) return;
        const StringView t = laid->parsed.text.view();
        const auto       size = uint32_t(t.size());
        if (clicks >= 3) {
                from = Pos{at.line, 0};
                to = Pos{at.line, size};
                return;
        }
        // Spaces end a word, and so does the gutter's end.
        const uint32_t gutter = laid->parsed.gutter;
        uint32_t       b = at.byte < size ? at.byte : size;
        uint32_t       e = b;
        const uint32_t low = gutter != ChatText::NoGutter && b >= gutter ? gutter : 0;
        const uint32_t high = gutter != ChatText::NoGutter && b < gutter ? gutter : size;
        while (b > low && t[b - 1] != ' ') --b;
        while (e < high && t[e] != ' ') ++e;
        from = Pos{at.line, b};
        to = Pos{at.line, e};
        return;
}

Error ChatView::selectedText(String &out) const {
        out.clear();
        if (!hasSelection()) return Error::Ok;
        if (_laid == nullptr) return Error::NoMem;
        const Pos      a = _selAnchor < _selCursor ? _selAnchor : _selCursor;
        const Pos      b = _selAnchor < _selCursor ? _selCursor : _selAnchor;
        const uint64_t from = a.line > _lines.first() ? a.line : _lines.first();
        const uint64_t to = b.line < _lines.next() ? b.line + 1 : _lines.next();
        _laid->line = UINT64_MAX;
        Error err;
        for (uint64_t n = from; n < to && err.isOk(); ++n) {
                if (n != from) err = out.append('\n');
                if (err.isOk()) err = ChatText::parse(_lines.line(n), _laid->parsed);
                if (err.isError()) break;
                const StringView t = _laid->parsed.text.view();
                const auto       size = uint32_t(t.size());
                const uint32_t   s = n == a.line ? (a.byte < size ? a.byte : size) : 0;
                const uint32_t   e = n == b.line ? (b.byte < size ? b.byte : size) : size;
                const uint32_t   g = _laid->parsed.gutter;
                if (g != ChatText::NoGutter && s < g && e > g) {
                        err = out.append(t.mid(s, g - s));
                        if (err.isOk()) err = out.append('\t');
                        if (err.isOk()) err = out.append(t.mid(g, e - g));
                } else if (e > s) {
                        err = out.append(t.mid(s, e - s));
                }
        }
        _laid->line = UINT64_MAX;
        return err;
}

void ChatView::selectAll() {
        if (_lines.count() == 0) return;
        _selAnchor = Pos{_lines.first(), 0};
        _selCursor = Pos{_lines.next() - 1, LineEnd};
        selectionChanged();
        return;
}

void ChatView::copy() {
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard == nullptr || !hasSelection()) return;
        String text(MemSpace::Psram);
        if (selectedText(text).isOk()) (void)clipboard->copyText(Clipboard::Selection::Clipboard, text.view());
        return;
}

Error ChatView::providePrimary(void *arg, StringView type, String &out) {
        (void)type;
        return static_cast<ChatView *>(arg)->selectedText(out);
}

// Redrawn; a selection offered as the primary one (its text made when asked).
void ChatView::selectionChanged() {
        update();
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard == nullptr || !hasSelection() || clipboard->owns(Clipboard::Selection::Primary)) return;
        const StringView types[] = {ClipboardType::Text};
        (void)clipboard->copy(Clipboard::Selection::Primary, Span<const StringView>(types, 1), providePrimary, this);
        return;
}

// ---- Keys and the mouse

bool ChatView::keyPressed(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        const bool    ctrl = event.hasCtrl() && !event.hasAlt();
        const int64_t page = viewport().height > _rowHeight ? int64_t(viewport().height - _rowHeight) : _rowHeight;
        switch (event.usage) {
                case HidKey::Up: scrollTo(scrollX(), int64_t(scrollY()) - _rowHeight); return true;
                case HidKey::Down: scrollTo(scrollX(), int64_t(scrollY()) + _rowHeight); return true;
                case HidKey::PageUp: scrollTo(scrollX(), int64_t(scrollY()) - page); return true;
                case HidKey::PageDown: scrollTo(scrollX(), int64_t(scrollY()) + page); return true;
                case HidKey::Home: scrollTo(scrollX(), 0); return true;
                case HidKey::End: scrollToEnd(); return true;
                case HidKey::Insert:
                        if (!ctrl) return false;
                        copy();
                        return true;
                case KeyC:
                        if (!ctrl) return false;
                        copy();
                        return true;
                case KeyA:
                        if (!ctrl) return false;
                        selectAll();
                        return true;
                default: return false;
        }
}

void ChatView::mousePressed(const WindowManager::MouseEvent &event) {
        setFocus();
        if (event.button != PointerEvent::Left) return;
        const uint64_t now = System::uptimeUs();
        _clicks = (now - _pressUs <= uint64_t(WindowManager::DoubleClickMs) * 1000) ? uint8_t(_clicks % 3 + 1) : 1;
        _pressUs = now;
        _pressX = event.x;
        _pressY = event.y;
        _moved = false;
        _pressLink = linkAt(event.x, event.y, _pressLinkLine);
        const Pos at = posAt(event.x, event.y);
        if (_clicks == 1) {
                if ((event.modifiers & KeyEvent::Shift) == 0) _selAnchor = at;
                _selCursor = at;
                _unitFrom = _unitTo = at;
        } else {
                selectUnit(at, _clicks, _unitFrom, _unitTo);
                _selAnchor = _unitFrom;
                _selCursor = _unitTo;
        }
        _dragging = true;
        selectionChanged();
        return;
}

void ChatView::dragTo(int x, int y) {
        const Pos at = posAt(x, y);
        if (_clicks <= 1) {
                _selCursor = at;
        } else {
                // By words or lines: the unit first taken stays selected.
                Pos from, to;
                selectUnit(at, _clicks, from, to);
                if (at < _unitFrom) {
                        _selAnchor = _unitTo;
                        _selCursor = from;
                } else {
                        _selAnchor = _unitFrom;
                        _selCursor = to;
                }
        }
        selectionChanged();
        return;
}

void ChatView::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_dragging || (event.buttons & PointerEvent::Left) == 0) return;
        if (!_moved) {
                const int dx = event.x - _pressX, dy = event.y - _pressY;
                if (dx > -DragSlop && dx < DragSlop && dy > -DragSlop && dy < DragSlop) return;
                _moved = true;
        }
        _dragX = event.x;
        _dragY = event.y;
        dragTo(event.x, event.y);
        // Above or below the view, it goes on scrolling while held there.
        const Rect v = viewport();
        const bool outside = event.y < v.y || event.y >= v.bottom();
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

void ChatView::edgeTick(void *arg) {
        auto *self = static_cast<ChatView *>(arg);
        if (!self->_dragging) return;
        const Rect v = self->viewport();
        if (self->_dragY < v.y)
                self->scrollTo(self->scrollX(), int64_t(self->scrollY()) - self->_rowHeight);
        else if (self->_dragY >= v.bottom())
                self->scrollTo(self->scrollX(), int64_t(self->scrollY()) + self->_rowHeight);
        self->dragTo(self->_dragX, self->_dragY);
        return;
}

void ChatView::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !_dragging) return;
        _dragging = false;
        GuiWindow *w = window();
        if (_edgeTimer >= 0 && w != nullptr) {
                w->application().stopTimer(_edgeTimer);
                _edgeTimer = -1;
        }
        // A click (no drag) on a link, released on it.
        if (_moved || _clicks != 1 || _pressLink < 0 || _linkActivated == nullptr) return;
        uint64_t line = 0;
        if (linkAt(event.x, event.y, line) != _pressLink || line != _pressLinkLine) return;
        const Laid *laid = layOut(line);
        if (laid == nullptr || _pressLink >= int(laid->parsed.linkCount)) return;
        const ChatText::Link &link = laid->parsed.links[_pressLink];
        _linkActivated(_linkArg, *this, laid->parsed.text.view().mid(link.start, link.length));
        return;
}

WindowManager::Pointer ChatView::pointerAt(int x, int y) const {
        uint64_t line = 0;
        return linkAt(x, y, line) >= 0 ? WindowManager::Pointer::Arrow : WindowManager::Pointer::Text;
}

// ---- Drawing

void ChatView::paintContent(Painter &painter, const Rect &viewport) {
        const WindowPalette &palette = painter.palette();
        const uint32_t       background = palette.color(Role::FaceHighlight);
        (void)painter.fill(viewport, background);
        if (_laid == nullptr || _rowHeight == 0) return;
        // The rows the damage reaches.
        const Rect d = painter.damageBounds();
        const int  top = d.y > viewport.y ? d.y : viewport.y;
        const int  bottom = d.bottom() < viewport.bottom() ? d.bottom() : viewport.bottom();
        if (bottom <= top) return;
        for (uint64_t row = (uint64_t(top - viewport.y) + scrollY()) / _rowHeight;; ++row) {
                const int y = int(int64_t(viewport.y) + int64_t(row * _rowHeight) - int64_t(scrollY()));
                if (y >= bottom) break;
                uint64_t      line = 0;
                uint32_t      within = 0;
                const RowKind kind = rowAt(row, line, within);
                if (kind == RowKind::Past) break;
                if (kind == RowKind::Marker) {
                        paintMarker(painter, y, viewport);
                        continue;
                }
                const Laid *laid = layOut(line);
                if (laid != nullptr) paintLine(painter, *laid, within, y, viewport);
        }
        return;
}

// Row @p row of a line at @p y: its pieces a font's run at a time (each starting on a column,
// `ChatText::wrap()` saw to it), each piece's colours cut at the selection's ends.
void ChatView::paintLine(Painter &painter, const Laid &laid, uint32_t row, int y, const Rect &viewport) {
        const ChatText::Parsed &p = laid.parsed;
        const ChatText::Layout &l = laid.layout;
        if (row >= l.rowCount) return;
        const WindowPalette &palette = painter.palette();
        const uint32_t       background = palette.color(Role::FaceHighlight);
        const uint32_t       text = palette.color(isEnabled() ? Role::FaceText : Role::DisabledText);
        const uint32_t       linkText = palette.color(Role::Highlight);
        const uint32_t       selText = palette.color(Role::HighlightText);
        const uint32_t       selBackground = palette.color(Role::Highlight);
        const StringView     t = p.text.view();
        // The selected bytes of this line.
        uint32_t selFrom = 0, selTo = 0;
        if (hasSelection()) {
                const Pos a = _selAnchor < _selCursor ? _selAnchor : _selCursor;
                const Pos b = _selAnchor < _selCursor ? _selCursor : _selAnchor;
                if (a.line <= laid.line && laid.line <= b.line) {
                        selFrom = a.line == laid.line ? a.byte : 0;
                        selTo = b.line == laid.line ? b.byte : UINT32_MAX;
                }
        }
        const ChatText::Row &r = l.rows[row];
        const int            left = lineLeft();
        const auto           colourOf = [this](const ChatText::Colour &c, uint32_t otherwise) {
                if (c.kind == ChatText::Colour::Kind::Index)
                        return _palette[c.index < ChatText::PaletteSize ? c.index : 0];
                if (c.kind == ChatText::Colour::Kind::Rgb) return c.rgb;
                return otherwise;
        };
        uint32_t k = 0;
        while (k < r.pieceCount) {
                // A segment: pieces side by side in one font.
                const ChatText::Piece &first = l.pieces[r.firstPiece + k];
                const size_t           font = fontIndex(p.runs[first.run].style.flags);
                const Font            &f = _fonts[font];
                uint32_t               end = k + 1;
                uint32_t xEnd = first.x + unitsOf(t.mid(first.start, first.length), p.runs[first.run].style.flags);
                while (end < r.pieceCount) {
                        const ChatText::Piece &prev = l.pieces[r.firstPiece + end - 1];
                        const ChatText::Piece &next = l.pieces[r.firstPiece + end];
                        if (fontIndex(p.runs[next.run].style.flags) != font || next.x != xEnd ||
                            next.start != prev.start + prev.length)
                                break;
                        xEnd = next.x + unitsOf(t.mid(next.start, next.length), p.runs[next.run].style.flags);
                        ++end;
                }
                const ChatText::Piece &last = l.pieces[r.firstPiece + end - 1];
                const uint32_t         segStart = first.start;
                const StringView       seg = t.mid(segStart, last.start + last.length - segStart);
                const int              x = left + pixelOf(first.x);
                // Its runs of colour: each piece's, cut at the selection; backgrounds of their own
                // filled the row's height first.
                Painter::TextRun runs[Painter::MaxRuns];
                size_t           n = 0;
                for (uint32_t i = k; i < end; ++i) {
                        const ChatText::Piece &pc = l.pieces[r.firstPiece + i];
                        const ChatText::Run   &run = p.runs[pc.run];
                        uint32_t               fg = colourOf(run.style.fg, run.link >= 0 ? linkText : text);
                        uint32_t               bg = colourOf(run.style.bg, background);
                        if ((run.style.flags & ChatText::Reverse) != 0) {
                                const uint32_t swap = fg;
                                fg = bg;
                                bg = swap;
                        }
                        const uint32_t s = pc.start, e = pc.start + pc.length;
                        const uint32_t cuts[] = {s, selFrom > s && selFrom < e ? selFrom : s,
                                                 selTo > s && selTo < e ? selTo : e, e};
                        for (size_t c = 0; c + 1 < 4; ++c) {
                                if (cuts[c + 1] <= cuts[c]) continue;
                                const bool     selected = cuts[c] >= selFrom && cuts[c] < selTo;
                                const uint32_t partFg = selected ? selText : fg;
                                const uint32_t partBg = selected ? selBackground : bg;
                                if (partBg != background) {
                                        const uint8_t fl = run.style.flags;
                                        const int     px0 =
                                                left + pixelOf(first.x + unitsOf(seg.left(cuts[c] - segStart), fl));
                                        const int px1 =
                                                left + pixelOf(first.x + unitsOf(seg.left(cuts[c + 1] - segStart), fl));
                                        fillSpan(painter, px0, px1, y, y + _rowHeight, partBg, viewport);
                                }
                                if (n == Painter::MaxRuns) {
                                        runs[n - 1].end = cuts[c + 1] - segStart; // past the most: as the last
                                } else {
                                        runs[n++] = Painter::TextRun{cuts[c + 1] - segStart, partFg, partBg};
                                }
                        }
                }
                const uint32_t span = (xEnd - first.x + Sub - 1) / Sub;
                if (n > 0 && x < int(viewport.right()) && x + int(span) > int(viewport.x)) {
                        const uint32_t room = span + Column;
                        (void)painter.text(x, y + _fontTop[font], seg, f, Span<const Painter::TextRun>(runs, n),
                                           uint16_t(room < 0xffffu ? room : 0xffffu));
                }
                // Underlines (links' too) and strikes, in the piece's colour.
                for (uint32_t i = k; i < end; ++i) {
                        const ChatText::Piece &pc = l.pieces[r.firstPiece + i];
                        const ChatText::Run   &run = p.runs[pc.run];
                        const bool             under = (run.style.flags & ChatText::Underline) != 0 || run.link >= 0;
                        const bool             strike = (run.style.flags & ChatText::Strike) != 0;
                        if (!under && !strike) continue;
                        uint32_t colour = colourOf(run.style.fg, run.link >= 0 ? linkText : text);
                        if ((run.style.flags & ChatText::Reverse) != 0) colour = colourOf(run.style.bg, background);
                        if (pc.start >= selFrom && pc.start + pc.length <= selTo) colour = selText;
                        const int px0 = left + pixelOf(pc.x);
                        const int px1 = left + pixelOf(pc.x + unitsOf(t.mid(pc.start, pc.length), run.style.flags));
                        if (under) {
                                const int uy = y + _baseline + 1;
                                fillSpan(painter, px0, px1, uy, uy + 1, colour, viewport);
                        }
                        if (strike) {
                                const int sy = y + _baseline - f.metrics().ascent / 3;
                                fillSpan(painter, px0, px1, sy, sy + 1, colour, viewport);
                        }
                }
                k = end;
        }
        return;
}

// The marker: a rule across, its words (if any) at the text's column.
void ChatView::paintMarker(Painter &painter, int y, const Rect &viewport) {
        const WindowPalette &palette = painter.palette();
        const uint32_t       colour = palette.color(Role::Highlight);
        const uint32_t       background = palette.color(Role::FaceHighlight);
        const int            left = lineLeft();
        const int            right = int(viewport.right()) - Margin;
        const int            mid = y + _rowHeight / 2;
        if (_markerWords.isEmpty()) {
                fillSpan(painter, left, right, mid, mid + 1, colour, viewport);
                return;
        }
        const int      at = left + int(_textX > 0 ? _textX : 4 * Column);
        const uint32_t w = _fonts[0].textWidth(_markerWords.view());
        fillSpan(painter, left, at - int(Column), mid, mid + 1, colour, viewport);
        (void)painter.text(at, y + _fontTop[0], _markerWords.view(), _fonts[0], colour, background,
                           uint16_t(w + Column < 0xffffu ? w + Column : 0xffffu));
        fillSpan(painter, at + int(w) + int(Column), right, mid, mid + 1, colour, viewport);
        return;
}

BMDS_NAMESPACE_END
