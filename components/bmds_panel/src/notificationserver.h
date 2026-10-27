/**
 * @file      notificationserver.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/widget.h>
#include <bmds/config.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/menu.h>
#include <bmds/notifications.h>
#include <bmds/trayicon.h>

BMDS_NAMESPACE_BEGIN

class NotificationServer;

/**
 * @brief A notification's balloon (`devplan/notifications.md` §3.4): its
 *        icon, the application's name, the title, the body wrapped (four lines
 *        at most), a close box and the action buttons; a red edge if critical.
 *        A click on it runs its target, the close box dismisses it, a button
 *        is its action. Its time waits while the pointer is over it.
 *
 * @par Thread Safety
 * The application's lock.
 */
class NotificationBalloon final : public GuiWindow {
        public:
                /** @brief Its width. */
                static constexpr uint16_t Width = 320;

                NotificationBalloon(GuiApplication &app, NotificationServer &server);

                /** @brief Shows @p n (its look made again), its height returned. */
                uint16_t setNotification(const Notification &n);

                /** @brief Returns the notification shown (0: none). */
                uint32_t id() const { return _id; }

                /** @brief Returns the change it shows (to know when it changed). */
                uint64_t changed() const { return _changed; }

                void paint(Painter &painter) override;
                void mousePressed(const WindowManager::MouseEvent &event) override;

        protected:
                void resized() override;
                void pointerEntered() override; // its time waits while the pointer is over it
                void pointerLeft() override;

        private:
                static constexpr size_t MaxLines = 4;

                static void actionClicked(void *arg, Button &button);
                Rect        closeRect() const; // in the window

                NotificationServer &_server;
                Notification        _n;
                uint32_t            _id = 0;
                uint64_t            _changed = 0;
                const Icon         *_icon = nullptr;
                StringView          _lines[MaxLines];
                size_t              _lineCount = 0;
                Button             *_buttons[Notification::MaxActions] = {};
                uint16_t            _height = 0;
                bool                _hovered = false;
};

/**
 * @brief The notification centre's list (`devplan/notifications.md` §3.4):
 *        the notifications kept, newest first, each its icon, title, the
 *        body's first line and how long ago, clickable as its balloon was, a
 *        close box removing it; Clear All. A popup by the tray icon.
 *
 * @par Thread Safety
 * The application's lock.
 */
class NotificationCentre final : public GuiWindow {
        public:
                NotificationCentre(GuiApplication &app, NotificationServer &server);

                /** @brief Opens it by @p anchor (the tray icon, on the screen). */
                void open(const Rect &anchor);

                /** @brief Reads the notifications again (they changed). */
                void refresh();

                void paint(Painter &painter) override;
                void mousePressed(const WindowManager::MouseEvent &event) override;
                bool wheelTurned(int notches) override;

                /** @brief Closes it: what it showed is read now. */
                void closeCentre();

        protected:
                void resized() override;
                bool keyUnused(const KeyEvent &event) override;
                void closedByManager() override;

        private:
                static constexpr uint16_t Width = 360;
                uint16_t                  rowHeight() const { return uint16_t(2 * textHeight() + 6); }

                static void clearClicked(void *arg, Button &button);
                uint16_t    rowsTop() const;
                size_t      visibleRows() const;

                NotificationServer &_server;
                uint32_t            _ids[NotificationLog::Capacity] = {};
                size_t              _count = 0;
                size_t              _first = 0; // scrolled past
                Button             *_clear = nullptr;
};

/**
 * @brief The panel's notification server (`devplan/notifications.md` §3.4):
 *        the kernel's `Notifications` shown as balloons by the tray (three at
 *        most, the rest waiting), a sound by urgency, and the centre (a tray
 *        icon and its list). Targets are run through the panel (`Targets`).
 *
 * @par Thread Safety
 * The application's lock (the kernel's changes are posted to its loop).
 */
class NotificationServer {
        public:
                /** @brief How the panel runs a target. */
                struct Targets {
                                void (*open)(void *arg, StringView path) = nullptr; ///< By its type's application.
                                void (*show)(void *arg, StringView path) = nullptr; ///< In its folder, selected.
                                void (*launch)(void *arg, StringView id) = nullptr; ///< A Desktop Entry.
                                void *arg = nullptr;
                };

