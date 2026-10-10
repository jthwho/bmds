/**
 * @file      trayicon.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program's icon in the system tray (devplan/mediaplayer.md §6e): its messages to the tray's
 * service, and the tray's replies to the program's own (one for all its icons).
 */

#include <cstdio>
#include <cstring>
#include <bmds/guiapplication.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/trayicon.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t PixelCount = TrayProtocol::IconSide * TrayProtocol::IconSide;

        // The application's icons have one reply service and one watch between them.
        bool anyJoined(TrayIcon *const (&icons)[GuiApplication::MaxTrayIcons]) {
                for (const TrayIcon *t : icons)
                        if (t != nullptr) return true;
                return false;
        }

} // namespace

TrayIcon::TrayIcon(GuiApplication &app) : _app(app), _notifier(app) {
        _notifier.setTold(messageTold, this);
}

TrayIcon::~TrayIcon() {
        leave();
        Memory::free(_pixels);
}

void TrayIcon::replyName(char (&out)[TrayProtocol::MaxReplyName + 1]) const {
        std::snprintf(out, sizeof(out), "tray.%u", unsigned(_app.client()));
        return;
}

Error TrayIcon::join() {
        if (_joined) return Error::Ok;
        TrayIcon **slot = nullptr;
        for (TrayIcon *&t : _app._trayIcons)
                if (t == nullptr) {
                        slot = &t;
                        break;
                }
        if (slot == nullptr) return Error::Full;
        if (!anyJoined(_app._trayIcons)) {
                // The first: the replies' service, and the tray's coming and going watched.
                char name[TrayProtocol::MaxReplyName + 1];
                replyName(name);
                Error err = _app.registerService(StringView(name), replied, &_app);
                if (err.isOk()) {
                        err = _app.watchServices(servicesChanged, &_app);
                        if (err.isError()) (void)_app.unregisterService(StringView(name));
                }
                if (err.isError()) return err;
        }
        *slot = this;
        if (++_app._lastTrayId == 0) ++_app._lastTrayId;
        _id = _app._lastTrayId;
        _joined = true;
        _available = isSystemTrayAvailable();
        return Error::Ok;
}

void TrayIcon::leave() {
        if (!_joined) return;
        if (_available) {
                TrayProtocol::Header h;
                h.kind = TrayProtocol::Kind::Remove;
                h.id = _id;
                replyName(h.reply);
                (void)_app.windowManager().sendToService(TrayProtocol::Service, TrayProtocol::bytes(h));
        }
        for (TrayIcon *&t : _app._trayIcons)
                if (t == this) t = nullptr;
        _joined = false;
        if (!anyJoined(_app._trayIcons)) {
                char name[TrayProtocol::MaxReplyName + 1];
                replyName(name);
                _app.unwatchServices(servicesChanged, &_app);
                (void)_app.unregisterService(StringView(name));
        }
        return;
}

bool TrayIcon::isSystemTrayAvailable() const {
        return _app.hasService(TrayProtocol::Service);
}

// All of it, each time: the tray keeps no state a message could leave half changed.
void TrayIcon::send() {
        sendUpdate();
        sendPixels();
        return;
}

void TrayIcon::sendUpdate() {
        if (!_joined || !_available) return;
        TrayProtocol::Update u;
        u.id = _id;
        replyName(u.reply);
        u.category = _category;
        u.flags = (_visible ? uint32_t(TrayProtocol::Visible) : 0u) |
                  (_pixels != nullptr ? uint32_t(TrayProtocol::HasPixels) : 0u);
        std::memcpy(u.icon, _icon, sizeof(u.icon));
        std::memcpy(u.toolTip, _toolTip, sizeof(u.toolTip));
        (void)_app.windowManager().sendToService(TrayProtocol::Service, TrayProtocol::bytes(u));
        return;
}

void TrayIcon::sendPixels() {
        if (!_joined || !_available || _pixels == nullptr) return;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(TrayProtocol::Pixels));
        if (mem == nullptr) return;
        auto *p = ::new (mem) TrayProtocol::Pixels();
        p->id = _id;
        replyName(p->reply);
        std::memcpy(p->argb, _pixels, sizeof(p->argb));
        (void)_app.windowManager().sendToService(TrayProtocol::Service, TrayProtocol::bytes(*p));
        Memory::free(mem);
        return;
}

void TrayIcon::setIcon(StringView name) {
        TrayProtocol::copy(_icon, name);
        Memory::free(_pixels);
        _pixels = nullptr;
        send();
        return;
}

