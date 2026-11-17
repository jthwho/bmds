/**
 * @file      screensharing.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <bmds/config.h>
#include <bmds/guiapplication.h>
#include <bmds/menu.h>
#include <bmds/trayicon.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tray's "screen shared" icon (`devplan/vnc.md` R9): shown while any program watches
 *        the screen (`ScreenWatch`), its tooltip and menu naming them. The system's, not the
 *        watching program's, so a program can't hide that it watches; no setting removes it.
 *
 * @par Thread Safety
 * The application's lock; `ScreenWatch`'s activity notification from any task.
 */
class ScreenSharing {
        public:
                explicit ScreenSharing(GuiApplication &app);
                ~ScreenSharing();
                ScreenSharing(const ScreenSharing &) = delete;
                ScreenSharing &operator=(const ScreenSharing &) = delete;

                /** @brief Listens to `ScreenWatch` (the one listener) and shows the icon if anyone watches. */
                void start();

        private:
                static void changed(void *arg);
                static void changeDue(void *arg);
                static void activated(void *arg, TrayIcon &icon, TrayIcon::Reason reason);
                void        refresh();

                GuiApplication   &_app;
                TrayIcon          _icon;
                Menu              _menu;
                std::atomic<bool> _posted{false};
                bool              _shown = false;
                char              _tip[96] = {};
};

BMDS_NAMESPACE_END
