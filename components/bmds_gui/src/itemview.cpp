/**
 * @file      itemview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The base of the item views: the selection, the mouse and the keys (devplan/filebrowser.md §4g).
 */

#include <bmds/font.h>
#include <bmds/guiwindow.h>
#include <bmds/itemview.h>
#include <bmds/mnemonic.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint8_t F2 = HidKey::F1 + 1;
        constexpr uint8_t F10 = HidKey::F1 + 9;

        constexpr uint32_t BandScrollMs = 50; // a rubber band past the edge scrolls this often
        constexpr int      BandScrollMin = 4; // and at least this far

} // namespace

ItemView::ItemView(Widget *parent) : ScrollArea(parent) {
        setFocusable(true);
}

ItemView::~ItemView() {
        stopRename();
        stopBandScroll();
        stopDropTimers();
}

void ItemView::setModel(ItemModel *model) {
        _model = model;
        modelReset();
        return;
}

void ItemView::modelReset() {
        resetSelecting(-1, true);
        return;
}

void ItemView::resetTo(Span<const size_t> rows, int current) {
        resetSelecting(-1, false);
        for (const size_t row : rows)
                if (row < count() && !(_single && _selection.selectedCount() > 0)) _selection.set(row, true);
        if (current >= 0 && size_t(current) < count()) _selection.setCurrent(current);
        // Drawn whole already: remembered as now.
        for (size_t i = 0; i < _drawn.size(); ++i) _drawn[i] = _selection.word(i);
        _drawnCurrent = this->current();
        if (_changed != nullptr) _changed(_changedArg, *this);
        return;
}

void ItemView::resetSelecting(int row, bool tell) {
        stopRename();
        stopBandScroll();
        _pressed = -1;
        _pressRow = -1;
        _dragging = false;
        _banding = false;
        _editing = -1;
        (void)_selection.reset(_model != nullptr ? _model->count() : 0); // none shown without the memory
        if (row >= 0) _selection.selectOnly(row);
        relayout();
        update();
        // Drawn whole: remembered as such.
        _drawn.clear();
        const size_t words = (count() + 31) / 32;
        if (_drawn.reserve(words).isOk())
                for (size_t i = 0; i < words; ++i) (void)_drawn.append(_selection.word(i));
        _drawnCurrent = current();
        if (tell && _changed != nullptr) _changed(_changedArg, *this);
        return;
}

void ItemView::resized() {
        relayout();
        ScrollArea::resized();
        return;
}

bool ItemView::showsFocus() const {
        const GuiWindow *w = window();
        return w != nullptr && w->isActive() && hasFocus();
}

Rect ItemView::toWindow(const ContentRect &r) const {
        const Rect    v = viewport();
        const int64_t left = int64_t(v.x) + int64_t(r.x) - scrollX();
        const int64_t top = int64_t(v.y) + int64_t(r.y) - scrollY();
        const int64_t l = left > v.x ? left : v.x, t = top > v.y ? top : v.y;
        const int64_t rr = left + r.width < v.right() ? left + r.width : v.right();
        const int64_t b = top + r.height < v.bottom() ? top + r.height : v.bottom();
        if (rr <= l || b <= t) return Rect();
        return Rect{uint16_t(l), uint16_t(t), uint16_t(rr - l), uint16_t(b - t)};
}

void ItemView::updateRow(size_t row) {
        GuiWindow *w = window();
        const Rect r = row < count() ? toWindow(rowRect(row)) : Rect();
        if (w != nullptr && !r.isEmpty()) w->invalidate(r);
        return;
}

void ItemView::changed() {
        // Only the rows whose look changed (a full redraw flickers: the screen is drawn on as it shows).
        GuiWindow   *w = window();
        const size_t words = (count() + 31) / 32;
        if (w != nullptr) {
                if (_drawn.size() != words) {
                        w->invalidate(viewport());
                } else {
                        for (size_t i = 0; i < words; ++i) {
                                const uint32_t diff = _drawn[i] ^ _selection.word(i);
                                for (uint32_t b = 0; diff != 0 && b < 32; ++b)
                                        if ((diff & (1u << b)) != 0) updateRow(i * 32 + b);
                        }
                        if (_drawnCurrent != current()) {
                                if (_drawnCurrent >= 0) updateRow(size_t(_drawnCurrent));
                                if (current() >= 0) updateRow(size_t(current()));
                        }
                }
        }
        // Remembered as drawn now (all of the viewport if it can't be).
        _drawn.clear();
        if (_drawn.reserve(words).isOk())
                for (size_t i = 0; i < words; ++i) (void)_drawn.append(_selection.word(i));
        _drawnCurrent = current();
        if (_changed != nullptr) _changed(_changedArg, *this);
        return;
}

