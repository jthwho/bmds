/**
 * @file      patchview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The patchbay's view (devplan/audio.md §24). The painter fills and draws
 * text, nothing else: a wire is three runs (across from its source, down to
 * its destination's row in its source's lane, across to it).
 */

#include <cstdio>
#include <cstring>
#include <bmds/font.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/painter.h>
#include <bmds/windowpalette.h>

#include "patchview.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr int16_t  Margin = 8;
        constexpr int16_t  HeaderHeight = 24;
        constexpr int16_t  RowHeight = 20;
        constexpr int16_t  RowGap = 2;
        constexpr int16_t  Jack = 6;  // a row's square where its wires meet it
        constexpr uint16_t Thin = 2;  // a wire
        constexpr uint16_t Thick = 4; // the selected pair's wire
        constexpr int      DisconnectAll = 1000;

        // The wires' colours, a source's each (it cycles past eight).
        constexpr uint32_t WireColors[] = {0x2f80ed, 0xeb5757, 0x27ae60, 0xf2994a,
                                           0x9b51e0, 0x00a8c6, 0xc99a06, 0xe0407b};

        int16_t column(int x) {
                return int16_t((x + 3) & ~3); // text starts on a 4-pixel column
        }

        Rect rectOf(int x, int y, int w, int h) {
                if (w < 0) {
                        x += w;
                        w = -w;
                }
                if (h < 0) {
                        y += h;
                        h = -h;
                }
                return Rect{uint16_t(x < 0 ? 0 : x), uint16_t(y < 0 ? 0 : y), uint16_t(w), uint16_t(h)};
        }

} // namespace

PatchView::PatchView(Widget *parent, GuiApplication &app, PatchSource &source)
    : Widget(parent), _app(app), _source(source), _menu(app) {
        setFocusable(true);
        _menu.setTriggered(menuChosen, this);
}

PatchView::~PatchView() {
        _source.unwatch();
        _app.cancelPosts(this);
}

Error PatchView::start() {
        const Error err = _source.watch(changed, this);
        _watching = err.isOk();
        refresh();
        return err;
}

// Any task: a refresh on the loop, one at a time.
void PatchView::changed(void *arg) {
        auto *self = static_cast<PatchView *>(arg);
        if (self->_posted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->_app.post(refreshLater, self).isError()) self->_posted.store(false, std::memory_order_release);
        return;
}

void PatchView::refreshLater(void *arg) {
        auto *self = static_cast<PatchView *>(arg);
        self->_posted.store(false, std::memory_order_release);
        self->refresh();
        return;
}

void PatchView::refresh() {
        // The selection follows its ports, not its rows.
        const int keep[2] = {_selected[Left] >= 0 ? _left[_selected[Left]].port : -1,
                             _selected[Right] >= 0 ? _right[_selected[Right]].port : -1};
        PatchSource::Port ports[MaxRows];
        const size_t      n = _source.ports(Span<PatchSource::Port>(ports, MaxRows));
        _leftCount = _rightCount = 0;
        for (size_t i = 0; i < n && i < MaxRows; ++i) {
                const PatchSource::Port &p = ports[i];
                Row                      row;
                row.port = p.number;
                std::snprintf(row.name, sizeof(row.name), "%s", p.name);
                std::snprintf(row.tag, sizeof(row.tag), "midi%u %s", unsigned(p.number), p.kind);
                if (p.source) _left[_leftCount++] = row;
                if (p.destination) _right[_rightCount++] = row;
        }
        const size_t c = _source.connections(Span<PatchSource::Connection>(_connections, MaxConnections));
        _connectionCount = c < MaxConnections ? c : MaxConnections;
        _selected[Left] = _selected[Right] = -1;
        for (size_t i = 0; i < _leftCount; ++i)
                if (_left[i].port == keep[Left]) _selected[Left] = int(i);
        for (size_t i = 0; i < _rightCount; ++i)
                if (_right[i].port == keep[Right]) _selected[Right] = int(i);
        if (_selected[Left] < 0 && _leftCount > 0) _selected[Left] = 0;
        if (_selected[Right] < 0 && _rightCount > 0) _selected[Right] = 0;
        update();
        return;
}

GuiSize PatchView::sizeHint() const {
        const size_t rows = _leftCount > _rightCount ? _leftCount : _rightCount;
        return GuiSize{560, uint16_t(HeaderHeight + (rows < 6 ? 6 : rows) * (RowHeight + RowGap) + 2 * Margin + 20)};
}

