/**
 * @file      windowmanager.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/compose.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/mutex.h>
#include <bmds/painter.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/task.h>
#include <bmds/waitlist.h>
#include <bmds/windowpalette.h>
#include <bmds/windowstack.h>

BMDS_NAMESPACE_BEGIN

class Display;
class Surface;

/**
 * @brief The desktop and its windows on a display's surface 0
 *        (`devplan/windows.md` §4a).
 *
 * Windows are drawn directly on surface 0: the window manager draws their
 * frames and asks each window, through its redraw callback, to draw what
 * of its client area was damaged, clipped to what of it shows. Changes
 * (open, close, move, raise, invalidate) only gather damage and wake the
 * window manager's task, which draws a round at most once a frame: every
 * damaged piece once, by the window that shows it, the desktop's in its
 * flat color. A double-buffered surface is presented after each round.
 *
 * Moves are shown as an outline (`drag()`) until `drop()` puts the window
 * there. Focus is the top window.
 *
 * One window manager per display (`manager()`); it claims the display.
 *
 * @par Thread Safety
 * Thread-safe; the calls wait for a round being drawn. Redraw callbacks run
 * on the window manager's task and must not call it.
 */
class WindowManager {
        public:
                /**
                 * @brief Draws a client area: at least the painter's damage, and
                 *        anything else in its clip (all of the area that shows,
                 *        after `update()`). The painter's origin is the area's
                 *        top-left corner.
                 */
                using Redraw = void (*)(void *arg, Painter &painter);

                /**
                 * @brief Told that the window closed (on the closing task, the
                 *        window manager locked: it must not call it).
                 */
                using Closed = void (*)(void *arg);

                /** @brief The longest title kept, in bytes. */
                static constexpr size_t MaxTitle = 47;

                /** @brief How long drawing waits for the 2D-DMA or a swap. */
                static constexpr unsigned int DrawTimeoutMs = 100;

                /** @brief A window to open. */
                struct WindowConfig {
                                Rect        rect;             ///< Outer, frame included; snapped to 4-pixel columns.
                                const char *title = "";       ///< Copied (`MaxTitle` bytes kept).
                                Redraw      redraw = nullptr; ///< Null: the client area is filled (`Role::Client`).
                                void       *arg = nullptr;
                                Closed      closed = nullptr; ///< May be null.
                };

                /** @brief A window, as `list()` reports it. */
                struct Info {
                                uint8_t id = 0;
                                Rect    rect;
                                char    title[MaxTitle + 1] = {};
                                bool    focused = false;
                };

                /** @brief Counts since `start()`. */
                struct Stats {
                                uint32_t rounds = 0;    ///< Rounds drawn.
                                uint32_t pieces = 0;    ///< Pieces planned.
                                uint32_t redraws = 0;   ///< Redraw callbacks.
                                uint32_t calls = 0;     ///< 2D-DMA calls (fills and copies).
                                uint32_t fallbacks = 0; ///< Damage too broken up to plan whole.
                                uint32_t errors = 0;    ///< Drawing errors.
                                uint64_t roundUs = 0;   ///< Drawing (presents not included).
                                uint32_t roundUsMax = 0;
                                uint32_t lastRoundUs = 0;
                };

                /** @brief Window managers: one per display. */
                static constexpr size_t MaxManagers = 1;

                /**
                 * @brief Returns display @p number's window manager.
                 * @return It, or `NotSupported` (no such display).
                 */
                static Result<WindowManager *> manager(size_t number);

                WindowManager() = default;
                WindowManager(const WindowManager &) = delete;
                WindowManager &operator=(const WindowManager &) = delete;

                /**
                 * @brief Claims @p display (running) and draws the desktop on its
                 *        surface 0, in @p palette's colors.
                 * @return `Ok`, `InvalidState` (running, or the display isn't),
                 *         `Busy` (claimed by another), or `NoMem`.
                 * @blocking
                 */
                Error start(Display &display, const WindowPalette &palette);

                /** @brief Closes every window and releases the display; the picture stays. Idempotent. */
                void stop();

                /** @brief Returns true between `start()` and `stop()`. */
                bool isRunning() const { return _surface != nullptr; }