void ItemView::selectOnly(int row) {
        _selection.selectOnly(row);
        if (row >= 0) showRow(size_t(row));
        changed();
        return;
}

void ItemView::setSelected(size_t row, bool selected) {
        if (_single && selected) _selection.clear();
        _selection.set(row, selected);
        changed();
        return;
}

void ItemView::setCurrent(int row) {
        _selection.setCurrent(row);
        changed();
        return;
}

void ItemView::showRow(size_t row) {
        if (row >= count()) return;
        const ContentRect r = rowRect(row);
        ensureVisible(r.x, r.y, r.width, r.height);
        return;
}

void ItemView::setEditing(int row) {
        if (row == _editing) return;
        if (_editing >= 0) updateRow(size_t(_editing));
        _editing = row;
        if (_editing >= 0) updateRow(size_t(_editing));
        return;
}

size_t ItemView::pageNeighbour(size_t row, int pages) const {
        // As many arrows as a page's height holds rows: walked one at a time.
        const ContentRect r = rowRect(row);
        const uint32_t    page = viewport().height;
        size_t            to = row;
        for (int p = 0; p < (pages < 0 ? -pages : pages); ++p) {
                const uint32_t start = rowRect(to).y;
                for (;;) {
                        const size_t next = neighbour(to, 0, pages < 0 ? -1 : 1);
                        if (next == to) break;
                        const uint32_t y = rowRect(next).y;
                        if ((pages < 0 ? start - y : y - start) > page - r.height) break;
                        to = next;
                }
        }
        return to;
}

// A Shift range to @p row, by the view's rule (`inRange()`), added to the selection with @p add.
void ItemView::selectRangeTo(size_t row, bool add) {
        _selection.selectRange(
                row, add,
                [](const void *arg, size_t r, size_t anchor, size_t to) {
                        return static_cast<const ItemView *>(arg)->inRange(r, anchor, to);
                },
                this);
        return;
}

void ItemView::activate(size_t row) {
        if (_activated != nullptr) _activated(_activatedArg, *this, row);
        return;
}

// ---- The mouse

void ItemView::mousePressed(const WindowManager::MouseEvent &event) {
        stopRename(); // a second press: a double click, not a slow one
        const Rect v = viewport();
        if (event.x < v.x || event.y < v.y || event.x >= v.right() || event.y >= v.bottom()) return;
        const uint32_t cx = contentX(event.x), cy = contentY(event.y);
        const int      row = rowAt(cx, cy);
        if (event.button == PointerEvent::Right) {
                // Its menu at the release, unless it drags first (a right drag asks at the drop).
                if (row >= 0 && !_selection.isSelected(size_t(row))) selectOnly(row);
                _rightPressed = true;
                _rightRow = row;
                _pressX = event.x;
                _pressY = event.y;
                _dragging = false;
                return;
        }
        if (event.button != PointerEvent::Left) return;
        const bool     ctrl = (event.modifiers & KeyEvent::Ctrl) != 0 && !_single;
        const bool     shift = (event.modifiers & KeyEvent::Shift) != 0 && !_single;
        const uint64_t now = System::uptimeUs();
        if (row >= 0 && row == _pressRow && !ctrl && now - _pressUs <= uint64_t(WindowManager::DoubleClickMs) * 1000) {
                // Shift too: the owner may open it otherwise (a folder in a new window).
                _pressUs = 0; // a third press starts again
                _pressRow = -1;
                _activation = event.modifiers;
                activate(size_t(row));
                _activation = 0;
                return;
        }
        _pressUs = now;
        _pressRow = row;
        _pressX = event.x;
        _pressY = event.y;
        _dragging = false;
        _onlyOnRelease = false;
        _onSelectedName = false;
        if (row < 0) {
                // No row: a rubber band (adding to the selection with Ctrl); without Ctrl, nothing is
                // current either (no focus mark left behind).
                if (!ctrl) _selection.selectOnly(-1);
                _pressed = -1;
                _banding = !_single;
                _bandAdds = ctrl;
                _bandX = cx;
                _bandY = cy;
                _band = ContentRect();
                changed();
                return;
        }
        const bool wasOnly = _selection.selectedCount() == 1 && _selection.isSelected(size_t(row));
        if (shift) {
                selectRangeTo(size_t(row), ctrl);
        } else if (ctrl) {
                _selection.toggle(size_t(row));
        } else if (_selection.isSelected(size_t(row))) {
                _selection.setCurrent(row);
                _onlyOnRelease = _selection.selectedCount() > 1;
                _onSelectedName = wasOnly && onName(size_t(row), cx, cy); // a slow second click renames
        } else {
                _selection.selectOnly(row);
        }
        _pressed = row;
        changed();
        if (_selection.selectedCount() == 1) startRename(row, LongPressMs); // held still: a rename
        return;
}

