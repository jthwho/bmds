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

#include "notifyarea.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tray's mark of a watched screen (`devplan/vnc.md` R9): while a program watches the
 *        screen (`ScreenWatch`), a red dot on its own tray icons (`NotifyArea` draws it), and this
 *        "screen shared" icon of the system's only for one with no icon of its own, its tooltip
 *        and menu naming the watchers. The system's, not the watching program's, so a program can't
 *        hide that it watches; no setting removes it.
 *
 * @par Thread Safety
 * The application's lock; `ScreenWatch`'s activity notification from any task.
 */
class ScreenSharing {
        public:
                ScreenSharing(GuiApplication &app, TrayService &tray);
                ~ScreenSharing();
                ScreenSharing(const ScreenSharing &) = delete;
                ScreenSharing &operator=(const ScreenSharing &) = delete;

                /** @brief Listens to `ScreenWatch` (the one listener) and shows the icon if anyone watches. */
                void start();

        private:
                static void changed(void *arg);
                static void trayChanged(void *arg); // the tray's icons came, went or changed
                bool        needsOwnIcon();         // a watcher with no visible icon of its own
                static void changeDue(void *arg);
                static void activated(void *arg, TrayIcon &icon, TrayIcon::Reason reason);
                void        refresh();

                GuiApplication   &_app;
                TrayService      &_tray;
                TrayIcon          _icon;
                Menu              _menu;
                std::atomic<bool> _posted{false};
                bool              _shown = false;
                char              _tip[96] = {};
};

BMDS_NAMESPACE_END