                /**
                 * @brief Opens a window on top (focused).
                 * @return Its number, `InvalidState` (not running), `Invalid`
                 *         (smaller than the frame, or off the screen), or `Full`.
                 */
                Result<uint8_t> open(const WindowConfig &config);

                /** @brief Closes window @p id. @return `Ok`, `NotFound`, `InvalidState`. */
                Error close(uint8_t id);

                /** @brief Moves window @p id's top-left corner to (@p x, @p y), kept on the screen. */
                Error move(uint8_t id, uint16_t x, uint16_t y);

                /** @brief Puts window @p id on top (focused). */
                Error raise(uint8_t id);

                /** @brief Asks window @p id to redraw @p rect of its client area. */
                Error invalidate(uint8_t id, const Rect &rect);

                /**
                 * @brief Asks window @p id's redraw to be called at the next round
                 *        even without damage, its painter clipped to all of its
                 *        client area that shows (a terminal's new rows, a scroll).
                 *        Doesn't wait for a round being drawn (an ask for a window
                 *        not open is dropped).
                 * @return `Ok`, or `NotFound` (no such number).
                 * @nonblocking
                 */
                Error update(uint8_t id);

                /**
                 * @brief Sets the colors and redraws what they changed (the
                 *        desktop alone, or everything).
                 */
                Error setPalette(const WindowPalette &palette);

                /** @brief Returns the colors. */
                WindowPalette palette();

                /**
                 * @brief Shows window @p id's outline with its top-left corner at
                 *        (@p x, @p y), kept on the screen: a move under way.
                 * @return `Ok`, `NotFound`, `InvalidState`, or `Busy` (another
                 *         window's drag under way).
                 */
                Error drag(uint8_t id, uint16_t x, uint16_t y);

                /** @brief Ends a drag: the window raised and moved where its outline is. */
                Error drop();

                /** @brief Writes up to @p out's size of windows, top first. @return The count. */
                size_t list(Span<Info> out);

                /** @brief Returns window @p id's client area (screen coordinates). */
                Result<Rect> clientRect(uint8_t id);

                /**
                 * @brief Waits until every change made before the call is drawn
                 *        and (double-buffered) shown.
                 * @return `Ok`, `Timeout`, or `InvalidState`.
                 * @blocking
                 */
                Error sync(unsigned int timeoutMs);

                /** @brief Returns the counts. */
                Stats stats();

        private:
                struct Window {
                                char   title[MaxTitle + 1] = {};
                                Redraw redraw = nullptr;
                                Closed closed = nullptr;
                                void  *arg = nullptr;
                };

                static constexpr size_t MaxPieces = 256;

                static void entry(void *arg);
                void        run();
                void        round();
                void        drawPieces(Span<const ComposePiece> pieces);
                void        drawLayer(size_t z, Span<const Rect> pieces);
                void        drawUpdates(); // windows asking for an update and not drawn yet this round
                void        drawBottomUp(const Rect &rect);
                void        drawOutline();
                void        changed(uint8_t oldTop); // wakes the task; repaints titles if the focus moved
                Rect        placed(const Rect &rect) const;
                void        count(const Painter &painter);

                Mutex                 _lock;
                Display              *_display = nullptr;
                Surface              *_surface = nullptr;
                WindowStack           _stack{0, 0};
                Window                _windows[WindowStack::MaxWindows]; // by number - 1
                WindowPalette         _palette;
                uint8_t               _dragId = 0;
                Rect                  _outline;
                bool                  _outlineShown = false;
                Painter::Tools        _tools;
                GlyphRenderer         _renderer{GlyphRenderer::best()};
                uint16_t              _updates = 0;       // windows asking for an update, a bit by number - 1
                ComposePiece         *_pieces = nullptr;  // a round's plan
                Rect                 *_scratch = nullptr; // a layer's pieces, client damage, and visible client
                std::atomic<uint32_t> _requests{0};       // changes made
                std::atomic<uint16_t> _asked{0};          // update() calls, taken by the next round
                std::atomic<uint32_t> _drawn{0};          // changes drawn
                uint32_t              _presented = 0;     // the last present's number
                Stats                 _stats;
                Semaphore             _wake{1, 0};
                WaitListOf<4>         _idle; // woken after each round
                Task                  _task;
};

BMDS_NAMESPACE_END
