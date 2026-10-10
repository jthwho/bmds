/**
 * @file      notifyarea.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system tray (devplan/mediaplayer.md §6e): its service, taking the programs' icons; and the
 * panel's notification area, showing them and the clock in one sunken field.
 */

#include <cstring>
#include <new>
#include <bmds/bevel.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/font.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

#include "notifyarea.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint64_t DoubleClickNs = uint64_t(WindowManager::DoubleClickMs) * 1000000u;

        // Text and icons are drawn from a column (GuiStyle::Column pixels): @p x down to one.
        int column(int x) {
                return x & ~int(GuiStyle::Column - 1);
        }

} // namespace

// ---- TrayService

Error TrayService::start() {
        if (_running) return Error::Ok;
        Error err = _app.registerService(TrayProtocol::Service, message, this);
        if (err.isOk()) {
                err = _app.watchServices(servicesChanged, this);
                if (err.isError()) (void)_app.unregisterService(TrayProtocol::Service);
        }
        _running = err.isOk();
        return err;
}

void TrayService::stop() {
        if (!_running) return;
        _app.unwatchServices(servicesChanged, this);
        (void)_app.unregisterService(TrayProtocol::Service);
        _running = false;
        _row.clear();
        changed();
        return;
}

void TrayService::setArea(NotifyArea *area) {
        _area = area;
        if (_area != nullptr) _area->refresh();
        return;
}

void TrayService::changed() {
        if (_area == nullptr) return;
        _area->refresh(); // its size: the row laid out again, then told where (moved())
        moved();
        return;
}

// A program's message (the loop's thread).
void TrayService::message(void *arg, ByteSpan message) {
        auto *self = static_cast<TrayService *>(arg);
        switch (TrayProtocol::Kind(TrayProtocol::kindOf(message))) {
                case TrayProtocol::Kind::Update: {
                        TrayProtocol::Update u;
                        bool                 moved = false;
                        if (!TrayProtocol::read(message, u) || self->_row.update(u, &moved).isError()) break;
                        if (moved) {
                                self->changed();
                        } else if (self->_area != nullptr) {
                                // Only its look or tooltip (a player's, each second): the pointer's place kept.
                                self->_area->iconChanged(self->_row.positionOf(TrayProtocol::view(u.reply), u.id));
                        }
                        break;
                }
                case TrayProtocol::Kind::Pixels: {
                        // 1 KB: off the loop's stack.
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(TrayProtocol::Pixels));
                        if (mem == nullptr) break;
                        auto *p = ::new (mem) TrayProtocol::Pixels();
                        if (TrayProtocol::read(message, *p) && self->_row.setPixels(*p).isOk() &&
                            self->_area != nullptr)
                                self->_area->iconChanged(self->_row.positionOf(TrayProtocol::view(p->reply), p->id));
                        Memory::free(mem);
                        break;
                }
                case TrayProtocol::Kind::Remove: {
                        TrayProtocol::Header h;
                        if (TrayProtocol::read(message, h) && self->_row.remove(TrayProtocol::view(h.reply), h.id))
                                self->changed();
                        break;
                }
                default: break; // a kind from a later version
        }
        return;
}

bool TrayService::gone(void *arg, StringView reply) {
        return !static_cast<TrayService *>(arg)->_app.hasService(reply);
}

// A program gone takes its reply service with it: its icons go too.
void TrayService::servicesChanged(void *arg) {
        auto *self = static_cast<TrayService *>(arg);
        if (self->_row.dropGone(gone, self) > 0) self->changed();
        return;
}

Rect TrayService::screenRect(size_t position) const {
        return _area != nullptr ? _area->cellOnScreen(position) : Rect();
}

