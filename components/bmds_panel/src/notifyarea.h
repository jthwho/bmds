/**
 * @file      notifyarea.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/timezone.h>
#include <bmds/guiapplication.h>
#include <bmds/string.h>
#include <bmds/rect.h>
#include <bmds/trayrow.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class NotifyArea;

/**
 * @brief The system tray's service (`devplan/mediaplayer.md` §6e): takes the
 *        programs' icons (`TrayProtocol`, from their `TrayIcon`s) on the
 *        service `tray`, forgets those of programs gone, and tells them of
 *        clicks, the wheel and where their icons are. Kept while the panel
 *        runs, so its icons outlive its items being made again; started
 *        while `panel/items` has a tray.
 *
 * @par Thread Safety
 * The application's lock.
 */
class TrayService {
        public:
                explicit TrayService(GuiApplication &app) : _app(app) {}
                ~TrayService() { stop(); }

                /** @brief Takes the service `tray`. @return `Ok`, or the application's error (`Exists`: another tray). */
                Error start();

                /** @brief Gives it up, the icons forgotten (their programs see it go). */
                void stop();

                /** @brief Returns true while it has the service. */
                bool isRunning() const { return _running; }

                /** @brief Shows the icons in @p area (null: none, being destroyed). */
                void setArea(NotifyArea *area);

                /** @brief Returns the icons. */
                const TrayRow &row() const { return _row; }

                /** @brief Returns the icon of @p reply numbered @p id's position shown (-1: not shown). */
                int positionOf(StringView reply, uint32_t id) const { return _row.positionOf(reply, id); }

                /** @brief Tells the program of the icon at @p position: clicked, for @p reason. */
                void activate(size_t position, TrayProtocol::Reason reason);

                /** @brief Tells the program of the icon at @p position: the wheel. */
                void scroll(size_t position, int notches);

                /** @brief The icons moved on the screen: each program told where its are. */
                void moved();

        private:
                static void message(void *arg, ByteSpan message);
                static void servicesChanged(void *arg);
                static bool gone(void *arg, StringView reply);
                void        send(const TrayRow::Entry &entry, TrayProtocol::Kind kind, TrayProtocol::Reason reason,
                                 int notches);
                Rect        screenRect(size_t position) const; // empty without an area
                void        changed();                         // the row: shown again, the icons' places told

                GuiApplication &_app;
                TrayRow         _row;
                NotifyArea     *_area = nullptr;
                bool            _running = false;
};

/**
 * @brief The panel's notification area: the system tray's icons and the
 *        clock, in one sunken field (`panel/items`' `tray` and `clock`; side by
 *        side they are one area, in the list's order; either alone is an area
 *        of its own).
 *
 * The icons sit in a row, a cell each, flat (no edges, as the classic trays),
 * with their tooltips; a press tells the program (a left one twice in a
 * while also a double click), as the wheel does. The clock is the time in
 * `panel/clock/format`, redrawn as it changes.
 *
 * @par Thread Safety
 * The application's lock.
 */
class NotifyArea final : public Widget {
        public:
                /** @brief Its parts, in order. */
                enum class Parts : uint8_t {
                        Tray,      ///< The icons only.
                        Clock,     ///< The clock only.
                        TrayClock, ///< The icons, then the clock.
                        ClockTray, ///< The clock, then the icons.
                };

                /**
                 * @brief Makes it in @p parent, the panel's @p row (laid out again as
                 *        the icons come and go), with @p parts; @p tray: the icons'
                 *        (unused without a tray among the parts).
                 */
                NotifyArea(Widget *parent, BoxLayout &row, GuiApplication &app, TrayService &tray, Parts parts);
                ~NotifyArea() override;

                /**
                 * @brief Sets the clock's format and its tooltip's (`strftime`'s,
                 *        empty: the defaults, " 1:00 AM" and "Thursday, January 1,
                 *        2026") and zone (empty: the kernel's).
                 * @return `Ok`, `BufferTooSmall` (a format), or the zone's error.
                 */
                Error configureClock(StringView format, StringView tipFormat, StringView zone);

                /** @brief Told of a right press outside the icons (at @p x, @p y in the window): the panel's menu. */
                using MenuAsked = void (*)(void *arg, int x, int y);

                void setMenuAsked(MenuAsked fn, void *arg) {
                        _menuAsked = fn;
                        _menuArg = arg;
                }

                /** @brief The icons came, went or moved: sized, drawn again and their places told. */
                void refresh();

                /** @brief Icon @p position's look or tooltip changed (-1: not shown): drawn again. */
                void iconChanged(int position);

                /** @brief Returns the icon at @p position's cell on the screen (empty: not open). */
                Rect cellOnScreen(size_t position) const;

                GuiSize    sizeHint() const override;
                void       paint(Painter &painter) override;
                void       mousePressed(const WindowManager::MouseEvent &event) override;
                void       mouseMoved(const WindowManager::MouseEvent &event) override;
                void       mouseLeft() override;
                bool       wheelTurned(int notches) override;
                StringView toolTip() const override;

        protected:
                void resized() override;

        private:
                static void movedDue(void *arg);
                static void clockDue(void *arg);
                bool        hasTray() const { return _parts != Parts::Clock; }
                bool        hasClock() const { return _parts != Parts::Tray; }
                // The room inside the edge at either end (an icon's first column, the edge being one
                // pixel), and between things: the icons' cells' own, and the icons and the clock.
                static constexpr uint16_t Inset = GuiStyle::Column - 1;
                static constexpr uint16_t Gap = TrayRow::CellWidth - GuiStyle::IconSize;
                size_t                    icons() const { return hasTray() ? _tray.row().shownCount() : 0; }
                Rect                      trayRect() const;    // the icons' cells (a gap after the last), in the window
                Rect                      clockRect() const;   // the clock's part, in the window
                int                       clockX() const;      // where its text starts (any pixel)
                int  iconAt(int x) const; // a window x: the position, or -1
                void tick();

                BoxLayout      &_row;
                GuiApplication &_app;
                TrayService    &_tray;
                Parts           _parts;
                int             _hovered = -1;
                int             _lastClicked = -1; // for a double click
                uint64_t        _lastClickNs = 0;
                Rect            _toldAt; // where the icons were last told to be
                bool            _movedPosted = false;
                MenuAsked       _menuAsked = nullptr;
                void           *_menuArg = nullptr;
                // The clock's.
                static constexpr int ClockHovered = -2; // _hovered: the clock (its tooltip, the date)
                uint16_t             timeWidth() const; // as wide as the time gets, in the font now
                char                 _format[32] = "%l:%M %p";
                char                 _tipFormat[64] = "%A, %B %-d, %Y";
                TimeZone             _zone;
                bool                 _hasZone = false;
                bool                 _seconds = false;
                int                  _clockTimer = -1;
                String               _time{MemSpace::Psram};
                String               _date{MemSpace::Psram}; // its tooltip
};

BMDS_NAMESPACE_END
