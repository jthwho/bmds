/**
 * @file      screensharing.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tray's "screen shared" icon (devplan/vnc.md R9): ScreenWatch's watchers, named.
 */

#include <cstdio>
#include <cstring>
#include <bmds/log.h>
#include <bmds/menulayout.h>
#include <bmds/screenwatch.h>

#include "screensharing.h"

BMDS_NAMESPACE_BEGIN

ScreenSharing::ScreenSharing(GuiApplication &app, TrayService &tray) : _app(app), _tray(tray), _icon(app), _menu(app) {
        _icon.setCategory(TrayIcon::Category::System);
        _icon.setIcon("preferences-desktop-display");
        _icon.setActivated(activated, this);
}

ScreenSharing::~ScreenSharing() {
        ScreenWatch::setActivity(nullptr, nullptr); // a notification running has ended (its lock)
        _tray.setChanged(nullptr, nullptr);
        _app.cancelPosts(this);
}

void ScreenSharing::start() {
        ScreenWatch::setActivity(changed, this);
        _tray.setChanged(trayChanged, this);
        refresh();
        return;
}

// On the watcher's task (ScreenWatch's lock held): the loop told, once until it reads.
void ScreenSharing::changed(void *arg) {
        auto *self = static_cast<ScreenSharing *>(arg);
        if (self->_posted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->_app.post(changeDue, self).isError()) self->_posted.store(false, std::memory_order_release);
        return;
}

void ScreenSharing::changeDue(void *arg) {
        auto *self = static_cast<ScreenSharing *>(arg);
        self->_posted.store(false, std::memory_order_release);
        self->refresh();
        return;
}

// The tray's row changed (the loop's thread): a watcher's own icon came or went.
void ScreenSharing::trayChanged(void *arg) {
        auto *self = static_cast<ScreenSharing *>(arg);
        if (ScreenWatch::count() == 0) return;
        changed(self); // after the row's change is done
        return;
}

bool ScreenSharing::needsOwnIcon() {
        uint32_t     clients[ScreenWatch::MaxWatchers];
        const size_t n = ScreenWatch::clients(clients, ScreenWatch::MaxWatchers);
        for (size_t i = 0; i < n && i < ScreenWatch::MaxWatchers; ++i) {
                char reply[TrayProtocol::MaxReplyName + 1];
                std::snprintf(reply, sizeof(reply), "tray.%u", unsigned(clients[i]));
                bool shown = false;
                for (size_t p = 0; !shown; ++p) {
                        const TrayRow::Entry *e = _tray.row().shown(p);
                        if (e == nullptr) break;
                        shown = std::strcmp(e->reply, reply) == 0;
                }
                if (!shown) return true;
        }
        return false;
}

void ScreenSharing::refresh() {
        _tray.repaint(); // the dots on watchers' own icons
        const size_t n = ScreenWatch::count();
        if (n == 0 || !needsOwnIcon()) {
                if (_shown) _icon.hide();
                _shown = false;
                return;
        }
        char first[32] = {};
        (void)ScreenWatch::owner(0, first, sizeof(first));
        if (n == 1)
                std::snprintf(_tip, sizeof(_tip), "The screen is shared with %s", first);
        else
                std::snprintf(_tip, sizeof(_tip), "The screen is shared with %s and %u more", first, unsigned(n - 1));
        _icon.setToolTip(StringView(_tip));
        if (!_shown) {
                const Error err = _icon.show();
                _shown = err.isOk();
                if (err.isError()) bmdsWarn("panel: the screen-shared icon can't be shown: %s", err.desc());
        }
        return;
}

// A click, left or right: who watches, read now.
void ScreenSharing::activated(void *arg, TrayIcon &icon, TrayIcon::Reason reason) {
        auto *self = static_cast<ScreenSharing *>(arg);
        if (reason != TrayIcon::Reason::Trigger && reason != TrayIcon::Reason::Context) return;
        self->_menu.clear();
        (void)self->_menu.addItem("The screen is shared with:", 0, MenuLayout::Disabled);
        char name[32];
        for (size_t i = 0; ScreenWatch::owner(i, name, sizeof(name)); ++i) {
                char line[48];
                std::snprintf(line, sizeof(line), "    %s", name);
                (void)self->_menu.addItem(StringView(line), 0, MenuLayout::Disabled);
        }
        (void)self->_menu.popup(icon.geometry());
        return;
}

BMDS_NAMESPACE_END