void TrayService::send(const TrayRow::Entry &entry, TrayProtocol::Kind kind, TrayProtocol::Reason reason, int notches) {
        TrayProtocol::Event e;
        e.kind = kind;
        e.id = entry.id;
        e.reason = reason;
        e.notches = notches;
        const int  position = _row.positionOf(TrayProtocol::view(entry.reply), entry.id);
        const Rect r = position >= 0 ? screenRect(size_t(position)) : Rect();
        e.x = int16_t(r.x);
        e.y = int16_t(r.y);
        e.width = r.width;
        e.height = r.height;
        (void)_app.windowManager().sendToService(TrayProtocol::view(entry.reply), TrayProtocol::bytes(e));
        return;
}

void TrayService::activate(size_t position, TrayProtocol::Reason reason) {
        const TrayRow::Entry *e = _row.shown(position);
        if (e != nullptr) send(*e, TrayProtocol::Kind::Activated, reason, 0);
        return;
}

void TrayService::scroll(size_t position, int notches) {
        const TrayRow::Entry *e = _row.shown(position);
        if (e != nullptr) send(*e, TrayProtocol::Kind::Scrolled, TrayProtocol::Reason::Trigger, notches);
        return;
}

void TrayService::moved() {
        for (size_t p = 0;; ++p) {
                const TrayRow::Entry *e = _row.shown(p);
                if (e == nullptr) break;
                send(*e, TrayProtocol::Kind::Geometry, TrayProtocol::Reason::Trigger, 0);
        }
        return;
}

// ---- NotifyArea

NotifyArea::NotifyArea(Widget *parent, BoxLayout &row, GuiApplication &app, TrayService &tray, Parts parts)
    : Widget(parent), _row(row), _app(app), _tray(tray), _parts(parts) {
        if (hasTray()) _tray.setArea(this);
        if (hasClock()) (void)configureClock(StringView(), StringView(), StringView());
}

NotifyArea::~NotifyArea() {
        if (_clockTimer >= 0) _app.stopTimer(_clockTimer);
        if (_movedPosted) _app.cancelPosts(this);
        if (hasTray()) _tray.setArea(nullptr);
}

// ---- Its parts

Rect NotifyArea::trayRect() const {
        const Rect     in = Bevel::inside(geometry(), Bevel::Style::Status);
        const uint16_t w = trayWidth();
        const uint16_t x = _parts == Parts::ClockTray ? uint16_t(in.right() - w) : in.x;
        return Rect{x, in.y, w, in.height};
}

Rect NotifyArea::clockRect() const {
        if (!hasClock()) return Rect();
        const Rect     in = Bevel::inside(geometry(), Bevel::Style::Status);
        const uint16_t tray = trayWidth();
        const uint16_t w = in.width > tray ? uint16_t(in.width - tray) : 0;
        const uint16_t x = _parts == Parts::TrayClock ? uint16_t(in.x + tray) : in.x;
        return Rect{x, in.y, w, in.height};
}

int NotifyArea::iconAt(int x) const {
        if (!hasTray()) return -1;
        return _tray.row().positionAt(x - trayRect().x - TrayLead);
}

GuiSize NotifyArea::sizeHint() const {
        const uint16_t edges = uint16_t(2 * Bevel::width(Bevel::Style::Status));
        return GuiSize{uint16_t(edges + trayWidth() + (hasClock() ? clockWidth() : 0)), GuiStyle::ButtonHeight};
}

// The time and its room: a character's each side, half of it by the icons (the cells' own edges
// make up the rest).
uint16_t NotifyArea::clockWidth() const {
        const uint16_t outer = Fonts::unscii16.width();
        const uint16_t near = trayWidth() != 0 ? uint16_t(GuiStyle::Column) : outer;
        return uint16_t((outer + _textWidth + near + GuiStyle::Column - 1) & ~uint32_t(GuiStyle::Column - 1));
}

Rect NotifyArea::cellOnScreen(size_t position) const {
        GuiWindow *w = window();
        if (w == nullptr) return Rect();
        const Rect s = w->screenRect();
        if (s.width == 0) return Rect();
        const Rect t = trayRect();
        return Rect{uint16_t(s.x + t.x + TrayLead + position * TrayRow::CellWidth), uint16_t(s.y + t.y),
                    TrayRow::CellWidth, t.height};
}

