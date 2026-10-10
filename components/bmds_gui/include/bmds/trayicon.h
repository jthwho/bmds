/**
 * @file      trayicon.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/rect.h>
#include <bmds/icon.h>
#include <bmds/notifier.h>
#include <bmds/stringview.h>
#include <bmds/trayprotocol.h>

BMDS_NAMESPACE_BEGIN

class GuiApplication;
class Menu;

/**
 * @brief A program's icon in the system tray (`devplan/mediaplayer.md` §6e),
 *        as Qt's `QSystemTrayIcon`: an icon, a tooltip, a click told, a menu
 *        on a right click, the wheel told.
 *
 * The tray is the panel's (its `tray` item); the icon talks to it through
 * window manager services (`TrayProtocol`), so it is shown wherever the tray
 * is, and shown again on its own when the tray comes back (the panel started
 * again). Without a tray it waits: `isSystemTrayAvailable()` says so, and
 * `setAvailabilityChanged()`'s function is told when that changes.
 *
 * A program has up to `GuiApplication::MaxTrayIcons` of them.
 *
 * @par Thread Safety
 * The application's lock (the loop's thread).
 */
class TrayIcon {
        public:
                using Reason = TrayProtocol::Reason;
                using Category = TrayProtocol::Category;

                /** @brief Told it was clicked (a `Context` after the menu, if any, opened). */
                using Activated = void (*)(void *arg, TrayIcon &icon, Reason reason);

                /** @brief Told of the wheel over it: @p notches, up positive. */
                using Scrolled = void (*)(void *arg, TrayIcon &icon, int notches);

                /** @brief Told that the tray came or went. */
                using AvailabilityChanged = void (*)(void *arg, TrayIcon &icon, bool available);

                /** @brief Told that its message (`showMessage()`) was clicked. */
                using MessageClicked = void (*)(void *arg, TrayIcon &icon);

                explicit TrayIcon(GuiApplication &app);
                ~TrayIcon();
                TrayIcon(const TrayIcon &) = delete;
                TrayIcon &operator=(const TrayIcon &) = delete;

                /** @brief Shows the theme's icon @p name (cut at 63 bytes). */
                void setIcon(StringView name);

                /**
                 * @brief Shows @p icon's own pixels (16 x 16, copied: drawn by the
                 *        program); nothing is sent for the same pixels, so a live
                 *        picture may be set at every change of what it shows.
                 * @return `Ok`, `Invalid` (not 16 x 16) or `NoMem`.
                 */
                Error setIcon(const Icon &icon);

                /** @brief Sets the tooltip (cut at 127 bytes), changed live. */
                void setToolTip(StringView text);

                /** @brief Sets where the tray puts it (`Application` by default). */
                void setCategory(Category category);

                /** @brief Opens @p menu (not owned; null: none) on a right click, at the icon. */
                void setContextMenu(Menu *menu) { _menu = menu; }

                void setActivated(Activated fn, void *arg) {
                        _activated = fn;
                        _activatedArg = arg;
                }

                void setScrolled(Scrolled fn, void *arg) {
                        _scrolled = fn;
                        _scrolledArg = arg;
                }

                void setAvailabilityChanged(AvailabilityChanged fn, void *arg) {
                        _availability = fn;
                        _availabilityArg = arg;
                }

                /**
                 * @brief Shows a message by the tray (a notification, `devplan/notifications.md`
                 *        §3.5, Qt's `showMessage()`): @p title, @p text, a theme @p icon
                 *        (empty: the application's), for @p timeoutMs (0: the setting's); the
                 *        last message replaced.
                 * @return `Ok`, or the notification's error.
                 */
                Error showMessage(StringView title, StringView text, StringView icon = StringView(),
                                  int32_t timeoutMs = 0);

                void setMessageClicked(MessageClicked fn, void *arg) {
                        _messageClicked = fn;
                        _messageClickedArg = arg;
                }

                /** @brief Shows it in the tray (now, or when there is one). @return `Ok`, `Full` or `NoMem`. */
                Error show();

                /** @brief Hides it (kept in the tray, not shown). */
                void hide();

                /** @brief Returns true if shown (asked to be: the tray may not be there). */
                bool isVisible() const { return _visible; }

                /** @brief Returns its place on the screen (empty until the tray says). */
                Rect geometry() const { return _geometry; }

                /** @brief Returns true if the tray is there. */
                bool isSystemTrayAvailable() const;

                /** @brief Returns its number in its program's messages. */
                uint32_t id() const { return _id; }

        private:
                Error       join(); // into the application's icons, its reply service made
                void        leave();
                void        send(); // all of it
                void        sendUpdate();
                void        sendPixels();
                static void replied(void *arg, ByteSpan message);
                static void servicesChanged(void *arg);
                void        replyName(char (&out)[TrayProtocol::MaxReplyName + 1]) const;
                void        told(const TrayProtocol::Event &event);
                static void messageTold(void *arg, Notifier &notifier, uint32_t id, Notifier::Event event,
                                        uint32_t action);

                GuiApplication     &_app;
                uint32_t            _id = 0;
                Category            _category = Category::Application;
                char                _icon[64] = {};
                char                _toolTip[128] = {};
                uint32_t           *_pixels = nullptr; // 16 x 16, PSRAM, when set
                Menu               *_menu = nullptr;
                Rect                _geometry;
                bool                _visible = false;
                bool                _joined = false;    // in the application's icons
                bool                _available = false; // the tray, as last seen
                Activated           _activated = nullptr;
                void               *_activatedArg = nullptr;
                Scrolled            _scrolled = nullptr;
                void               *_scrolledArg = nullptr;
                AvailabilityChanged _availability = nullptr;
                void               *_availabilityArg = nullptr;
                Notifier            _notifier;
                uint32_t            _messageId = 0; // the last message's notification
                MessageClicked      _messageClicked = nullptr;
                void               *_messageClickedArg = nullptr;
};

BMDS_NAMESPACE_END