void ItemView::mouseMoved(const WindowManager::MouseEvent &event) {
        if (_rightPressed && (event.buttons & PointerEvent::Right) != 0 && !_dragging) {
                const int dx = event.x - _pressX, dy = event.y - _pressY;
                if (dx * dx + dy * dy > DragDistance * DragDistance && _rightRow >= 0) {
                        _dragging = true;
                        beginDrag();
                }
                return;
        }
        if ((event.buttons & PointerEvent::Left) == 0) return;
        if (_banding) {
                band(event.x, event.y);
                return;
        }
        if (_pressed < 0) return;
        const int dx = event.x - _pressX, dy = event.y - _pressY;
        if (!_dragging) {
                if (dx * dx + dy * dy <= DragDistance * DragDistance) return;
                stopRename();
                _dragging = true;
                _pressUs = 0; // no double click after a drag
                _onSelectedName = false;
                beginDrag();
        }
        return;
}

void ItemView::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button == PointerEvent::Right && _rightPressed) {
                _rightPressed = false;
                if (!_dragging) menuAt(_rightRow, event.x, event.y);
                _dragging = false;
                return;
        }
        if (event.button != PointerEvent::Left) return;
        if (_banding) {
                _banding = false;
                stopBandScroll();
                showBand(false); // the band gone
                changed();
                return;
        }
        if (_pressed < 0) return;
        const int  pressed = _pressed;
        const bool wasDragging = _dragging;
        _pressed = -1;
        _dragging = false;
        if (wasDragging) return; // no drag started (none asked for): a release that does nothing
        stopRename(); // released before the long press
        if (_onlyOnRelease) {
                _selection.selectOnly(pressed);
                changed();
        }
        // A slow second click on its name: renamed once a double click can't come.
        if (_onSelectedName) startRename(pressed, WindowManager::DoubleClickMs + 50);
        _onSelectedName = false;
        return;
}

void ItemView::mouseLeft() {
        stopRename();
        stopBandScroll();
        _rightPressed = false;
        if (_banding) {
                showBand(false);
                changed();
        }
        _banding = false;
        _pressed = -1;
        _dragging = false;
        return;
}

// The band from its corner to window point (@p x, @p y): the rows it touches selected.
void ItemView::band(int x, int y) {
        const Rect     v = viewport();
        const int      cx = x < v.x ? v.x : x >= v.right() ? v.right() - 1 : x;
        const int      cy = y < v.y ? v.y : y >= v.bottom() ? v.bottom() - 1 : y;
        const uint32_t ex = contentX(cx), ey = contentY(cy);
        _band = ContentRect{_bandX < ex ? _bandX : ex, _bandY < ey ? _bandY : ey,
                            (_bandX < ex ? ex - _bandX : _bandX - ex) + 1,
                            (_bandY < ey ? ey - _bandY : _bandY - ey) + 1};
        // Where it was and where it is redrawn (its edges move), then exactly the rows it touches;
        // with Ctrl, those added to what was selected.
        showBand(true);
        if (!_bandAdds) _selection.clear();
        for (size_t r = 0; r < count(); ++r)
                if (rowRect(r).intersects(_band)) _selection.set(r, true);
        changed();
        // Past the viewport's edge, the view scrolls while the pointer stays there (as Windows'):
        // a timer only then, as a scroll bar's arrow held.
        _bandPointerX = x;
        _bandPointerY = y;
        const bool past = x < v.x || x >= int(v.right()) || y < v.y || y >= int(v.bottom());
        GuiWindow *w = window();
        if (!past) {
                stopBandScroll();
        } else if (_bandTimer < 0 && w != nullptr) {
                auto [timer, err] = w->application().startTimer(BandScrollMs, true, bandDue, this);
                if (err.isOk()) _bandTimer = timer;
        }
        return;
}

