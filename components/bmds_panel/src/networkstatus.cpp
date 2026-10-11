/**
 * @file      networkstatus.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tray's network icon (devplan/network.md §17, §20): the kernel's NetStatus, read when it
 * changes, as a picture, a tooltip and a menu.
 */

#include <cstdio>
#include <cstring>
#include <bmds/ethernet.h>
#include <bmds/guiclipboard.h>
#include <bmds/icon.h>
#include <bmds/menulayout.h>
#include <bmds/sysconf.h>

#include "networkstatus.h"

BMDS_NAMESPACE_BEGIN

NetworkStatus::NetworkStatus(GuiApplication &app) : _app(app), _icon(app), _menu(app) {
        _icon.setCategory(TrayIcon::Category::Hardware);
        _icon.setActivated(activated, this);
        _menu.setTriggered(chosen, this);
}

NetworkStatus::~NetworkStatus() {
        _watcher.stop();        // a notification running has ended
        _app.cancelPosts(this); // a change or a Remove from Panel not taken yet
}

Error NetworkStatus::start() {
        // No network stack: shown off, and nothing will change.
        if (!_watcher.isStarted()) (void)_watcher.start(changed, this);
        refresh(); // the picture before it is shown
        return _icon.show();
}

// On the network task, the core lock held: the loop told, once until it reads.
void NetworkStatus::changed(void *arg) {
        auto *self = static_cast<NetworkStatus *>(arg);
        if (self->_posted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->_app.post(changeDue, self).isError()) self->_posted.store(false, std::memory_order_release);
        return;
}

void NetworkStatus::changeDue(void *arg) {
        auto *self = static_cast<NetworkStatus *>(arg);
        self->_posted.store(false, std::memory_order_release); // a change after this posts again
        self->refresh();
        return;
}

void NetworkStatus::read() {
        _count = 0;
        while (_count < MaxInterfaces && NetStatus::info(_count, _interfaces[_count]).isOk()) ++_count;
        return;
}

void NetworkStatus::refresh() {
        read();
        const Span<const NetStatus::Info> all(_interfaces, _count);
        NetworkView::draw(NetworkView::best(all), _pixels);
        (void)_icon.setIcon(Icon{NetworkView::Side, NetworkView::Side, _pixels});
        NetworkView::toolTip(all, Span<char>(_tip, sizeof(_tip)));
        _icon.setToolTip(StringView(_tip));
        return;
}

// A click, left or right: the menu, read now (the lease's time left is the moment's).
void NetworkStatus::activated(void *arg, TrayIcon &icon, TrayIcon::Reason reason) {
        auto *self = static_cast<NetworkStatus *>(arg);
        if (reason != TrayIcon::Reason::Trigger && reason != TrayIcon::Reason::Context) return;
        self->refresh();
        self->fillMenu();
        (void)self->_menu.popup(icon.geometry());
        return;
}

void NetworkStatus::fillMenu() {
        _menu.clear();
        _address[0] = '\0';
        bool any = false;
        for (size_t i = 0; i < _count; ++i) {
                const NetStatus::Info &info = _interfaces[i];
                if (info.loopback) continue;
                if (any) (void)_menu.addSeparator();
                any = true;
                char         lines[NetworkView::MaxLines][NetworkView::LineBytes];
                const size_t n = NetworkView::lines(info, true, lines);
                for (size_t l = 0; l < n; ++l) (void)_menu.addItem(StringView(lines[l]), 0, MenuLayout::Disabled);
                if (_address[0] == '\0' && NetworkView::stateOf(info) == NetworkView::State::Connected) {
                        uint8_t b[4];
                        std::memcpy(b, &info.address, sizeof(b));
                        std::snprintf(_address, sizeof(_address), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                }
        }
        if (!any) (void)_menu.addItem("No network interfaces", 0, MenuLayout::Disabled);
        (void)_menu.addSeparator();
        (void)_menu.addItem("&Copy Address", ChooseCopy, _address[0] != '\0' ? 0 : MenuLayout::Disabled);
        for (size_t i = 0; i < _count; ++i) {
                const NetStatus::Info &info = _interfaces[i];
                if (info.dhcp != NetStatus::Dhcp::Bound) continue;
                char text[NetworkView::LineBytes];
                std::snprintf(text, sizeof(text), "&Renew %s's Lease", info.name);
                (void)_menu.addItem(StringView(text), ChooseRenew + int(i));
        }
        (void)_menu.addSeparator();
        (void)_menu.addItem("&Remove from Panel", ChooseRemove);
        return;
}

void NetworkStatus::chosen(void *arg, int item) {
        auto *self = static_cast<NetworkStatus *>(arg);
        if (item == ChooseCopy) {
                GuiClipboard *clipboard = self->_app.clipboard();
                if (clipboard != nullptr && self->_address[0] != '\0')
                        (void)clipboard->copyText(Clipboard::Selection::Clipboard, StringView(self->_address));
        } else if (item == ChooseRemove) {
                // After the menu's call returns: the panel destroys the icon (and this menu).
                if (self->_app.post(removeDue, self).isError()) removeDue(self);
        } else if (item >= ChooseRenew) {
                (void)NetStatus::renewLease(size_t(item - ChooseRenew)); // the watch tells what came of it
        }
        return;
}

// Saved, and kept across a restart; the panel's menu shows it again.
void NetworkStatus::removeDue(void *arg) {
        (void)arg;
        if (SysConf::set(StringView(ShownKey), "false").isOk()) (void)SysConf::save(StringView(ShownKey));
        return;
}

BMDS_NAMESPACE_END
