/**
 * @file      networkstatus.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/guiapplication.h>
#include <bmds/menu.h>
#include <bmds/netstatus.h>
#include <bmds/trayicon.h>

#include "networkview.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tray's network icon (`devplan/network.md` §17, §20): a tray icon
 *        in the panel's process showing the interfaces' best state
 *        (`NetworkView`), its tooltip each interface's lines. Only the
 *        kernel's public `NetStatus`, as any program could.
 *
 * It watches `NetStatus` for its life: a change on the network task posts one
 * refresh to the application's loop, so nothing runs while nothing changes.
 * A click, left or right, opens a menu read at that moment: the lines again
 * (with the lease's time left), Copy Address, Renew Lease (an interface
 * holding a DHCP lease) and Remove from Panel (`panel/tray/network` false).
 *
 * @par Thread Safety
 * The application's lock; the watch's notification from the network task.
 */
class NetworkStatus {
        public:
                /** @brief The setting its Remove from Panel clears (the panel's). */
                static constexpr const char *ShownKey = "panel/tray/network";

                explicit NetworkStatus(GuiApplication &app);
                ~NetworkStatus();
                NetworkStatus(const NetworkStatus &) = delete;
                NetworkStatus &operator=(const NetworkStatus &) = delete;

                /**
                 * @brief Starts watching (when there is a network stack) and shows its icon.
                 * @return `Ok`, or the tray's error.
                 */
                Error start();

                /** @brief Told to open Settings at a page (its Settings... item; the panel starts it). */
                using OpenSettings = void (*)(void *arg, const char *page);

                /** @brief Has @p fn told of its Settings... item (none without it). */
                void setOpenSettings(OpenSettings fn, void *arg) {
                        _openSettings = fn;
                        _openSettingsArg = arg;
                        return;
                }

        private:
                // The interfaces read at most (with lo).
                static constexpr size_t MaxInterfaces = 4;

                enum Choice : int {
                        ChooseCopy = 1,
                        ChooseRemove,
                        ChooseSettings,
                        ChooseRenew, // + the interface's position
                };

                static void changed(void *arg);
                static void changeDue(void *arg);
                static void activated(void *arg, TrayIcon &icon, TrayIcon::Reason reason);
                static void chosen(void *arg, int item);
                static void removeDue(void *arg);
                void        read();
                void        refresh();
                void        fillMenu();

                GuiApplication    &_app;
                TrayIcon           _icon;
                Menu               _menu;
                NetStatus::Watcher _watcher;
                std::atomic<bool>  _posted{false}; // changeDue() on its way
                size_t             _count = 0;
                NetStatus::Info    _interfaces[MaxInterfaces];
                uint32_t           _pixels[NetworkView::Side * NetworkView::Side] = {};
                char               _tip[TrayProtocol::MaxToolTip + 1] = {};
                char               _address[24] = {}; // Copy Address's: the first connected one
                OpenSettings       _openSettings = nullptr;
                void              *_openSettingsArg = nullptr;
};

BMDS_NAMESPACE_END