PatchView::Layout PatchView::layout() const {
        const Rect &r = geometry();
        Layout      l;
        int         w = (int(r.width) - 2 * Margin) * 3 / 8;
        if (w > 320) w = 320;
        if (w < 96) w = 96;
        l.columnWidth = uint16_t(w & ~3);
        l.leftX = column(r.x + Margin);
        l.rightX = int16_t((r.x + r.width - Margin - l.columnWidth) & ~3);
        l.top = int16_t(r.y + Margin + HeaderHeight);
        l.bandLeft = int16_t(l.leftX + l.columnWidth + Jack);
        l.bandRight = int16_t(l.rightX - Jack);
        return l;
}

int16_t PatchView::rowY(size_t index) const {
        return int16_t(layout().top + int(index) * (RowHeight + RowGap) + RowHeight / 2);
}

int PatchView::rowAt(Side side, int x, int y) const {
        const Layout l = layout();
        const int    left = side == Left ? l.leftX : l.rightX;
        if (x < left - Jack || x >= left + l.columnWidth + Jack || y < l.top) return -1;
        const int    i = (y - l.top) / (RowHeight + RowGap);
        const size_t count = side == Left ? _leftCount : _rightCount;
        return i >= 0 && size_t(i) < count ? i : -1;
}

bool PatchView::isConnected(uint8_t source, uint8_t destination) const {
        for (size_t i = 0; i < _connectionCount; ++i)
                if (_connections[i].source == source && _connections[i].destination == destination) return true;
        return false;
}

uint32_t PatchView::colorOf(size_t sourceIndex) const {
        return WireColors[sourceIndex % (sizeof(WireColors) / sizeof(WireColors[0]))];
}

void PatchView::say(const char *text) {
        std::snprintf(_message, sizeof(_message), "%s", text);
        update();
        return;
}

void PatchView::toggle(uint8_t source, uint8_t destination) {
        const bool  connected = isConnected(source, destination);
        const Error err = connected ? _source.disconnect(source, destination) : _source.connect(source, destination);
        char        text[96];
        if (err.isError())
                std::snprintf(text, sizeof(text), "Not %s: %s", connected ? "disconnected" : "connected",
                              err == Error::Full ? "no connection left" : err.desc());
        else
                text[0] = '\0';
        say(text);
        refresh();
        return;
}

void PatchView::drawWire(Painter &painter, const Layout &l, size_t from, size_t to, uint32_t color,
                         uint16_t thick) const {
        // Each source has a lane of its own across the band.
        const int span = l.bandRight - l.bandLeft;
        const int lane = l.bandLeft + span * int(from + 1) / int(_leftCount + 1);
        const int y1 = rowY(from) - thick / 2;
        const int y2 = rowY(to) - thick / 2;
        (void)painter.fill(rectOf(l.leftX + l.columnWidth, y1, lane - (l.leftX + l.columnWidth) + thick, thick), color);
        (void)painter.fill(rectOf(lane, y1, thick, y2 - y1 + (y2 >= y1 ? thick : 0)), color);
        (void)painter.fill(rectOf(lane, y2, l.rightX - lane, thick), color);
        return;
}

void PatchView::drawSide(Painter &painter, const Layout &l, Side side) const {
        const WindowPalette &pal = painter.palette();
        const Row           *rows = side == Left ? _left : _right;
        const size_t         count = side == Left ? _leftCount : _rightCount;
        const int16_t        x = side == Left ? l.leftX : l.rightX;
        for (size_t i = 0; i < count; ++i) {
                const bool     selected = int(i) == _selected[side];
                const bool     lit = selected && hasFocus() && _side == side;
                const uint32_t bg = lit        ? pal.color(Role::Highlight)
                                    : selected ? pal.color(Role::FaceLight)
                                               : pal.color(Role::Client);
                const uint32_t fg = lit ? pal.color(Role::HighlightText) : pal.color(Role::FaceText);
                const int16_t  top = int16_t(l.top + int(i) * (RowHeight + RowGap));
                (void)painter.fill(Rect{uint16_t(x), uint16_t(top), l.columnWidth, uint16_t(RowHeight)}, bg);
                const int16_t tagWidth = int16_t(Fonts::unscii16.textWidth(StringView(rows[i].tag)));
                const int16_t textY = int16_t(top + (RowHeight - 16) / 2);
                const int16_t tagX = int16_t((x + l.columnWidth - 4 - tagWidth) & ~3);
                (void)painter.text(column(x + 4), textY, StringView(rows[i].name), Fonts::unscii16, fg, bg,
                                   uint16_t(tagX - column(x + 4) - 8 > 0 ? tagX - column(x + 4) - 8 : 0));
                (void)painter.text(tagX, textY, StringView(rows[i].tag), Fonts::unscii16,
                                   lit ? fg : pal.color(Role::DisabledText), bg, uint16_t(tagWidth));
                // The jack: on the band's side.
                const int16_t jx = side == Left ? int16_t(x + l.columnWidth) : int16_t(x - Jack);
                (void)painter.fill(
                        Rect{uint16_t(jx), uint16_t(top + (RowHeight - Jack) / 2), uint16_t(Jack), uint16_t(Jack)},
                        pal.color(Role::FaceDark));
        }
        return;
}