                /** @brief The settings it follows. */
                static constexpr const char *SoundKey = "notify/sound";
                static constexpr const char *TimeoutKey = "notify/timeout";
                static constexpr const char *BalloonsKey = "notify/balloons";
                static constexpr const char *QuietKey = "notify/quiet";

                NotificationServer(GuiApplication &app, const Targets &targets);
                ~NotificationServer();
                NotificationServer(const NotificationServer &) = delete;
                NotificationServer &operator=(const NotificationServer &) = delete;

                /** @brief Becomes the system's server. @return `Ok`, or `Exists` (another panel's). */
                Error start();

                /** @brief Shows or hides the centre's tray icon (`panel/tray/notifications`). */
                void setCentreShown(bool shown);

                /** @brief The panel moved (`panel/edge`), @p bottom or not, @p panelHeight tall. */
                void setPanel(bool bottom, uint16_t panelHeight);

                /** @brief The settings changed: read again. */
                void settingsChanged();

                // ---- The balloons' and the centre's
                void activate(uint32_t id, uint32_t action, bool fromBalloon); // run its target, tell its sender
                void dismiss(uint32_t id);                                     // a balloon's close box
                void remove(uint32_t id);                                      // the centre's close box
                void clearAll();
                void hoverChanged(NotificationBalloon &balloon, bool over); // its time waits while over
                void centreClosed() { Notifications::markRead(0); }         // shown: read

                /** @brief Returns @p n's icon (its own, else by urgency), @p side square. */
                const Icon *iconOf(const Notification &n, uint16_t side);

                GuiApplication &application() const { return _app; }

        private:
                static constexpr size_t   MaxBalloons = 3;
                static constexpr uint16_t Gap = 8;

                // The centre's menu's choices.
                enum Choice : int {
                        ChooseBalloons = 1,
                        ChooseSound,
                        ChooseTime3,
                        ChooseTime6,
                        ChooseTime10,
                        ChooseTime20,
                        ChooseQuiet10m,
                        ChooseQuiet1h,
                        ChooseQuiet8h,
                        ChooseQuiet24h,
                        ChooseQuietOn,
                        ChooseQuietOff,
                        ChooseClear,
                };

                struct Shown {
                                NotificationBalloon *balloon = nullptr;
                                int                  timer = -1;
                                int32_t              timeoutMs = 0;
                };

                static void changedFromKernel(void *arg); // any thread: posts sync()
                static void syncDue(void *arg);
                static void expired(void *arg);
                static void trayActivated(void *arg, TrayIcon &icon, TrayIcon::Reason reason);
                static void chosen(void *arg, int item);
                static void quietEnded(void *arg);
                void        fillMenu();
                bool        isQuiet() const;
                void        quietFor(uint32_t minutes); // 0: off (the setting too)
                void        save(const char *key, StringView value);
                void        sync();
                void        layout();
                void        startTimer(Shown &s);
                void        stopTimer(Shown &s);
                void        showCentreIcon();
                void        runTarget(const Notification &n);

                GuiApplication     &_app;
                Targets             _targets;
                bool                _started = false;
                std::atomic<bool>   _syncPosted{false};
                Shown               _shown[MaxBalloons];
                NotificationCentre *_centre = nullptr; // PSRAM, made when first opened
                TrayIcon            _icon;
                Menu                _menu;
                Menu                _timeMenu;
                Menu                _quietMenu;
                bool                _quietForever = false; // notify/quiet
                int64_t             _quietUntilMs = 0;     // a timed Do Not Disturb's end (monotonic; 0: none)
                int                 _quietTimer = -1;
                bool                _iconQuiet = false; // as the icon shows it
                bool                _centreShown = false;
                bool                _bottom = false;
                uint16_t            _panelHeight = 0;
                bool                _sound = true;
                bool                _balloons = true;
                int32_t             _timeoutMs = 6000;
                size_t              _unread = SIZE_MAX; // as the icon shows it
                uint32_t            _iconPixels[16 * 16] = {};
                char                _tip[96] = {};
};

BMDS_NAMESPACE_END