void ItemView::stopBandScroll() {
        GuiWindow *w = _bandTimer >= 0 ? window() : nullptr;
        if (w != nullptr) w->application().stopTimer(_bandTimer);
        _bandTimer = -1;
        return;
}

// A tick with the pointer past the edge: scrolled toward it, further the further past it is (at
// least a few pixels), the band following.
void ItemView::bandDue(void *arg) {
        auto *self = static_cast<ItemView *>(arg);
        if (!self->_banding) {
                self->stopBandScroll();
                return;
        }
        const Rect v = self->viewport();
        const int  x = self->_bandPointerX, y = self->_bandPointerY;
        const auto past = [](int p, int low, int high) {
                return p < low ? p - low : p >= high ? p - high + 1 : 0;
        };
        const auto step = [](int d) {
                const int at = d == 0 ? 0
                                      : (d < 0 ? (d > -BandScrollMin ? -BandScrollMin : d)
                                               : (d < BandScrollMin ? BandScrollMin : d));
                return at < 0 ? -((-at + 3) & ~3) : (at + 3) & ~3; // whole columns across (as text needs)
        };
        self->scrollTo(int64_t(self->scrollX()) + step(past(x, v.x, int(v.right()))),
                       int64_t(self->scrollY()) + step(past(y, v.y, int(v.bottom()))));
        self->band(x, y);
        return;
}

// The band shown by the window manager over the view (as a move's outline: drawn over what the
// view draws, no redraw of the view's own to move it), or hidden.
void ItemView::showBand(bool shown) {
        GuiWindow *w = window();
        if (w == nullptr || !w->isShown()) return;
        (void)w->application().windowManager().setBand(w->id(), shown ? toWindow(_band) : Rect());
        return;
}

void ItemView::menuAt(int row, int x, int y) {
        if (_menuAsked == nullptr) return;
        GuiWindow *w = window();
        const Rect s = w != nullptr ? w->screenRect() : Rect();
        _menuAsked(_menuArg, *this, row, uint16_t(s.x + x), uint16_t(s.y + y));
        return;
}

// ---- Renaming by a slow click or a long press

void ItemView::startRename(int row, uint32_t ms) {
        stopRename();
        GuiWindow *w = window();
        if (_renameAsked == nullptr || row < 0 || w == nullptr) return;
        auto [timer, err] = w->application().startTimer(ms, false, renameDue, this);
        if (err.isError()) return;
        _renameTimer = timer;
        _renameRow = row;
        return;
}

void ItemView::stopRename() {
        GuiWindow *w = _renameTimer >= 0 ? window() : nullptr;
        if (w != nullptr) w->application().stopTimer(_renameTimer);
        _renameTimer = -1;
        _renameRow = -1;
        return;
}

void ItemView::renameDue(void *arg) {
        auto     *self = static_cast<ItemView *>(arg);
        const int row = self->_renameRow;
        self->_renameTimer = -1; // fired: its number is free
        self->_renameRow = -1;
        if (self->_dragging) return;
        self->_pressed = -1; // a long press: its release does nothing now
        self->_onlyOnRelease = false;
        if (row >= 0 && size_t(row) < self->count() && self->_renameAsked != nullptr)
                self->_renameAsked(self->_renameArg, *self, size_t(row));
        return;
}

// ---- The keys

void ItemView::moveTo(size_t row, bool shift, bool ctrl) {
        stopRename();
        if (shift && !_single) {
                selectRangeTo(row, ctrl);
        } else if (ctrl && !_single) {
                _selection.setCurrent(int(row));
        } else {
                _selection.selectOnly(int(row));
        }
        showRow(row);
        changed();
        return;
}

int ItemView::startingWith(uint32_t key) const {
        const size_t n = count();
        size_t       start = 0;
        for (size_t k = 0; k < n; ++k)
                if (int(typingOrder(k)) == current()) start = k + 1;
        for (size_t k = 0; k < n && _model != nullptr; ++k) {
                const size_t     row = typingOrder((start + k) % n);
                const StringView name = _model->text(row);
                if (!name.isEmpty() && Mnemonic::fold(uint8_t(name[0])) == key) return int(row);
        }
        return -1;
}