void PatchView::paint(Painter &painter) {
        const WindowPalette &pal = painter.palette();
        const Rect          &r = geometry();
        const uint32_t       face = pal.color(Role::Face);
        (void)painter.fill(r, face);
        const Layout  l = layout();
        const int16_t headY = int16_t(r.y + Margin);
        (void)painter.text(column(l.leftX + 4), headY, "From (plays)", Fonts::unscii16, pal.color(Role::FaceText), face,
                           l.columnWidth);
        (void)painter.text(column(l.rightX + 4), headY, "To (is played)", Fonts::unscii16, pal.color(Role::FaceText),
                           face, l.columnWidth);
        drawSide(painter, l, Left);
        drawSide(painter, l, Right);
        // The wires; the selected pair's last, thick.
        const int sel[2] = {_selected[Left], _selected[Right]};
        int       selFrom = -1, selTo = -1;
        for (size_t c = 0; c < _connectionCount; ++c) {
                int from = -1, to = -1;
                for (size_t i = 0; i < _leftCount; ++i)
                        if (_left[i].port == _connections[c].source) from = int(i);
                for (size_t i = 0; i < _rightCount; ++i)
                        if (_right[i].port == _connections[c].destination) to = int(i);
                if (from < 0 || to < 0) continue;
                if (from == sel[Left] && to == sel[Right]) {
                        selFrom = from;
                        selTo = to;
                        continue;
                }
                drawWire(painter, l, size_t(from), size_t(to), colorOf(size_t(from)), Thin);
        }
        if (selFrom >= 0) drawWire(painter, l, size_t(selFrom), size_t(selTo), colorOf(size_t(selFrom)), Thick);
        // A drag under way: a wire to the pointer.
        if (_dragging && _dragRow >= 0) {
                const int      x0 = _dragSide == Left ? l.leftX + l.columnWidth : l.rightX;
                const int      y0 = rowY(size_t(_dragRow)) - 1;
                const uint32_t color = pal.color(Role::FaceDark);
                (void)painter.fill(rectOf(x0, y0, _dragX - x0, 2), color);
                (void)painter.fill(rectOf(_dragX, y0, 2, _dragY - y0), color);
        }
        // The status line: a refusal, or how to use it.
        const int16_t statusY = int16_t(r.y + r.height - Margin - 16);
        const char   *status = _message[0] != '\0' ? _message
                               : _leftCount == 0 || _rightCount == 0
                                       ? "No MIDI ports to connect"
                                       : "Drag between ports, or Enter: (dis)connect the selected pair";
        (void)painter.text(column(r.x + Margin), statusY, StringView(status), Fonts::unscii16,
                           pal.color(_message[0] != '\0' ? Role::FaceText : Role::DisabledText), face,
                           uint16_t(r.width - 2 * Margin));
        return;
}

void PatchView::mousePressed(const WindowManager::MouseEvent &event) {
        _message[0] = '\0';
        for (Side side : {Left, Right}) {
                const int row = rowAt(side, event.x, event.y);
                if (row < 0) continue;
                _selected[side] = row;
                _side = side;
                if (event.button == PointerEvent::Right) {
                        openMenu(side, row, event.x, event.y);
                } else if (event.button == PointerEvent::Left) {
                        _dragging = true;
                        _dragSide = side;
                        _dragRow = row;
                        _dragX = event.x;
                        _dragY = event.y;
                }
                break;
        }
        update();
        return;
}