// ---- The clock

Error NotifyArea::configureClock(StringView format, StringView tipFormat, StringView zone) {
        if (format.size() >= sizeof(_format) || tipFormat.size() >= sizeof(_tipFormat)) return Error::BufferTooSmall;
        if (!format.isEmpty()) {
                std::memcpy(_format, format.data(), format.size());
                _format[format.size()] = '\0';
        }
        if (!tipFormat.isEmpty()) {
                std::memcpy(_tipFormat, tipFormat.data(), tipFormat.size());
                _tipFormat[tipFormat.size()] = '\0';
        }
        if (!zone.isEmpty()) {
                Result<TimeZone> rules = TimeZone::isName(zone) ? Vfs::zoneRules(zone) : TimeZone::parse(zone);
                if (rules.error().isError()) return rules.error();
                _zone = rules.value();
                _hasZone = true;
        }
        const StringView f(_format);
        _seconds = f.contains(StringView("%S")) || f.contains(StringView("%T")) || f.contains(StringView("%s")) ||
                   f.contains(StringView("%r"));
        tick();
        // Wide enough for any time: a sample's width (padded fields keep it).
        _textWidth = uint16_t(Fonts::unscii16.textWidth(_time.view()));
        return Error();
}

void NotifyArea::clockDue(void *arg) {
        static_cast<NotifyArea *>(arg)->tick();
        return;
}

void NotifyArea::tick() {
        const Timestamp now = Clock::now();
        const LocalTime local = _hasZone ? _zone.localTime(now) : Clock::localTime(now);
        String          text(MemSpace::Psram);
        if (formatTime(text, StringView(_format), local).isOk() && text.view() != _time.view()) {
                (void)_time.assign(text.view());
                update();
        }
        text.clear();
        if (formatTime(text, StringView(_tipFormat), local).isOk() && text.view() != _date.view()) {
                (void)_date.assign(text.view()); // a new day
                if (_hovered == ClockHovered) _app.toolTipChanged(this);
        }
        // The next change: the next second or minute's start.
        const int64_t ms = now.ns / 1'000'000;
        const int64_t period = _seconds ? 1000 : 60'000;
        const int64_t wait = period - ((ms % period) + period) % period + 5;
        if (_clockTimer >= 0) _app.stopTimer(_clockTimer);
        auto [timer, err] = _app.startTimer(static_cast<uint32_t>(wait), false, clockDue, this);
        _clockTimer = err.isOk() ? timer : -1;
        return;
}

// ---- The icons

void NotifyArea::iconChanged(int position) {
        if (position < 0) return;
        update();
        if (position == _hovered) _app.toolTipChanged(this); // its new text, if showing
        return;
}

void NotifyArea::refresh() {
        if (sizeHint().width != geometry().width) {
                _row.relayout(); // resized() tells the programs where
        } else {
                _toldAt = Rect(); // painting tells them
        }
        _hovered = -1; // the icons moved: the next motion finds the one under the pointer
        update();
        _app.toolTipChanged(this);
        return;
}

void NotifyArea::resized() {
        _toldAt = Rect();
        update(); // painting tells the programs where
        return;
}

void NotifyArea::movedDue(void *arg) {
        auto *self = static_cast<NotifyArea *>(arg);
        self->_movedPosted = false;
        self->_tray.moved();
        return;
}