bool ItemView::keyPressed(const KeyEvent &event) {
        const bool   plain = !event.hasCtrl() && !event.hasAlt();
        const bool   shift = event.hasShift(), ctrl = event.hasCtrl() && !event.hasAlt();
        const size_t n = count();
        if (n > 0 && !event.hasAlt()) {
                const size_t from = current() >= 0 ? size_t(current()) : 0;
                const bool   none = current() < 0;
                long         to = -1;
                switch (event.usage) {
                        case HidKey::Left: to = long(none ? endRow(false) : neighbour(from, -1, 0)); break;
                        case HidKey::Right: to = long(none ? endRow(false) : neighbour(from, 1, 0)); break;
                        case HidKey::Up: to = long(none ? endRow(false) : neighbour(from, 0, -1)); break;
                        case HidKey::Down: to = long(none ? endRow(false) : neighbour(from, 0, 1)); break;
                        case HidKey::PageUp: to = long(pageNeighbour(from, -1)); break;
                        case HidKey::PageDown: to = long(pageNeighbour(from, 1)); break;
                        case HidKey::Home: to = long(endRow(false)); break;
                        case HidKey::End: to = long(endRow(true)); break;
                        default: break;
                }
                if (to >= 0) {
                        moveTo(size_t(to), shift, ctrl);
                        return true;
                }
        }
        if (ctrl && event.usage == HidKey::Space && current() >= 0 && !_single) {
                _selection.toggle(size_t(current()));
                changed();
                return true;
        }
        if (ctrl && event.usage == HidKey::A && !_single) {
                _selection.selectAll();
                changed();
                return true;
        }
        if (plain && (event.usage == HidKey::Enter || event.usage == HidKey::KeypadEnter)) {
                _activation = event.modifiers; // Shift+Enter: as Shift and a double click
                if (current() >= 0) activate(size_t(current()));
                _activation = 0;
                return true;
        }
        if (plain && event.usage == F2) {
                if (current() >= 0 && _renameAsked != nullptr) _renameAsked(_renameArg, *this, size_t(current()));
                return true;
        }
        if (plain && event.usage == HidKey::Escape && _selection.selectedCount() > 0) {
                _selection.clear();
                changed();
                return true;
        }
        if (event.usage == HidKey::Application || (event.usage == F10 && shift && plain)) {
                // At the current row's middle, or the viewport's corner.
                const Rect r = current() >= 0 ? toWindow(rowRect(size_t(current()))) : Rect();
                const Rect v = viewport();
                menuAt(current() >= 0 && _selection.isSelected(size_t(current())) ? current() : -1,
                       r.isEmpty() ? v.x : r.x + r.width / 2, r.isEmpty() ? v.y : r.y + r.height / 2);
                return true;
        }
        if (_keyTyped != nullptr && _keyTyped(_keyArg, *this, event)) return true;
        // A letter: the next row named with it.
        if (plain && event.codepoint > 0x20 && event.codepoint != 0x7f && event.usage != HidKey::Tab) {
                const int to = startingWith(Mnemonic::fold(event.codepoint));
                if (to >= 0) moveTo(size_t(to), false, false);
                return true;
        }
        return false;
}

namespace {

        Rect cutTo(int x, int y, int w, int h, const Rect &clip) {
                const int l = x > clip.x ? x : clip.x, t = y > clip.y ? y : clip.y;
                const int r = x + w < int(clip.right()) ? x + w : int(clip.right());
                const int b = y + h < int(clip.bottom()) ? y + h : int(clip.bottom());
                if (r <= l || b <= t) return Rect();
                return Rect{uint16_t(l), uint16_t(t), uint16_t(r - l), uint16_t(b - t)};
        }

} // namespace

uint32_t ItemView::drawLabel(Painter &painter, const Rect &clip, int x, int y, int width, bool selected, bool focus,
                             bool current, uint32_t background) {
        (void)current;
        if (!selected) return background;
        const uint32_t bg = painter.palette().color(focus ? Role::Highlight : Role::Face);
        const Rect     r = cutTo(x, y + 1, width, GuiStyle::TextHeight, clip);
        if (!r.isEmpty()) (void)painter.fill(r, bg);
        return bg;
}

