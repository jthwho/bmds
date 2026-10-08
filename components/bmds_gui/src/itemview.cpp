/**
 * @file      itemview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The base of the item views: the selection, the mouse and the keys (devplan/filebrowser.md §4g).
 */

#include <bmds/guiwindow.h>
#include <bmds/itemview.h>
#include <bmds/mnemonic.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint8_t F2 = HidKey::F1 + 1;
        constexpr uint8_t F10 = HidKey::F1 + 9;

} // namespace

ItemView::ItemView(Widget *parent) : ScrollArea(parent) {
        setFocusable(true);
}

ItemView::~ItemView() {
        stopRename();
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
                if (row >= 0 && !_selection.isSelected(size_t(row))) selectOnly(row);
                menuAt(row, event.x, event.y);
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
        }
        (void)dragged(dx, dy, false);
        return;
}

void ItemView::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        if (_banding) {
                _banding = false;
                showBand(false); // the band gone
                changed();
                return;
        }
        if (_pressed < 0) return;
        const int  pressed = _pressed;
        const bool wasDragging = _dragging;
        _pressed = -1;
        _dragging = false;
        if (wasDragging) {
                (void)dragged(event.x - _pressX, event.y - _pressY, true);
                return;
        }
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
        if (_dragging) dragCancelled();
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
        return;
}

BMDS_NAMESPACE_END
