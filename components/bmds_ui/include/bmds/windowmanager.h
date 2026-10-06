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
#include <bmds/inputevent.h>
#include <bmds/mutex.h>
#include <bmds/painter.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/spinlock.h>
#include <bmds/stringview.h>
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
 * A window chooses its decorations (`WindowFrame`, `devplan/windows.md`
 * §4f): a border, a title bar, and on it a menu button and minimize,
 * maximize and close buttons. A minimized window is hidden until it is
 * restored or raised; a maximized one fills the screen until restored, and
 * isn't moved meanwhile. A resizable window is sized from the handles on
 * its border, shown as an outline until released (§4g), in the size steps it
 * asked for. A window whose client area changes size is told (`Resized`).
 *
 * Moves are shown as an outline (`drag()`) until `drop()` puts the window
 * there. Focus is the top window: keys go to it (`devplan/input.md`), the
 * window manager being the input's sink while it runs. The mouse moves a
 * pointer drawn over everything, shaped by what is under it (a double arrow
 * over a resize handle, the window's own shape over its client area), shown
 * or hidden on request, and hidden while a window that asks for it is typed
 * into, until the mouse moves again (§4h). The wheel goes to the window
 * under the pointer (§4i). A press raises the window under it, on a title bar
 * starts a move (a double click maximizes or restores), on a handle a
 * resize, and on a button (released there) does what the button does.
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

                /**
                 * @brief Takes a key typed while the window has the focus (on the
                 *        window manager's task, locked: it must not call it).
                 */
                using Key = void (*)(void *arg, const KeyEvent &event);

                /**
                 * @brief Told the client area's new size, @p width × @p height
                 *        (on the changing task, the window manager locked: it
                 *        must not call it). Its whole area is redrawn after.
                 */
                using Resized = void (*)(void *arg, uint16_t width, uint16_t height);

                /**
                 * @brief Takes the wheel turned over the window: @p notches, positive
                 *        away from the user (on the window manager's task, locked: it
                 *        must not call it).
                 */
                using Wheel = void (*)(void *arg, int notches);

                /**
                 * @brief A window's icon: @p width × @p height pixels, 0xRRGGBB, rows
                 *        top first. Kept by the window's owner while it is set.
                 */
                struct Icon {
                                uint16_t        width = 0;
                                uint16_t        height = 0;
                                const uint32_t *pixels = nullptr;
                };

                /** @brief The pointer's shapes. Append only. */
                enum class Pointer : uint8_t {
                        Arrow,
                        Horizontal,   ///< A double arrow: a left or right edge's resize.
                        Vertical,     ///< A top or bottom edge's.
                        DiagonalDown, ///< The top-left and bottom-right corners'.
                        DiagonalUp,   ///< The top-right and bottom-left corners'.
                        Text,         ///< An I-beam: text to select or type into.
                };

                /** @brief Looks at the screen (`capture()`). */
                using Capture = void (*)(void *arg, const Surface &surface);

                /** @brief Key events queued for the window manager's task. */
                static constexpr size_t KeyQueue = 32;

                /** @brief Pointer events queued (motion without a button change is merged). */
                static constexpr size_t PointerQueue = 16;

                /** @brief The longest title kept, in bytes. */
                static constexpr size_t MaxTitle = 47;

                /** @brief How long drawing waits for the 2D-DMA or a swap. */
                static constexpr unsigned int DrawTimeoutMs = 100;

                /** @brief Two presses on a title bar this close together are a double click. */
                static constexpr uint32_t DoubleClickMs = 400;

                /** @brief A window to open. */
                struct WindowConfig {
                                Rect        rect;             ///< Outer, frame included; snapped to 4-pixel columns.
                                const char *title = "";       ///< Copied (`MaxTitle` bytes kept).
                                Redraw      redraw = nullptr; ///< Null: the client area is filled (`Role::Client`).
                                void       *arg = nullptr;
                                Closed      closed = nullptr; ///< May be null.
                                Key         key = nullptr;    ///< May be null: keys dropped.
                                Resized     resized = nullptr;                   ///< May be null.
                                uint8_t     decorations = WindowFrame::Standard; ///< `WindowFrame`'s flags.
                                uint16_t    widthStep = 1;  ///< Resizing by hand: the client width in steps of this.
                                uint16_t    heightStep = 1; ///< And its height.
                                Pointer     pointer = Pointer::Arrow;       ///< The shape over the client area.
                                bool        hidePointerWhileTyping = false; ///< Until the mouse moves again.
                                Wheel       wheel = nullptr;                ///< May be null: the wheel ignored over it.
                };

                /** @brief A window, as `list()` reports it. */
                struct Info {
                                uint8_t id = 0;
                                Rect    rect;
                                char    title[MaxTitle + 1] = {};
                                bool    focused = false;
                                bool    minimized = false;
                                bool    maximized = false;
                                uint8_t decorations = 0;
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
                 *         (smaller than its frame's least, or decorations that
                 *         aren't), or `Full`.
                 */
                Result<uint8_t> open(const WindowConfig &config);

                /** @brief Closes window @p id. @return `Ok`, `NotFound`, `InvalidState`. */
                Error close(uint8_t id);

                /**
                 * @brief Moves window @p id's top-left corner to (@p x, @p y), kept on the screen.
                 * @return `Ok`, `NotFound`, or `InvalidState` (maximized, or not running).
                 */
                Error move(uint8_t id, uint16_t x, uint16_t y);

                /**
                 * @brief Sets window @p id's size to @p width × @p height (outer),
                 *        its top-left corner kept where it fits on the screen.
                 * @return `Ok`, `NotFound`, `Invalid` (smaller than its frame's
                 *         least), or `InvalidState` (maximized, or not running).
                 */
                Error resize(uint8_t id, uint16_t width, uint16_t height);

                /** @brief Puts window @p id on top (focused), restoring it if minimized. */
                Error raise(uint8_t id);

                /** @brief Hides window @p id until it is restored or raised. */
                Error minimize(uint8_t id);

                /** @brief Puts window @p id on top, filling the screen until restored. */
                Error maximize(uint8_t id);

                /**
                 * @brief Shows window @p id again if minimized, else puts it back
                 *        where it was before it was maximized.
                 */
                Error restore(uint8_t id);

                /**
                 * @brief Sets window @p id's title (copied, `MaxTitle` bytes kept, cut
                 *        on a character's boundary) and redraws its title bar.
                 * @return `Ok`, `NotFound`, or `InvalidState` (not running).
                 */
                Error setTitle(uint8_t id, StringView title);

                /**
                 * @brief Sets window @p id's icon (null: none). Kept, not drawn yet:
                 *        the window menu's button and the panel will (§4j).
                 * @return `Ok`, `NotFound`, or `InvalidState` (not running).
                 */
                Error setIcon(uint8_t id, const Icon *icon);

                /**
                 * @brief Sets window @p id's decorations (`WindowFrame`'s flags);
                 *        the outer rectangle stays and the client area changes.
                 * @return `Ok`, `NotFound`, `InvalidState`, or `Invalid` (not
                 *         decorations, or too small a window for them).
                 */
                Error setDecorations(uint8_t id, uint8_t decorations);

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
                 * @brief Queues a key for the focused window, delivered by the
                 *        window manager's task (dropped when the queue is full or
                 *        it is stopped).
                 * @nonblocking
                 */
                void key(const KeyEvent &event);

                /**
                 * @brief Queues a mouse's motion and buttons, applied by the window
                 *        manager's task once a round. The pointer shows from the
                 *        first.
                 * @nonblocking
                 */
                void pointer(const PointerEvent &event);

                /**
                 * @brief Calls @p fn with the screen still: nothing drawn
                 *        meanwhile, the last present shown (a screenshot's copy).
                 *        Keep it short: the desktop waits.
                 * @return `Ok`, `InvalidState` (stopped), or `Timeout` (the swap). @blocking
                 */
                Error capture(Capture fn, void *arg, unsigned int timeoutMs);

                /**
                 * @brief Shows or hides the pointer (it still moves and presses
                 *        while hidden).
                 */
                void setPointerVisible(bool visible);

                /** @brief Returns true unless the pointer was hidden by `setPointerVisible()`. */
                bool isPointerVisible();

                /** @brief Returns where the pointer is drawn (4-pixel columns), or an empty rectangle. */
                Rect pointerArea();

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

                /**
                 * @brief Writes up to @p out's size of windows, top first, then the
                 *        minimized ones (the last minimized first). @return The count.
                 */
                size_t list(Span<Info> out);

                /** @brief Returns window @p id's client area (screen coordinates; minimized too). */
                Result<Rect> clientRect(uint8_t id);

                /**
                 * @brief Waits until every change made before the call is drawn
                 *        and (double-buffered) shown, and every key queued before
                 *        it delivered.
                 * @return `Ok`, `Timeout`, or `InvalidState`.
                 * @blocking
                 */
                Error sync(unsigned int timeoutMs);

                /** @brief Returns the counts. */
                Stats stats();

        private:
                struct Window {
                                char    title[MaxTitle + 1] = {};
                                Redraw  redraw = nullptr;
                                Closed  closed = nullptr;
                                Key     key = nullptr;
                                Resized resized = nullptr;
                                void   *arg = nullptr;
                                Rect    restore;         // where it was before it was maximized
                                uint8_t  decorations = 0; // WindowFrame's flags
                                bool     maximized = false;
                                uint16_t widthStep = 1; // a resize by hand's client steps
                                uint16_t heightStep = 1;
                                Pointer  pointer = Pointer::Arrow;
                                bool     hideWhileTyping = false;
                                const Icon *icon = nullptr; // not drawn yet
                                Wheel    wheel = nullptr;
                };

                static constexpr size_t MaxPieces = 256;

                static void entry(void *arg);
                static void keyEntry(void *arg, const KeyEvent &event);
                static void pointerEntry(void *arg, const PointerEvent &event);
                static void drawCursorPixels(void *arg, uint8_t *pixels, size_t stride);
                void        deliverKeys();
                void        applyPointer();
                void        press(uint16_t x, uint16_t y);
                void        release(uint16_t x, uint16_t y);
                void        pressedMoved(uint16_t x, uint16_t y); // a button's press shown while over it
                void        sizeTo(uint16_t x, uint16_t y);       // a resize under way: the outline there
                Pointer     shapeAt(uint16_t x, uint16_t y) const;
                bool        isCursorDrawn() const { return _cursorShown && !_pointerHidden && !_typingHidden; }
                void        act(uint8_t id, WindowFrame::Part part);
                WindowFrame frame(uint8_t id, const Rect &outer) const;
                void        drawCursor();
                Rect        cursorArea() const; // where the pointer is drawn: on the screen, 4-pixel columns
                uint8_t     windowAt(uint16_t x, uint16_t y) const; // 0: the desktop
                Error       closeLocked(uint8_t id);
                Error       raiseLocked(uint8_t id);
                Error       minimizeLocked(uint8_t id);
                Error       maximizeLocked(uint8_t id);
                Error       restoreLocked(uint8_t id);
                Error       resizeLocked(uint8_t id, const Rect &rect, uint8_t decorations); // told if it changed
                Error       dragLocked(uint8_t id, uint16_t x, uint16_t y);
                Error       outlineLocked(uint8_t id, const Rect &rect);
                Error       dropLocked();
                void        run();
                void        round(uint32_t requests);
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
                SpinLock              _inputLock;         // the two queues
                KeyEvent              _keys[KeyQueue];
                uint8_t               _keyHead = 0; // the next to deliver
                uint8_t               _keyCount = 0;
                PointerEvent          _pointers[PointerQueue];
                uint8_t               _pointerHead = 0;
                uint8_t               _pointerCount = 0;
                uint16_t              _pointerX = 0; // the pointer's tip
                uint16_t              _pointerY = 0;
                uint8_t               _buttons = 0;         // down, as last applied
                bool                  _cursorShown = false; // a mouse has moved it
                bool                  _cursorMoved = false; // to draw this round
                uint8_t               _grabId = 0;          // its title bar pressed: moved while held
                uint16_t              _grabX = 0;           // the press, from the window's corner
                uint16_t              _grabY = 0;
                Pointer               _shape = Pointer::Arrow;
                bool                  _pointerHidden = false; // by setPointerVisible()
                bool                  _typingHidden = false;  // a key went to a window asking: until a move
                uint8_t               _sizeId = 0;    // its resize handle pressed: resized while held
                uint8_t               _sizeEdges = 0; // the edges it holds
                Rect                  _sizeStart;     // the window at the press
                uint16_t              _sizeX = 0;     // the press
                uint16_t              _sizeY = 0;
                uint8_t               _pressId = 0; // its button pressed
                WindowFrame::Part     _pressPart = WindowFrame::Part::Outside;
                bool                  _pressShown = false; // drawn pressed: the pointer on it
                uint8_t               _clickId = 0;        // the last title bar press, for a double click
                uint64_t              _clickUs = 0;
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