void ItemView::focusRect(Painter &painter, const Rect &clip, int x, int y, int width) {
        // The rows above and below the text's (an 18-pixel row), and its sides.
        const int  h = GuiStyle::TextHeight + 2;
        const Rect edges[] = {cutTo(x, y, width, 1, clip), cutTo(x, y + h - 1, width, 1, clip),
                              cutTo(x, y + 1, 1, h - 2, clip), cutTo(x + width - 1, y + 1, 1, h - 2, clip)};
        for (const Rect &e : edges)
                if (!e.isEmpty()) (void)painter.fill(e, painter.palette().color(Role::FaceShadow));
        return;
}

void ItemView::paintContent(Painter &painter, const Rect &viewport) {
        paintRows(painter, viewport); // the band is the window manager's (showBand())
        if (_dropRow >= 0 && size_t(_dropRow) < count()) {
                // A drag's target row: framed in the highlight, two pixels.
                const Rect     r = toWindow(rowRect(size_t(_dropRow)));
                const uint32_t c = painter.palette().color(Role::Highlight);
                if (r.width >= 4 && r.height >= 4) {
                        const Rect edges[] = {
                                Rect{r.x, r.y, r.width, 2}, Rect{r.x, uint16_t(r.bottom() - 2), r.width, 2},
                                Rect{r.x, uint16_t(r.y + 2), 2, uint16_t(r.height - 4)},
                                Rect{uint16_t(r.right() - 2), uint16_t(r.y + 2), 2, uint16_t(r.height - 4)}};
                        for (const Rect &e : edges) (void)painter.fill(e.intersected(viewport), c);
                }
        }
        return;
}

// ---- Drag and drop (devplan/clipboard.md §5d)

DragImage::Box ItemView::rowBox(size_t row) const {
        const Rect        v = viewport();
        const ContentRect c = rowRect(row);
        return DragImage::Box{int(v.x) + int(c.x) - int(scrollX()), int(v.y) + int(c.y) - int(scrollY()), int(c.width),
                              int(c.height)};
}

void ItemView::drawDragRow(DragPicture &picture, size_t row, const WindowPalette &palette) const {
        if (_model == nullptr) return;
        const DragImage::Box b = rowBox(row);
        const Icon          *icon = _model->icon(row, 16);
        if (icon != nullptr && icon->isValid()) picture.icon(b.x + 2, b.y + 1, *icon);
        const StringView name = _model->text(row);
        const int        w = int(Fonts::unscii16.textWidth(name));
        const int        room = b.width - 22 > 0 ? b.width - 22 : 0;
        picture.fill(b.x + 20, b.y + 1, (w < room ? w : room) + 4, GuiStyle::TextHeight,
                     palette.color(Role::Highlight));
        picture.text(b.x + 22, b.y + 1, name, palette.color(Role::HighlightText), uint16_t(room));
        return;
}

// The press moved past the drag distance: the owner's drag, with the selection's image.
void ItemView::beginDrag() {
        GuiWindow *w = window();
        const int  row = _rightPressed ? _rightRow : _pressed;
        if (w == nullptr || _dragAsked == nullptr || row < 0 || size_t(row) >= count()) return;
        GuiDrag drag;
        if (!_dragAsked(_dragArg, *this, drag)) return;
        const WindowPalette  palette = w->application().windowManager().palette();
        const DragImage::Box box = DragImage::around(rowBox(size_t(row)), _pressX, _pressY);
        DragPicture          picture(box);
        if (picture.isValid()) {
                size_t total = 0;
                for (int r = _selection.nextSelected(0); r >= 0; r = _selection.nextSelected(size_t(r) + 1)) {
                        ++total;
                        if (DragImage::holds(box, rowBox(size_t(r)))) drawDragRow(picture, size_t(r), palette);
                }
                if (total > 1) {
                        const DragImage::Box p = rowBox(size_t(row));
                        const int right = p.x + p.width < box.x + box.width ? p.x + p.width : box.x + box.width;
                        picture.badge(right, p.y > box.y ? p.y : box.y, total);
                }
                if (picture.crop(_pressX, _pressY)) {
                        drag.image = picture.pixels();
                        drag.width = picture.width();
                        drag.height = picture.height();
                        drag.hotX = picture.hotX();
                        drag.hotY = picture.hotY();
                }
        }
        (void)w->startDrag(drag, this); // refused: the press was over already
        return;
}

// The row under window point (@p x, @p y), or -1 (the background, or outside the viewport).
int ItemView::dropRowAt(int x, int y) const {
        const Rect v = viewport();
        if (x < v.x || y < v.y || x >= v.right() || y >= v.bottom()) return -1;
        return rowAt(contentX(x), contentY(y));
}