Error TrayIcon::setIcon(const Icon &icon) {
        if (!icon.isValid() || icon.width != TrayProtocol::IconSide || icon.height != TrayProtocol::IconSide)
                return Error::Invalid;
        const bool had = _pixels != nullptr;
        if (had && std::memcmp(_pixels, icon.pixels, PixelCount * sizeof(uint32_t)) == 0) return Error::Ok;
        if (!had) {
                _pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, PixelCount * sizeof(uint32_t)));
                if (_pixels == nullptr) return Error::NoMem;
        }
        std::memcpy(_pixels, icon.pixels, PixelCount * sizeof(uint32_t));
        // Only the pixels once the tray has the rest: a live picture is a message, not two.
        if (had)
                sendPixels();
        else
                send();
        return Error::Ok;
}

void TrayIcon::setToolTip(StringView text) {
        char next[sizeof(_toolTip)];
        TrayProtocol::copy(next, text);
        if (std::strcmp(next, _toolTip) == 0) return; // nothing sent for the same
        std::memcpy(_toolTip, next, sizeof(next));
        sendUpdate(); // the tray keeps the pixels it has
        return;
}

void TrayIcon::setCategory(Category category) {
        _category = category;
        send();
        return;
}

Error TrayIcon::show() {
        const Error err = join();
        if (err.isError()) return err;
        _visible = true;
        send();
        return Error::Ok;
}

void TrayIcon::hide() {
        if (!_visible) return;
        _visible = false;
        send();
        return;
}

// A service came or went: the tray's, perhaps. Back: every icon sent again.
void TrayIcon::servicesChanged(void *arg) {
        auto      *app = static_cast<GuiApplication *>(arg);
        const bool available = app->hasService(TrayProtocol::Service);
        for (TrayIcon *t : app->_trayIcons) {
                if (t == nullptr || t->_available == available) continue;
                t->_available = available;
                if (!available) t->_geometry = Rect();
                t->send();
                if (t->_availability != nullptr) t->_availability(t->_availabilityArg, *t, available);
        }
        return;
}

void TrayIcon::replied(void *arg, ByteSpan message) {
        auto               *app = static_cast<GuiApplication *>(arg);
        TrayProtocol::Event e;
        if (!TrayProtocol::read(message, e)) return;
        for (TrayIcon *t : app->_trayIcons) {
                if (t == nullptr || t->_id != e.id) continue;
                t->told(e);
                return;
        }
        return;
}

void TrayIcon::told(const TrayProtocol::Event &event) {
        if (event.width != 0 && event.height != 0)
                _geometry = Rect{uint16_t(event.x), uint16_t(event.y), event.width, event.height};
        switch (event.kind) {
                case TrayProtocol::Kind::Activated:
                        if (event.reason == Reason::Context && _menu != nullptr) (void)_menu->popup(_geometry);
                        if (_activated != nullptr) _activated(_activatedArg, *this, event.reason);
                        break;
                case TrayProtocol::Kind::Scrolled:
                        if (_scrolled != nullptr) _scrolled(_scrolledArg, *this, int(event.notches));
                        break;
                default: break; // Geometry: kept above
        }
        return;
}

Error TrayIcon::showMessage(StringView title, StringView text, StringView icon, int32_t timeoutMs) {
        Notification n;
        const auto   put = [](char *out, size_t size, StringView v) {
                const size_t len = v.size() < size - 1 ? v.size() : size - 1;
                std::memcpy(out, v.data(), len);
                out[len] = '\0'; // the log cuts it at a character's end
        };
        put(n.title, sizeof(n.title), title);
        put(n.body, sizeof(n.body), text);
        put(n.icon, sizeof(n.icon), icon);
        n.timeoutMs = timeoutMs;
        n.replaces = _messageId; // one message at a time, as Qt's
        Result<uint32_t> id = _notifier.show(n);
        if (id.isError() && n.replaces != 0) { // the last one went meanwhile: a new one
                n.replaces = 0;
                id = _notifier.show(n);
        }
        if (id.isError()) return id.error();
        _messageId = id.value();
        return Error::Ok;
}

void TrayIcon::messageTold(void *arg, Notifier &, uint32_t id, Notifier::Event event, uint32_t action) {
        auto *self = static_cast<TrayIcon *>(arg);
        if (id != self->_messageId || event != Notifier::Event::Activated || action != 0) return;
        if (self->_messageClicked != nullptr) self->_messageClicked(self->_messageClickedArg, *self);
        return;
}

BMDS_NAMESPACE_END