void PatchView::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_dragging) return;
        _dragX = event.x;
        _dragY = event.y;
        update();
        return;
}

void PatchView::mouseReleased(const WindowManager::MouseEvent &event) {
        if (!_dragging) return;
        _dragging = false;
        const Side other = _dragSide == Left ? Right : Left;
        const int  row = rowAt(other, event.x, event.y);
        if (row >= 0) {
                _selected[other] = row;
                const uint8_t source = _dragSide == Left ? _left[_dragRow].port : _left[row].port;
                const uint8_t destination = _dragSide == Left ? _right[row].port : _right[_dragRow].port;
                toggle(source, destination);
        }
        update();
        return;
}

bool PatchView::keyPressed(const KeyEvent &event) {
        if (event.hasCtrl() || event.hasAlt()) return false;
        const size_t count = _side == Left ? _leftCount : _rightCount;
        switch (event.usage) {
                case HidKey::Up:
                        if (_selected[_side] > 0) --_selected[_side];
                        break;
                case HidKey::Down:
                        if (_selected[_side] + 1 < int(count)) ++_selected[_side];
                        break;
                case HidKey::Left: _side = Left; break;
                case HidKey::Right: _side = Right; break;
                case HidKey::Tab: _side = _side == Left ? Right : Left; break;
                case HidKey::Enter:
                case HidKey::KeypadEnter:
                case HidKey::Space:
                        if (_selected[Left] >= 0 && _selected[Right] >= 0)
                                toggle(_left[_selected[Left]].port, _right[_selected[Right]].port);
                        return true;
                case HidKey::Delete:
                case HidKey::Backspace:
                        if (_selected[Left] >= 0 && _selected[Right] >= 0 &&
                            isConnected(_left[_selected[Left]].port, _right[_selected[Right]].port))
                                toggle(_left[_selected[Left]].port, _right[_selected[Right]].port);
                        return true;
                case HidKey::Application:
                        if (_selected[_side] >= 0) {
                                const Layout l = layout();
                                openMenu(_side, _selected[_side], (_side == Left ? l.leftX : l.rightX) + 8,
                                         rowY(size_t(_selected[_side])));
                        }
                        return true;
                default: return false;
        }
        _message[0] = '\0';
        update();
        return true;
}

// A row's connections, each to end, and all of them.
void PatchView::openMenu(Side side, int index, int x, int y) {
        _menu.clear();
        _menuSide = side;
        _menuRow = index;
        _menuCount = 0;
        const uint8_t port = side == Left ? _left[index].port : _right[index].port;
        for (size_t c = 0; c < _connectionCount && _menuCount < MaxConnections; ++c) {
                const PatchSource::Connection &k = _connections[c];
                if ((side == Left ? k.source : k.destination) != port) continue;
                const uint8_t other = side == Left ? k.destination : k.source;
                const Row    *rows = side == Left ? _right : _left;
                const size_t  count = side == Left ? _rightCount : _leftCount;
                const char   *name = "?";
                for (size_t i = 0; i < count; ++i)
                        if (rows[i].port == other) name = rows[i].name;
                char text[64];
                std::snprintf(text, sizeof(text), "Disconnect %s %s", side == Left ? "to" : "from", name);
                (void)_menu.addItem(StringView(text), int(_menuCount));
                _menuItems[_menuCount++] = k;
        }
        if (_menuCount == 0) {
                (void)_menu.addItem("No connections", 0, MenuLayout::Disabled);
        } else {
                (void)_menu.addSeparator();
                (void)_menu.addItem("Disconnect All", DisconnectAll);
        }
        const GuiWindow *w = window();
        if (w == nullptr) return;
        const Rect s = w->screenRect();
        (void)_menu.popup(Rect{uint16_t(s.x + x), uint16_t(s.y + y), 0, 0});
        return;
}

void PatchView::menuChosen(void *arg, int item) {
        auto *self = static_cast<PatchView *>(arg);
        if (item == DisconnectAll) {
                for (size_t i = 0; i < self->_menuCount; ++i)
                        (void)self->_source.disconnect(self->_menuItems[i].source, self->_menuItems[i].destination);
        } else if (item >= 0 && size_t(item) < self->_menuCount) {
                (void)self->_source.disconnect(self->_menuItems[item].source, self->_menuItems[item].destination);
        }
        self->refresh();
        return;
}

BMDS_NAMESPACE_END