void NotifyArea::paint(Painter &painter) {
        const Rect    &g = geometry();
        const uint32_t face = painter.palette().color(Role::Face);
        drawBevel(painter, g, Bevel::Style::Status); // sunk into the bar
        (void)painter.fill(Bevel::inside(g, Bevel::Style::Status), face);
        if (hasClock()) {
                // A character's room before it; by the icons, half (their cells' edges are the rest).
                const Rect     c = clockRect();
                const uint16_t tw = uint16_t(Fonts::unscii16.textWidth(_time.view()));
                const int      x = _parts == Parts::TrayClock && trayWidth() != 0 ? column(c.x + GuiStyle::Column)
                                   : _parts == Parts::Clock
                                           ? column(c.x + (c.width > tw ? (c.width - tw) / 2 : 0) + GuiStyle::Column / 2)
                                           : column(c.x + Fonts::unscii16.width());
                const int      y = c.y + (c.height - GuiStyle::TextHeight) / 2;
                (void)painter.text(x, y, _time.view(), Fonts::unscii16, painter.palette().color(Role::FaceText), face,
                                   c.width);
        }
        if (!hasTray()) return;
        const Rect t = trayRect();
        IconTheme &icons = _app.icons();
        for (size_t p = 0;; ++p) {
                const TrayRow::Entry *e = _tray.row().shown(p);
                if (e == nullptr) break;
                const int   x = column(t.x + TrayLead + int(p * TrayRow::CellWidth));
                const int   y = t.y + (t.height - GuiStyle::IconSize) / 2;
                const Icon  own = e->pixels();
                const Icon *icon = own.isValid() ? &own : icons.find(TrayProtocol::view(e->icon), GuiStyle::IconSize);
                if (icon == nullptr) icon = icons.find(IconId::ApplicationXExecutable, GuiStyle::IconSize);
                if (icon != nullptr) (void)painter.image(x, y, *icon, face);
        }
        // Moved on the screen (the row laid out again): the programs told, after the paint.
        const Rect at = cellOnScreen(0);
        if ((at.x != _toldAt.x || at.y != _toldAt.y || _toldAt.width == 0) && at.width != 0 && !_movedPosted) {
                _toldAt = at;
                _movedPosted = _app.post(movedDue, this).isOk();
        }
        return;
}

void NotifyArea::mousePressed(const WindowManager::MouseEvent &event) {
        const int p = iconAt(event.x);
        if (p < 0) {
                // Outside the icons (the clock, the edges): the panel's.
                if (event.button == PointerEvent::Right && _menuAsked != nullptr)
                        _menuAsked(_menuArg, event.x, event.y);
                return;
        }
        if (event.button == PointerEvent::Middle) {
                _tray.activate(size_t(p), TrayProtocol::Reason::MiddleClick);
        } else if (event.button == PointerEvent::Right) {
                _tray.activate(size_t(p), TrayProtocol::Reason::Context);
        } else if (event.button == PointerEvent::Left) {
                const uint64_t now = uint64_t(Clock::monotonicNs());
                const bool     twice = p == _lastClicked && now - _lastClickNs < DoubleClickNs;
                _tray.activate(size_t(p), TrayProtocol::Reason::Trigger);
                if (twice) _tray.activate(size_t(p), TrayProtocol::Reason::DoubleClick);
                _lastClicked = twice ? -1 : p;
                _lastClickNs = now;
        }
        return;
}

void NotifyArea::mouseMoved(const WindowManager::MouseEvent &event) {
        const Rect c = clockRect();
        int        p = iconAt(event.x);
        if (p < 0 && c.width != 0 && event.x >= c.x && event.x < int(c.right())) p = ClockHovered;
        if (p == _hovered) return;
        _hovered = p;
        _app.toolTipChanged(this);
        return;
}

void NotifyArea::mouseLeft() {
        _hovered = -1;
        return;
}

bool NotifyArea::wheelTurned(int notches) {
        if (_hovered < 0) return false; // none, or the clock
        _tray.scroll(size_t(_hovered), notches);
        return true;
}

StringView NotifyArea::toolTip() const {
        if (_hovered == ClockHovered) return _date.view();
        const TrayRow::Entry *e = _hovered >= 0 ? _tray.row().shown(size_t(_hovered)) : nullptr;
        return e != nullptr ? TrayProtocol::view(e->toolTip) : StringView();
}

BMDS_NAMESPACE_END