void ItemView::setDropRow(int row) {
        if (row == _dropRow) return;
        if (_dropRow >= 0 && size_t(_dropRow) < count()) updateRow(size_t(_dropRow));
        _dropRow = row;
        if (row >= 0 && size_t(row) < count()) updateRow(size_t(row));
        return;
}

void ItemView::dragEntered(GuiDropEvent &event) {
        dragMoved(event);
        return;
}

void ItemView::dragMoved(GuiDropEvent &event) {
        int              row = dropRowAt(event.x(), event.y());
        const DropAction a = _dropAsked != nullptr ? _dropAsked(_dropArg, *this, row, event) : DropAction::None;
        event.accept(a);
        setDropRow(a != DropAction::None ? row : -1);
        dropScroll(event.x(), event.y());
        // Held over the same row: told after a while (a tree opens it).
        GuiWindow *w = window();
        const int  held = a != DropAction::None ? row : dropRowAt(event.x(), event.y());
        if (held != _dropHoldRow && w != nullptr) {
                if (_dropHoldTimer >= 0) w->application().stopTimer(_dropHoldTimer);
                _dropHoldTimer = -1;
                _dropHoldRow = held;
                if (held >= 0) {
                        auto [timer, err] = w->application().startTimer(DropHoldMs, false, dropHoldDue, this);
                        if (err.isOk()) _dropHoldTimer = timer;
                }
        }
        return;
}

void ItemView::dragLeft() {
        stopDropTimers();
        setDropRow(-1);
        return;
}

bool ItemView::dropped(GuiDropEvent &event) {
        stopDropTimers();
        int              row = dropRowAt(event.x(), event.y());
        const DropAction a = _dropAsked != nullptr ? _dropAsked(_dropArg, *this, row, event) : DropAction::None;
        setDropRow(-1);
        if (a == DropAction::None || _dropped == nullptr) return false;
        return _dropped(_dropArg, *this, row, event);
}

// A drag near the viewport's edge: scrolled toward it while it stays (a timer, as the band's).
void ItemView::dropScroll(int x, int y) {
        _dropX = x;
        _dropY = y;
        const Rect v = viewport();
        const bool near = x < v.x + DropScrollZone || x >= int(v.right()) - DropScrollZone ||
                          y < v.y + DropScrollZone || y >= int(v.bottom()) - DropScrollZone;
        GuiWindow *w = window();
        if (!near) {
                if (_dropScrollTimer >= 0 && w != nullptr) w->application().stopTimer(_dropScrollTimer);
                _dropScrollTimer = -1;
        } else if (_dropScrollTimer < 0 && w != nullptr) {
                auto [timer, err] = w->application().startTimer(BandScrollMs, true, dropScrollDue, this);
                if (err.isOk()) _dropScrollTimer = timer;
        }
        return;
}

void ItemView::dropScrollDue(void *arg) {
        auto         *self = static_cast<ItemView *>(arg);
        const Rect    v = self->viewport();
        const int     x = self->_dropX, y = self->_dropY;
        const int     dx = x < v.x + DropScrollZone ? -16 : x >= int(v.right()) - DropScrollZone ? 16 : 0;
        const int     dy = y < v.y + DropScrollZone ? -16 : y >= int(v.bottom()) - DropScrollZone ? 16 : 0;
        const int64_t sx = int64_t(self->scrollX()) + dx, sy = int64_t(self->scrollY()) + dy;
        self->scrollTo(sx < 0 ? 0 : sx, sy < 0 ? 0 : sy);
        return;
}

void ItemView::dropHoldDue(void *arg) {
        auto     *self = static_cast<ItemView *>(arg);
        const int row = self->_dropHoldRow;
        self->_dropHoldTimer = -1; // fired: its number is free
        if (row >= 0 && size_t(row) < self->count()) self->dropHeld(size_t(row));
        return;
}

void ItemView::stopDropTimers() {
        GuiWindow *w = window();
        if (w != nullptr) {
                if (_dropScrollTimer >= 0) w->application().stopTimer(_dropScrollTimer);
                if (_dropHoldTimer >= 0) w->application().stopTimer(_dropHoldTimer);
        }
        _dropScrollTimer = -1;
        _dropHoldTimer = -1;
        _dropHoldRow = -1;
        return;
}

BMDS_NAMESPACE_END
