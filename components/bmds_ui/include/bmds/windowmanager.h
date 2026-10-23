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
#include <bmds/dragdrop.h>
#include <bmds/error.h>
#include <bmds/icon.h>
#include <bmds/inputevent.h>
#include <bmds/list.h>
#include <bmds/menulayout.h>
#include <bmds/mutex.h>
#include <bmds/painter.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/servicequeue.h>
#include <bmds/spinlock.h>
#include <bmds/stream.h>
#include <bmds/stringview.h>
#include <bmds/span.h>
#include <bmds/stats.h>
#include <bmds/switcherlayout.h>
#include <bmds/task.h>
#include <bmds/waitlist.h>
#include <bmds/windowpalette.h>
#include <bmds/windowplacement.h>
#include <bmds/windowstack.h>

BMDS_NAMESPACE_BEGIN

class Display;
class ProgramLauncher;
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
 * there. Focus is the window raised last: keys go to it (`devplan/input.md`), the
 * window manager being the input's sink while it runs. The mouse moves a
 * pointer drawn over everything, shaped by what is under it (a double arrow
 * over a resize handle, the window's own shape over its client area), shown
 * or hidden on request, and hidden while a window that asks for it is typed
 * into, until the mouse moves again (§4h). The wheel goes to the window
 * under the pointer (§4i). A press raises the window under it, on a title bar
 * starts a move (a double click maximizes or restores), on a handle a
 * resize, and on a button (released there) does what the button does.
 *
 * Windows are on levels (`Level`, `devplan/gui.md` §4a): a desktop window
 * under everything, applications' windows, panels above them, popups above
 * all. Focus is the focusable window raised last (so a window kept above the
 * others doesn't take it from one raised under it); panels and desktop windows
 * never take it. A popup takes the keys while it is open, and a press outside every
 * popup closes them all (the press goes no further). A panel reserves the
 * strip of the screen it covers: maximized windows fill the rest (the work
 * area). A window that asks gets the mouse in its client area (`Mouse`): a
 * press there sends it the motion and the release too, until every button is
 * up. The title bar's menu button, or `showWindowMenu()`, opens the window's
 * menu. A connection may watch the list of windows (`watch()`).
 *
 * Keys the window manager keeps (`devplan/gui.md` §4g): Alt+Tab and
 * Shift+Alt+Tab show a list of the windows in the screen's middle and move
 * through it, the window chosen raised when Alt is released (Escape: none);
 * Alt+F4 closes the focused window, Alt+Space opens its menu. Alt and a
 * left press anywhere in a window moves it. A window may be kept above the
 * others (`setAbove()`, the window menu's "Always on Top"). A program may
 * claim a key (`grabKey()`: the panel takes the Windows key for its
 * launcher).
 *
 * A window dragged by its title bar to the work area's top edge is maximized
 * when dropped, to a side, it fills that half (snapped; dragged away again,
 * it takes back its size). A window may be rolled up to its title bar
 * (`shade()`, the window menu, the wheel over its title bar): its client
 * area hidden, its application not told.
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

                /** @brief Where a window is when it isn't maximized, and whether it is (`WindowPlacement`). */
                using Placement = WindowPlacement;

                /**
                 * @brief Told that the window closed, and where it was (its placement), as
                 *        `Closed` (in its place when set).
                 */
                using ClosedAt = void (*)(void *arg, const Placement &placement);

                /**
                 * @brief Told that the user asked to close the window (its close box, its menu's
                 *        Close, Alt+F4), on the window manager's task, locked: the window stays
                 *        until its owner closes it (`devplan/guiprograms.md` P5). Without one, the
                 *        window manager closes it.
                 */
                using CloseRequest = void (*)(void *arg);

                /**
                 * @brief Takes a key typed while the window has the focus (on the
                 *        window manager's task, locked: it must not call it).
                 */
                using Key = void (*)(void *arg, const KeyEvent &event);

                /**
                 * @brief Takes a gamepad's event while the window has the focus (on the window
                 *        manager's task, locked: it must not call it). What was down when the
                 *        focus came or went is the window's to settle (`Activated`).
                 */
                using Pad = void (*)(void *arg, const PadEvent &event);

                /**
                 * @brief Told that the window gained (@p active) or lost the keys (on
                 *        the changing task, the window manager locked: it must not call
                 *        it).
                 */
                using Activated = void (*)(void *arg, bool active);

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
                 * @brief A window's icon (`bmds::Icon`: 0xAARRGGBB), kept by the
                 *        window's owner while it is set. 16 × 16 is drawn (the title
                 *        bar, Alt+Tab's list); a panel copies it (`copyIcon()`).
                 */
                using Icon = bmds::Icon;

                /**
                 * @brief Told that the window manager stopped (on the stopping
                 *        task, locked: it must not call it); the connection's
                 *        windows were closed first.
                 */
                using Gone = void (*)(void *arg);

                /**
                 * @brief Asked to show the window full screen (@p on) or back in its window (on
                 *        the window manager's task, locked: it must not call it). Alt+Enter asks
                 *        either way; an escape key (Alt+Tab, Alt+F4, Alt+Space, a Windows key)
                 *        asks a full-screen window to leave before doing what it does. The owner
                 *        switches, then says so with `setFullScreen()`.
                 */
                using FullScreen = void (*)(void *arg, bool on);

                /** @brief Writes the window's contents as text to @p out (`win dump`; a test aid). */
                using Dump = void (*)(void *arg, Stream &out);

                /**
                 * @brief Draws what the window's client area should show into @p pixels
                 *        (B, G, R, @p stride bytes a row; `win check`; a test aid).
                 */
                using Reference = void (*)(void *arg, uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                           const WindowPalette &palette);

                /** @brief What a window's mouse callback is told. */
                struct MouseEvent {
                                /** @brief What happened. */
                                enum class Type : uint8_t {
                                        Press,   ///< A button went down over the client area.
                                        Release, ///< A button came up (after a press there, anywhere).
                                        Motion,  ///< The pointer moved over it (or anywhere, a button held).
                                        Leave,   ///< The pointer left it.
                                };

                                Type    type = Type::Motion;
                                int16_t x = 0;         ///< From the client area's top-left corner.
                                int16_t y = 0;         ///< Outside it while a press is held: negative or past it.
                                uint8_t button = 0;    ///< `PointerEvent::Button`: the one pressed or released.
                                uint8_t buttons = 0;   ///< `PointerEvent::Button` bits down after it.
                                uint8_t modifiers = 0; ///< `KeyEvent::Modifier` bits down with it.
                                int16_t dx = 0; ///< `Motion`: the mouse's, in screen pixels (held or at an edge too).
                                int16_t dy = 0;
                };

                /**
                 * @brief Takes the mouse over the window's client area (on the window
                 *        manager's task, locked: it must not call it).
                 */
                using Mouse = void (*)(void *arg, const MouseEvent &event);

                /** @brief Window levels, lowest first (`WindowStack::Level`). */
                using Level = WindowStack::Level;

                /**
                 * @brief Told that the list of windows changed (opened, closed,
                 *        titled, focused, minimized, restored, maximized, an icon
                 *        set) or the work area did, once a round at most (on the window manager's task,
                 *        locked: it must not call it).
                 */
                using Watch = void (*)(void *arg);

                /**
                 * @brief Told that item @p index of the desktop's menu was chosen
                 *        (`setDesktopMenu()`; on the window manager's task, its lock
                 *        released).
                 */
                using DesktopChosen = void (*)(void *arg, size_t index);

                /** @brief The desktop menu's items at most. */
                static constexpr size_t MaxDesktopItems = 10;

                /** @brief The longest desktop menu label kept, in bytes. */
                static constexpr size_t MaxDesktopLabel = 31;

                /** @brief An item of a program's menu (`showMenu()`). */
                struct MenuItem {
                                StringView
                                        label; ///< Its text (cut at `MaxMenuLabel` bytes); `&` marks its key (`Mnemonic`).
                                StringView  shortcut;  ///< Its key, shown at the right (cut at `MaxMenuShortcut`).
                                uint8_t     flags = 0; ///< `MenuLayout::Flag`s: `Disabled`, `Separator`, `Checked`.
                                const Icon *icon = nullptr; ///< 16 × 16 at its left (null: none), kept while it shows.
                };

                /**
                 * @brief Told that item @p index of a program's menu (`showMenu()`) was
                 *        chosen (on the window manager's task, its lock released).
                 */
                using MenuChosen = void (*)(void *arg, size_t index);

                /** @brief A program's menu's items at most. */
                static constexpr size_t MaxMenuItems = 10;

                /** @brief The longest program's menu label kept, in bytes. */
                static constexpr size_t MaxMenuLabel = 31;

                /** @brief The longest program's menu shortcut kept, in bytes. */
                static constexpr size_t MaxMenuShortcut = 15;

                /** @brief The index `DesktopChosen` is told for Ctrl+Shift+Windows (`devplan/gui.md` §4p). */
                static constexpr size_t DesktopKey = MaxDesktopItems;

                /** @brief Connections at once: every number a byte holds (0 is none). */
                static constexpr size_t MaxClients = 255;

                /** @brief The longest kind kept, in bytes. */
                static constexpr size_t MaxKind = 15;

                /** @brief The pointer's shapes. Append only. */
                enum class Pointer : uint8_t {
                        Arrow,
                        Horizontal,   ///< A double arrow: a left or right edge's resize.
                        Vertical,     ///< A top or bottom edge's.
                        DiagonalDown, ///< The top-left and bottom-right corners'.
                        DiagonalUp,   ///< The top-right and bottom-left corners'.
                        Text,         ///< An I-beam: text to select or type into.
                        None,         ///< None drawn (a game's own, or none).
                        Starting,     ///< The arrow with an hourglass: a program starting (`programStarting()`).
                        DragMove,     ///< A drag that moves here (`startDrag()`): the arrow and a dotted box.
                        DragCopy,     ///< A drag that copies here: the arrow and a plus.
                        DragLink,     ///< A drag that links here: the arrow and a bent arrow.
                        DragNone,     ///< A drag that can't drop here: a slashed circle.
                };

                /** @brief Programs shown starting at once (`programStarting()`): a newer replaces the oldest. */
                static constexpr size_t MaxStarting = 4;

                /** @brief How long a program is shown starting at most, without a window. */
                static constexpr unsigned int StartingTimeoutMs = 20000;

                /** @brief The starting pointer's frames: the sand turns this often. */
                static constexpr unsigned int StartingFrameMs = 250;

                /** @brief Looks at the screen (`capture()`). */
                using Capture = void (*)(void *arg, const Surface &surface);

                /** @brief Key events queued for the window manager's task. */
                static constexpr size_t KeyQueue = 32;

                /** @brief Gamepads' events queued for the window manager's task. */
                static constexpr size_t PadQueue = 32;

                /** @brief Pointer events queued (motion without a button change is merged). */
                static constexpr size_t PointerQueue = 16;

                /** @brief The longest title kept, in bytes. */
                static constexpr size_t MaxTitle = 47;

                /** @brief How long drawing waits for the 2D-DMA or a swap. */
                static constexpr unsigned int DrawTimeoutMs = 100;

                /** @brief Two presses on a title bar this close together are a double click. */
                static constexpr uint32_t DoubleClickMs = 400;

                /**
                 * @brief Told that drag @p drag ended (`startDrag()`): @p done is what the
                 *        target did (`None`: refused or cancelled; `Move`: the source removes
                 *        its own). On the window manager's task, its lock released.
                 */
                using DragEnded = void (*)(void *arg, uint32_t drag, DropAction done);

                /** @brief A drag to start (`startDrag()`, `devplan/clipboard.md` §5c). */
                struct DragConfig {
                                uint32_t        offer = 0; ///< The `Drag` selection's serial: passed to targets.
                                uint8_t         actions = DropActions::CanCopy; ///< What the source allows.
                                DropAction      preferred = DropAction::Copy; ///< The source's choice, all else equal.
                                const uint32_t *image =
                                        nullptr;     ///< 0xAARRGGBB, `width` × `height`; copied (null: none).
                                uint8_t   width = 0; ///< `DragImage::Max` at most.
                                uint8_t   height = 0;
                                uint8_t   hotX = 0; ///< The pointer's place in the image (kept inside it).
                                uint8_t   hotY = 0;
                                DragEnded ended = nullptr; ///< May be null.
                                void     *arg = nullptr;
                };

                /** @brief What a drop target is told (`WindowConfig::drop`). */
                struct DropEvent {
                                /** @brief What happened. */
                                enum class Type : uint8_t {
                                        Enter,  ///< The drag came over the client area.
                                        Motion, ///< It moved there, or a modifier changed.
                                        Leave,  ///< It left (or ended there, cancelled).
                                        Drop,   ///< It was let go there: read, then `finishDrop()`.
                                };

                                Type       type = Type::Motion;
                                uint32_t   drag = 0;  ///< `startDrag()`'s number: for the answers.
                                uint32_t   offer = 0; ///< The `Drag` selection's serial.
                                int16_t    x = 0;     ///< From the client area's top-left corner.
                                int16_t    y = 0;
                                uint8_t    actions = 0;   ///< `DropActions` bits the source allows.
                                uint8_t    modifiers = 0; ///< `KeyEvent::Modifier` bits down.
                                bool       ask = false;   ///< A right drag's `Drop`: the target asks which action.
                                bool       own = false;   ///< The source is this window's application.
                                DropAction preferred = DropAction::Copy; ///< The source's choice.
                };

                /**
                 * @brief Takes a drag over the window's client area (on the window manager's
                 *        task, locked: it must not call it). Answers come later (`answerDrop()`).
                 */
                using Drop = void (*)(void *arg, const DropEvent &event);

                /** @brief How long after a drop the target has to `finishDrop()`. */
                static constexpr unsigned int DropTimeoutMs = 10000;

                /** @brief A window to open. */
                struct WindowConfig {
                                Rect        rect;             ///< Outer, frame included; snapped to 4-pixel columns.
                                const char *title = "";       ///< Copied (`MaxTitle` bytes kept).
                                Redraw      redraw = nullptr; ///< Null: the client area is filled (`Role::Client`).
                                void       *arg = nullptr;
                                Closed      closed = nullptr;                    ///< May be null.
                                Key         key = nullptr;                       ///< May be null: keys dropped.
                                Resized     resized = nullptr;                   ///< May be null.
                                uint8_t     decorations = WindowFrame::Standard; ///< `WindowFrame`'s flags.
                                uint16_t    widthStep = 1;  ///< Resizing by hand: the client width in steps of this.
                                uint16_t    heightStep = 1; ///< And its height.
                                Pointer     pointer = Pointer::Arrow;       ///< The shape over the client area.
                                bool        hidePointerWhileTyping = false; ///< Until the mouse moves again.
                                Wheel       wheel = nullptr;                ///< May be null: the wheel ignored over it.
                                uint8_t     client = 0;     ///< Its application's connection (`connect()`); 0: none.
                                const char *kind = nullptr; ///< What it is, for `win list` (copied, `MaxKind` bytes).
                                Dump        dump = nullptr; ///< May be null.
                                Reference   reference = nullptr; ///< May be null.
                                Mouse       mouse = nullptr;     ///< May be null: the mouse not reported.
                                Level       level = Level::Normal;
                                uint16_t    reserve = 0; ///< A panel's: the strip it reserves (at the top or bottom).
                                bool        focusable = true; ///< False: never focused. A desktop's or a panel's
                                                              ///< takes the keys when pressed or raised, never in
                                                              ///< Alt+Tab (`devplan/gui.md` §4r).
                                bool cascade = false;         ///< A normal window placed by the window manager
                                                              ///< (`cascadeRect()`): `rect`'s size kept, its place not.
                                bool       focusOnPress = true;  ///< False: a press doesn't focus it (`raise()` does).
                                Activated  activated = nullptr;  ///< May be null.
                                ClosedAt   closedAt = nullptr;   ///< May be null; told instead of `closed`.
                                FullScreen fullScreen = nullptr; ///< May be null: Alt+Enter does nothing to it.
                                Pad        pad = nullptr;        ///< May be null: pads' events dropped.
                                Drop       drop = nullptr;       ///< May be null: drags show they can't drop.
                                CloseRequest closeRequested = nullptr; ///< May be null: the user's close closes it.
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
                                char    kind[MaxKind + 1] = {}; ///< Empty: none.
                                Level   level = Level::Normal;
                                bool    hasIcon = false;
                                bool    shaded = false; ///< Rolled up to its title bar.
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
                                uint32_t deferred = 0; ///< Redraws put off (`Painter::defer()`).
                                uint32_t windows = 0;  ///< Windows now, shown and hidden (not a count).
                                uint32_t jobs = 0;     ///< 2D-DMA jobs run on the desktop's surface (not a count).
                };

                /** @brief Window managers: one per display. */
                static constexpr size_t MaxManagers = 1;

                /**
                 * @brief Returns display @p number's window manager.
                 * @return It, or `NotSupported` (no such display).
                 */
                static Result<WindowManager *> manager(size_t number);

                WindowManager();
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
                 * @brief Connects a GUI application (`devplan/windows.md` §8 step 9):
                 *        its windows (`WindowConfig::client`) close with it, and
                 *        @p gone is called if the window manager stops first.
                 * @return Its connection, `InvalidState` (not running: no window
                 *         system), `Full` or `NoMem`.
                 */
                Result<uint8_t> connect(Gone gone, void *arg);

                /** @brief Ends connection @p client: its windows close. */
                void disconnect(uint8_t client);

                /**
                 * @brief Has @p fn told when the list of windows changes (null:
                 *        no more), for connection @p client.
                 * @return `Ok`, `NotFound` (no such connection), `InvalidState`.
                 */
                Error watch(uint8_t client, Watch fn, void *arg);

                /**
                 * @brief Opens window @p id's menu (Restore, Minimize, Maximize,
                 *        Close) with its top-left corner at (@p x, @p y), kept on
                 *        the screen.
                 * @return `Ok`, `NotFound`, or `InvalidState`.
                 */
                Error showWindowMenu(uint8_t id, uint16_t x, uint16_t y);

                /**
                 * @brief Sets the menu a right press opens where no window is
                 *        (`devplan/gui.md` §4o): @p labels (copied, `MaxDesktopItems`
                 *        at most, cut at `MaxDesktopLabel` bytes; none: no menu),
                 *        @p fn told which was chosen. A desktop-level window (a panel's)
                 *        covering the desktop takes the press instead. Kept until
                 *        `stop()`.
                 * @return `Ok`, `InvalidState` (not running), or `Invalid` (too many).
                 */
                Error setDesktopMenu(Span<const StringView> labels, DesktopChosen fn, void *arg);

                /**
                 * @brief Opens a menu of @p items (copied, `MaxMenuItems` at most) for
                 *        window @p id, its top-left corner at (@p x, @p y) of the
                 *        window's client area, kept on the screen: drawn and run as the
                 *        window manager's own menus (`devplan/files.md` §4c), for a
                 *        program not on the toolkit. @p fn is told the item chosen;
                 *        nothing when it is dismissed. One menu at a time: it closes
                 *        another, and closes with its window.
                 * @return `Ok`, `NotFound` (no such window), `Invalid` (no items, too
                 *         many), or `InvalidState` (not running).
                 */
                Error showMenu(uint8_t id, Span<const MenuItem> items, uint16_t x, uint16_t y, MenuChosen fn,
                               void *arg);

                /**
                 * @brief Returns true if windows can be minimized: a program watches
                 *        the window list (`watch()`), so it can bring them back
                 *        (`devplan/gui.md` §4p).
                 */
                bool canMinimize();

                /** @brief Returns the screen less the strips panels reserve. */
                Rect workArea();

                /** @brief Returns the screen (empty when not running). */
                Rect screen();

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
                 * @brief Asks to close window @p id as the user's close does (its close box): its
                 *        owner is asked (`CloseRequest`), or it closes (a panel's window list,
                 *        `devplan/guiprograms.md` §12 4c).
                 * @return `Ok`, `NotFound`, `InvalidState`.
                 */
                Error requestClose(uint8_t id);

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

                /**
                 * @brief Hides window @p id until it is restored or raised.
                 * @return `Ok`, `NotFound`, or `InvalidState` (not a normal window: a panel's).
                 */
                Error minimize(uint8_t id);

                /**
                 * @brief Keeps window @p id above the other applications' windows
                 *        ("always on top"), or not.
                 * @return `Ok`, `NotFound`, or `InvalidState` (not an application's window).
                 */
                Error setAbove(uint8_t id, bool above);

                /**
                 * @brief Rolls window @p id up to its title bar (its client area
                 *        hidden, its application not told: its size is the same),
                 *        or down again.
                 * @return `Ok`, `NotFound`, or `InvalidState` (no title bar,
                 *         minimized, or not an application's window).
                 */
                Error shade(uint8_t id, bool shaded);

                /** @brief Told a key claimed with `grabKey()` (on the window manager's task, locked). */
                using Grab = void (*)(void *arg, uint8_t usage);

                /** @brief Key claims at once. */
                static constexpr size_t MaxGrabs = 8;

                /**
                 * @brief Claims a key for connection @p client: HID @p usage with
                 *        exactly @p modifiers (`KeyEvent::Modifier`; left and right
                 *        alike) on its press. A modifier key with no modifiers (the
                 *        Windows key, `0xe3`/`0xe7`) is claimed as a tap: told on its
                 *        release when no other key came while it was down. Claimed
                 *        keys aren't delivered to windows. Released with the
                 *        connection.
                 * @return `Ok`, `NotFound` (no such connection), `Exists`, `Full`.
                 */
                Error grabKey(uint8_t client, uint8_t usage, uint8_t modifiers, Grab fn, void *arg);

                /**
                 * @brief Told that a message for a service is waiting (`registerService()`),
                 *        on the sender's task with the window manager locked: it only wakes
                 *        a loop, which takes it with `takeServiceMessage()`.
                 */
                using ServiceNotify = void (*)(void *arg);

                /** @brief A service's longest name. */
                static constexpr size_t MaxServiceName = 31;

                /** @brief A service message's most bytes. */
                static constexpr size_t MaxServiceMessage = ServiceQueue::MaxMessage;

                /**
                 * @brief Names a service of connection @p client (`devplan/filebrowser.md`
                 *        §4h, FB6): messages sent to @p name wait for it, @p fn told of
                 *        each. Released with the connection.
                 * @return `Ok`, `Invalid` (the name: empty, too long; a null @p fn),
                 *         `NotFound` (no such connection), `Exists` (named already) or `NoMem`.
                 */
                Error registerService(uint8_t client, StringView name, ServiceNotify fn, void *arg);

                /**
                 * @brief Sends @p message to service @p name (any task).
                 * @return `Ok`, `NotFound` (no such service), `Invalid` (empty, or over
                 *         `MaxServiceMessage`), `Full` (too much not taken yet), `NoMem` or
                 *         `InvalidState` (no window system).
                 */
                Error sendToService(StringView name, ByteSpan message);

                /**
                 * @brief Moves the oldest message for connection @p client's service
                 *        @p name into @p out.
                 * @return Its bytes (0: none), `NotFound` (not its service) or
                 *         `BufferTooSmall` (kept).
                 */
                Result<size_t> takeServiceMessage(uint8_t client, StringView name, WritableByteSpan out);

                /**
                 * @brief Forgets connection @p client's service @p name, with the
                 *        messages waiting for it: no notification for it comes after.
                 * @return `Ok`, or `NotFound` (not its service).
                 */
                Error unregisterService(uint8_t client, StringView name);

                /** @brief Returns true if a service is named @p name (any task). */
                bool hasService(StringView name);

                /**
                 * @brief Has @p fn told when a service is named or goes (null: no
                 *        more), for connection @p client (`devplan/mediaplayer.md`
                 *        §6e): on the task that changed them, the window manager
                 *        locked: it only wakes a loop, which asks `hasService()`.
                 * @return `Ok`, `NotFound` (no such connection), `InvalidState`.
                 */
                Error watchServices(uint8_t client, ServiceNotify fn, void *arg);

                /**
                 * @brief Puts window @p id on top, filling the work area until restored.
                 * @return `Ok`, `NotFound`, or `InvalidState` (not a normal window: a panel's).
                 */
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
                 * @brief Sets window @p id's icon (null: none), kept by the caller
                 *        until another is set or the window closes. A 16 × 16 one
                 *        replaces the title bar's menu button (as the classic title
                 *        bars showed theirs) and shows in Alt+Tab's list.
                 * @return `Ok`, `NotFound`, or `InvalidState` (not running).
                 */
                Error setIcon(uint8_t id, const Icon *icon);

                /**
                 * @brief Sets the pointer's shape over window @p id's client area
                 *        (`WindowConfig::pointer` at first): a widget's under it. Called
                 *        while a round draws, it doesn't wait: the shape is taken at the next
                 *        round (an application holding its own lock mustn't wait for ours).
                 * @return `Ok`, `NotFound`, or `InvalidState` (not running); `Ok` when put off.
                 */
                Error setPointer(uint8_t id, Pointer shape);

                /**
                 * @brief Shows that program @p pid is starting (`devplan/windows.md` §4l): the
                 *        arrow becomes `Pointer::Starting` until a process of session @p pid opens
                 *        an application window, the program is gone (@p launcher's
                 *        `sendSignal(pid, 0)`), or @p timeoutMs pass. For a program started as
                 *        the leader of a new session (the panel's launches).
                 * @param launcher Kept until then: one that lasts (the loader).
                 * @nonblocking
                 */
                void programStarting(int32_t pid, ProgramLauncher *launcher, unsigned int timeoutMs);

                /**
                 * @brief Locks the pointer to window @p id (@p on), or lets it go: while the
                 *        window is focused (full screen or not), the pointer is hidden and held
                 *        still, and every pointer event goes to the window, motion as `dx`, `dy`
                 *        (mouse look). Focus elsewhere (Alt+Tab, the Windows key) lets it go until
                 *        the window is focused again. One window at a time: the last locked.
                 * @return `Ok`, `NotFound`, or `InvalidState` (not running).
                 */
                Error setPointerLock(uint8_t id, bool on);

                /** @brief Told its window's own menu item was chosen (on the window manager's task, unlocked). */
                using WindowItemChosen = void (*)(void *arg);

                /** @brief Bytes of a window's own menu item's label kept. */
                static constexpr size_t MaxWindowItem = 23;

                /**
                 * @brief Gives window @p id an item of its own in its window menu (Alt+Space), after
                 *        Full Screen: @p label (`&` marking its key; empty: none), @p fn told when
                 *        chosen (a program's Controls..., `devplan/controls.md` §7d).
                 * @return `Ok`, `NotFound`.
                 */
                Error setWindowItem(uint8_t id, StringView label, WindowItemChosen fn, void *arg);

                /**
                 * @brief Keeps window @p id's client area at least @p width × @p height when the
                 *        user resizes it (0: no minimum but the frame's).
                 * @return `Ok`, `NotFound`.
                 */
                Error setMinimumSize(uint8_t id, uint16_t width, uint16_t height);

                /** @brief The arrow pointer's picture and colours, for drawing it elsewhere. */
                struct CursorPicture {
                                uint8_t            width = 0;
                                uint8_t            height = 0;
                                uint8_t            hotX = 0; ///< The tip, in the picture.
                                uint8_t            hotY = 0;
                                const char *const *rows =
                                        nullptr;   ///< A row's characters: `X` edge, `o` fill, `.` clear.
                                uint32_t edge = 0; ///< 0xRRGGBB.
                                uint32_t fill = 0;
                };

                /**
                 * @brief Returns the arrow's picture in the palette's colours: what a full-screen
                 *        surface draws over its own picture, the desktop not showing.
                 */
                CursorPicture arrowPicture();

                /** @brief Returns where the pointer's tip is, on the desktop (`ScreenWidth` across). */
                void pointerPosition(uint16_t &x, uint16_t &y);

                /**
                 * @brief Starts a drag from window @p id (`devplan/clipboard.md` §5c): the
                 *        pointer is the drag's until its button comes up. The source made its
                 *        offer on the `Drag` selection first. The window gets a `Leave`, then no
                 *        more of the mouse until the drag ends.
                 * @return The drag's number, or `InvalidState` (no button down, or the press
                 *         wasn't in @p id), `Busy` (another drag, or one waiting for its
                 *         finish), `Invalid` (an image too big), `NoMem`.
                 */
                Result<uint32_t> startDrag(uint8_t id, const DragConfig &config);

                /**
                 * @brief Window @p id's answer to drag @p drag where it is now (`None`: it
                 *        can't drop there): the pointer shows it. The latest answer counts.
                 * @return `Ok`, or `InvalidState` (the drag ended, or isn't over @p id now).
                 */
                Error answerDrop(uint32_t drag, uint8_t id, DropAction action);

                /**
                 * @brief Says what the target did with drag @p drag's drop: the source is
                 *        told @p done.
                 * @return `Ok`, or `InvalidState` (not waiting for a finish: ended, or timed out).
                 */
                Error finishDrop(uint32_t drag, DropAction done);

                /**
                 * @brief Ends drag @p drag as cancelled (the source gives up).
                 * @return `Ok`, or `InvalidState` (ended).
                 */
                Error cancelDrag(uint32_t drag);

                /** @brief A drag's state, as `win drag` shows it. */
                struct DragInfo {
                                uint32_t   drag = 0; ///< 0: none.
                                uint8_t    source = 0;
                                uint32_t   offer = 0;
                                uint8_t    actions = 0;
                                uint8_t    target = 0; ///< The window under it that takes drops; 0: none.
                                DropAction answer = DropAction::None;
                                bool       dropped = false; ///< Let go: waiting for the target's finish.
                };

                /** @brief Returns the drag under way (`drag` 0: none). */
                DragInfo dragInfo();

                /**
                 * @brief Sets window @p id's large icon (null: none; 32 × 32 at
                 *        most), kept by the caller as `setIcon()`'s: Alt+Tab's list
                 *        shows it (the small one doubled without it).
                 * @return `Ok`, `NotFound`, `InvalidState`, or `Invalid` (too big).
                 */
                Error setLargeIcon(uint8_t id, const Icon *icon);

                /**
                 * @brief Copies window @p id's icon's pixels into @p out (a window
                 *        list's buttons; another program's icon can't be kept).
                 * @return Its size; `NotFound`, `NotSupported` (it has none), or
                 *         `BufferTooSmall`.
                 */
                Result<Icon> copyIcon(uint8_t id, Span<uint32_t> out);

                /**
                 * @brief Sets window @p id's decorations (`WindowFrame`'s flags);
                 *        the outer rectangle stays and the client area changes.
                 * @return `Ok`, `NotFound`, `InvalidState`, or `Invalid` (not
                 *         decorations, or too small a window for them).
                 */
                Error setDecorations(uint8_t id, uint8_t decorations);

                /**
                 * @brief Marks window @p id full screen (@p on): its owner shows it on the display
                 *        itself (`devplan/games.md` §10c), so the window manager gives it every
                 *        pointer event, in the screen's coordinates, and asks it to leave
                 *        (`FullScreen`) before an escape key. Off: back to normal.
                 * @return `Ok`, or `NotFound` (no such window).
                 */
                Error setFullScreen(uint8_t id, bool on);

                /** @brief Asks window @p id to redraw @p rect of its client area. */
                Error invalidate(uint8_t id, const Rect &rect);

                /**
                 * @brief Asks window @p id to redraw @p rects of its client area, all in
                 *        one round (none drawn without the others).
                 */
                Error invalidate(uint8_t id, Span<const Rect> rects);

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

                /** @brief Queues a key for window @p id rather than the focused one (`win type`). */
                void keyTo(uint8_t id, const KeyEvent &event);

                /** @brief Writes window @p id's contents as text. @return `Ok`, `NotFound`, or `NotSupported`. */
                Error dump(uint8_t id, Stream &out);

                /**
                 * @brief Draws what window @p id's client area should show.
                 * @return `Ok`, `NotFound`, or `NotSupported` (the window offers none).
                 */
                Error reference(uint8_t id, uint8_t *pixels, size_t stride, uint16_t width, uint16_t height);

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
                 * @brief Shows a rubber band over window @p id's client area at @p rect
                 *        (client coordinates; empty: none), drawn by the window manager
                 *        over what the window draws, where it shows: a light line inside
                 *        a dark one (seen on any colour, as a move's outline). Moving it
                 *        has what it covered redrawn by whoever shows there. One band at
                 *        a time: another window's goes.
                 * @return `Ok`, `NotFound`, or `InvalidState`.
                 */
                Error setBand(uint8_t id, const Rect &rect);

                /**
                 * @brief Returns the windows, top first, then the minimized ones
                 *        (the last minimized first), in a PSRAM list of their own.
                 * @return The list, or `NoMem`.
                 */
                Result<List<Info>> windows();

                /**
                 * @brief Returns true if window @p id is minimized (false: shown, or no such window).
                 *        While a round draws, as at its start (it doesn't wait).
                 */
                bool isMinimized(uint8_t id);

                /** @brief Returns true if window @p id is rolled up to its title bar (`shade()`). */
                bool isShaded(uint8_t id);

                /** @brief Returns window @p id's outer rectangle (screen coordinates; minimized too). */
                Result<Rect> rect(uint8_t id);

                /** @brief Returns window @p id's placement: its normal rectangle and whether it is maximized. */
                Result<Placement> placement(uint8_t id);

                /**
                 * @brief Returns window @p id's client area (screen coordinates; minimized too).
                 *        While a round draws, as at its start (it doesn't wait).
                 */
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
                                char        title[MaxTitle + 1] = {};
                                Redraw      redraw = nullptr;
                                Closed      closed = nullptr;
                                Key         key = nullptr;
                                Pad         pad = nullptr;
                                Resized     resized = nullptr;
                                WindowItemChosen itemFn = nullptr; // its own window menu item
                                void            *itemArg = nullptr;
                                char             itemLabel[MaxWindowItem + 1] = {};
                                void       *arg = nullptr;
                                Rect        restore;         // where it was before it was maximized or snapped
                                uint8_t     decorations = 0; // WindowFrame's flags
                                bool        maximized = false;
                                bool        snapped = false; // to half of the work area
                                bool        shaded = false;  // rolled up to its title bar
                                uint16_t    shadeHeight = 0; // its height unrolled
                                uint16_t    widthStep = 1;   // a resize by hand's client steps
                                uint16_t    heightStep = 1;
                                Pointer     pointer = Pointer::Arrow;
                                bool        hideWhileTyping = false;
                                const Icon *icon = nullptr;      // the owner's
                                const Icon *largeIcon = nullptr; // the owner's
                                Wheel       wheel = nullptr;
                                uint8_t     client = 0;
                                char        kind[MaxKind + 1] = {};
                                Dump        dump = nullptr;
                                Reference   reference = nullptr;
                                Mouse       mouse = nullptr;
                                uint16_t    reserve = 0;
                                bool        focusable = true;
                                bool        focusOnPress = true;
                                Activated   activated = nullptr;
                                ClosedAt    closedAt = nullptr;
                                FullScreen  fullScreen = nullptr;
                                Drop             drop = nullptr;
                                CloseRequest     closeRequested = nullptr;
                                uint16_t         minWidth = 0; // the client area's, a resize by hand's floor
                                uint16_t         minHeight = 0;
                };

                struct Client {
                                Gone          gone = nullptr;
                                void         *arg = nullptr;
                                bool          used = false;
                                Watch         watch = nullptr;
                                void         *watchArg = nullptr;
                                ServiceNotify servicesWatch = nullptr;
                                void         *servicesWatchArg = nullptr;
                };

                // A registerService()'s (PSRAM, one allocation each).
                struct Service {
                                char          name[MaxServiceName + 1] = {};
                                uint8_t       client = 0;
                                ServiceNotify fn = nullptr;
                                void         *arg = nullptr;
                                ServiceQueue  queue;
                };

                struct KeyGrab {
                                Grab    fn = nullptr;
                                void   *arg = nullptr;
                                uint8_t client = 0; // 0: free
                                uint8_t usage = 0;
                                uint8_t modifiers = 0; // left and right folded together
                };

                // Where a dragged window's outline went to snap.
                enum class Snap : uint8_t {
                        None,
                        Maximize,
                        Left,
                        Right
                };

                // The window menu's items (separators between).
                enum class MenuChoice : uint8_t {
                        Restore,
                        Minimize,
                        Maximize,
                        FullScreen, // as Alt+Enter (a window with WindowConfig::fullScreen)
                        Shade,
                        Above,
                        Close,
                        None,
                        Desktop, // the desktop menu's item _menuCommand[i]
                        Program, // a program's menu's (showMenu())
                        Window   // the window's own item (setWindowItem())
                };
                static constexpr size_t MenuItems = MaxDesktopItems; // the window menu has 10 at most
                struct Switcher;                                     // Alt+Tab's list, while it shows

                static constexpr size_t MaxPieces = 256;
                static constexpr size_t UpdateWords = 8; // a bit for each window number

                bool hasUpdates() const;

                static void entry(void *arg);
                static void keyEntry(void *arg, const KeyEvent &event);
                static void pointerEntry(void *arg, const PointerEvent &event);
                static void padEntry(void *arg, const PadEvent &event);
                static void drawCursorPixels(void *arg, uint8_t *pixels, size_t stride);
                static void restorePixels(void *arg, uint8_t *pixels, size_t stride);

        public:
                /** @brief Bytes saved from under the pointer at most: its largest picture's area. */
                static constexpr size_t UnderBytes = (24 + 2 * 4) * 27 * 3; // B, G, R: the starting pointer's

        private:
                void            deliverKeys();
                void            deliverPads();
                bool            freedLockAt(uint16_t x, uint16_t y) const; // over the lock's client, let go
                void            lockTitleChanged();                        // its title's hint redrawn
                bool            shortcut(const KeyEvent &event);      // a key the window manager keeps
                bool            fullScreenKey(const KeyEvent &event); // Alt+Enter, and escape keys while full screen
                void            capturedPointer(uint8_t id, const PointerEvent &event); // every event to it
                uint8_t         lockedId() const; // the pointer's lock while its window is focused; 0: none
                void            switchOpen(int step);
                void            switchTo(int item);
                void            switchEnd(bool go);
                static void     switchRedraw(void *arg, Painter &painter);
                static void     switchMouse(void *arg, const MouseEvent &event);
                static void     switchClosed(void *arg);
                uint8_t         focusId() const;  // the top focusable window; 0: none
                uint8_t         topPopup() const; // 0: none
                void            closePopups();
                void mouseTo(uint8_t id, MouseEvent::Type type, uint16_t x, uint16_t y, uint8_t button, int16_t dx = 0,
                             int16_t dy = 0);
                void            hoverAt(uint16_t x, uint16_t y); // motion without a press held: to the client under it
                void            workAreaChanged();
                Rect            workAreaLocked() const;
                Result<uint8_t> openLocked(const WindowConfig &config);
                Error           showWindowMenuLocked(uint8_t id, const Rect &anchor);
                Error           openMenuLocked(const char *title, const Rect &anchor, uint8_t menuFor);
                void            menuChoose(int item);
                void            menuHighlight(int item);
                static void     menuRedraw(void *arg, Painter &painter);
                static void     menuMouse(void *arg, const MouseEvent &event);
                static void     menuKey(void *arg, const KeyEvent &event);
                void            setMenuItem(size_t i, StringView label, StringView shortcut, uint8_t flags,
                                            const Icon *icon = nullptr);
                void            menuMnemonic(uint32_t key);
                static void     menuClosed(void *arg);
                void            applyPointer();
                bool            dndShowing() const { return _dnd.drag != 0 && !_dnd.dropped; }
                uint8_t         dropTargetAt(uint16_t x, uint16_t y) const; // 0: none
                void            dndTell(uint8_t id, DropEvent::Type type, uint16_t x, uint16_t y);
                void            dndMotion(uint16_t x, uint16_t y);  // the window under it told
                void            dndRelease(uint16_t x, uint16_t y); // dropped there, or ended
                bool            dndKey(const KeyEvent &event);      // Escape, modifiers; true: taken
                void            dndEnd(DropAction done);            // the source told after the round
                void            dndClosed(uint8_t id);              // a window gone
                Placement       placementLocked(uint8_t id);        // its normal rectangle, maximized or not
                void            requestCloseLocked(uint8_t id);     // the user's close: asked, or done
                Pointer         dndShape() const;
                void            dndRetire(); // the image and its under out of use
                void            dndFree();   // and freed, once nothing drawn holds them
                void            press(uint16_t x, uint16_t y, uint8_t button);
                void            release(uint16_t x, uint16_t y);
                void            pressedMoved(uint16_t x, uint16_t y); // a button's press shown while over it
                void            sizeTo(uint16_t x, uint16_t y);       // a resize under way: the outline there
                Pointer         shapeAt(uint16_t x, uint16_t y) const; // the arrow: the starting one, if so
                Pointer         frameShapeAt(uint16_t x, uint16_t y) const;
                bool            startingTick();                 // the sand turned, the timed out dropped
                void            startingChecks();               // the programs gone dropped (unlocked)
                void            startingEnded(int32_t session); // a window of @p session opened
                void            startingChanged();              // the pointer's shape again
                bool            isCursorDrawn() const {
                        return _cursorShown && !_pointerHidden && !_typingHidden && _shape != Pointer::None &&
                               lockedId() == 0;
                }
                void            act(uint8_t id, WindowFrame::Part part);
                WindowFrame     frame(uint8_t id, const Rect &outer) const;
                void            drawCursor();
                void            restoreUnder();     // what the pointer covered, put back
                Rect            cursorArea() const; // where the pointer is drawn: on the screen, 4-pixel columns
                uint16_t        cursorStripRows(uint16_t width) const;  // rows of it one patch takes
                uint8_t         windowAt(uint16_t x, uint16_t y) const; // 0: the desktop
                Error           closeLocked(uint8_t id);
                Error           showDesktopMenuLocked(uint16_t x, uint16_t y);
                bool            windowKey(const KeyEvent &event); // Win+arrows, Alt+F10, Ctrl+Shift+Win
                void            watchersChanged();                // minimizing allowed or not since
                Error           raiseLocked(uint8_t id, bool focus = true);
                Error           minimizeLocked(uint8_t id);
                Error           maximizeLocked(uint8_t id);
                Error           restoreLocked(uint8_t id);
                Error           resizeLocked(uint8_t id, const Rect &rect, uint8_t decorations,
                                             bool tell = true); // told if it changed (and asked)
                Error           shadeLocked(uint8_t id, bool shaded);
                Error           snapLocked(uint8_t id, Snap snap);
                Snap            snapAt(uint8_t id, uint16_t x, uint16_t y) const;
                Rect            snapRect(Snap snap) const;
                void            hideOutline();
                bool            grabbed(const KeyEvent &event); // a key a program claimed
                Service        *findService(StringView name);
                void            dropServices(uint8_t client); // 0: all
                void            servicesChanged();            // the watches told (locked)
                Error           dragLocked(uint8_t id, uint16_t x, uint16_t y);
                Error           outlineLocked(uint8_t id, const Rect &rect);
                Error           dropLocked();
                void            run();
                void            round(uint32_t requests);
                struct Snapshot;                  // below
                void            snapshotLocked(); // every window's, for queries during a round
                bool            readSnapshot(uint8_t id, Snapshot &out) const; // without the lock (false: none kept)
                Error           setPointerLocked(uint8_t id, Pointer shape);
                void            applyPointersAsked(); // setPointer()'s made during a round
                void            drawPieces(Span<const ComposePiece> pieces);
                void            drawLayer(size_t z, Span<const Rect> pieces);
                void            drawUpdates(); // windows asking for an update and not drawn yet this round
                void            drawBottomUp(const Rect &rect);
                void            drawOutline();
                void            drawBand();
                void            restoreBand(); // what the band covered put back
                static void     bandPixels(void *arg, uint8_t *pixels, size_t stride);
                static void     unbandPixels(void *arg, uint8_t *pixels, size_t stride);
                void            changed(uint8_t oldFocus); // wakes the task; repaints titles if the focus moved
                Rect            placed(const Rect &rect) const;
                void            count(const Painter &painter);
                void            drawError(Error err, const char *what); // counted, and logged (the first few)

                Mutex         _lock;
                Display      *_display = nullptr;
                Surface      *_surface = nullptr;
                WindowStack   _stack{0, 0};
                List<Window>  _windows; // by number - 1, grown as numbers are used
                WindowPalette _palette;
                uint8_t      *_under = nullptr; // what the pointer covers (UnderBytes, Psram)
                uint8_t      *_underIn = nullptr; // the buffer _underRect's pixels are in (_under, or a drag's)
                // A drag and drop (devplan/clipboard.md §5c): one at a time.
                struct Dnd {
                                uint32_t   drag = 0; // 0: none
                                uint8_t    source = 0;
                                uint8_t    sourceClient = 0;
                                uint32_t   offer = 0;
                                uint8_t    actions = 0;
                                DropAction preferred = DropAction::Copy;
                                uint8_t    button = 0; // the one held
                                uint8_t    target = 0; // told Enter; 0: none
                                DropAction answer = DropAction::None;
                                bool       dropped = false; // waiting for finishDrop()
                                int64_t    deadlineUs = 0;
                                DragEnded  ended = nullptr;
                                void      *arg = nullptr;
                                uint32_t  *image = nullptr; // PSRAM
                                uint8_t    width = 0;
                                uint8_t    height = 0;
                                uint8_t    hotX = 0;
                                uint8_t    hotY = 0;
                                uint8_t   *under = nullptr; // PSRAM: the image and pointer's area
                };
                Dnd           _dnd;
                uint32_t      _dndSerial = 0;
                uint32_t     *_dndOldImage = nullptr; // an ended drag's, freed once not drawn
                uint8_t      *_dndOldUnder = nullptr;
                DragEnded     _dndEnded = nullptr; // told after the round, unlocked
                void         *_dndEndedArg = nullptr;
                uint32_t      _dndEndedDrag = 0;
                DropAction    _dndEndedDone = DropAction::None;
                Rect          _underRect;       // where; empty: nothing saved
                uint8_t       _dragId = 0;
                Rect          _outline;
                bool          _outlineShown = false;
                uint8_t       _bandId = 0;        // the window a rubber band is over; 0: none
                Rect          _band;              // where (screen coordinates)
                bool          _bandMoved = false; // to draw this round
                struct BandPiece {
                                Rect     rect;       // a piece of its rings, widened to 4-pixel columns
                                uint32_t offset = 0; // what it covered, in _bandUnder
                };
                List<BandPiece>   _bandPieces{MemSpace::Psram}; // drawn, to put back
                List<uint8_t>     _bandUnder{MemSpace::Psram};  // what they covered
                Painter::Tools    _tools;
                GlyphRenderer     _renderer{GlyphRenderer::best()};
                uint32_t          _updates[UpdateWords] = {}; // windows asking for an update, a bit by number
                ComposePiece     *_pieces = nullptr;          // a round's plan
                Rect             *_scratch = nullptr;         // a layer's pieces, client damage, and visible client
                SpinLock          _inputLock;                 // the two queues
                KeyEvent          _keys[KeyQueue];
                uint8_t           _keyTargets[KeyQueue] = {}; // the window a key is for; 0: the focus
                List<Client>      _clients;                   // by connection - 1, grown as they are used
                uint8_t           _keyHead = 0;               // the next to deliver
                uint8_t           _keyCount = 0;
                PointerEvent      _pointers[PointerQueue];
                PadEvent          _pads[PadQueue];
                uint8_t           _padHead = 0;
                uint8_t           _padCount = 0;
                uint8_t           _pointerHead = 0;
                uint8_t           _pointerCount = 0;
                uint16_t          _pointerX = 0; // the pointer's tip
                uint16_t          _pointerY = 0;
                uint8_t           _buttons = 0;         // down, as last applied
                bool              _cursorShown = false; // a mouse has moved it
                bool              _cursorMoved = false; // to draw this round
                uint8_t           _grabId = 0;          // its title bar pressed: moved while held
                uint16_t          _grabX = 0;           // the press, from the window's corner
                uint16_t          _grabY = 0;
                Pointer           _shape = Pointer::Arrow;
                struct Starting {
                                int32_t          pid = 0; // and session; 0: none
                                int64_t          untilUs = 0;
                                ProgramLauncher *launcher = nullptr;
                };
                Starting          _starting[MaxStarting];
                uint8_t           _startingCount = 0;
                uint8_t           _startingFrame = 0;
                int64_t           _startingFrameUs = 0;   // when the sand turns next
                bool              _pointerHidden = false; // by setPointerVisible()
                bool              _typingHidden = false;  // a key went to a window asking: until a move
                uint8_t           _sizeId = 0;            // its resize handle pressed: resized while held
                uint8_t           _sizeEdges = 0;         // the edges it holds
                Rect              _sizeStart;             // the window at the press
                uint16_t          _sizeX = 0;             // the press
                uint16_t          _sizeY = 0;
                uint8_t           _pressId = 0; // its button pressed
                WindowFrame::Part _pressPart = WindowFrame::Part::Outside;
                bool              _pressShown = false; // drawn pressed: the pointer on it
                uint8_t           _clickId = 0;        // the last title bar press, for a double click
                uint64_t          _clickUs = 0;
                uint8_t           _mouseId = 0;      // its client area pressed: has the mouse until all buttons are up
                uint8_t           _fullScreenId = 0; // shown full screen by its owner (setFullScreen())
                uint8_t           _lockId = 0;       // the pointer locked to it (setPointerLock())
                bool              _lockFreed = false; // Ctrl+Alt let the lock go: a click in it takes it again
                uint8_t           _hoverId = 0;      // the client area the pointer is over (told the mouse)
                uint8_t           _swallowed = 0;    // buttons whose press closed popups: their release goes nowhere
                bool              _listChanged = false; // for the watchers, at the round's end
                Rect              _workArea;
                uint8_t           _menuId = 0;  // the window menu's popup; 0: closed
                uint8_t           _menuFor = 0; // the window it is for (0: the desktop's)
                size_t            _menuCount = 0;
                char              _desktopLabels[MaxDesktopItems][MaxDesktopLabel + 1] = {};
                size_t            _desktopCount = 0;
                DesktopChosen     _desktopChosen = nullptr;
                void             *_desktopArg = nullptr;
                int               _desktopPicked = -1; // chosen: told after the round, unlocked
                char              _menuLabels[MenuItems][MaxMenuLabel + 1] = {}; // marks taken out
                uint32_t          _menuKeys[MenuItems] = {};                     // their mnemonics
                char              _programShortcuts[MaxMenuItems][MaxMenuShortcut + 1] = {};
                MenuChosen        _programChosen = nullptr; // the open program menu's
                void             *_programArg = nullptr;
                int               _programPicked = -1; // chosen: told after the round, unlocked
                MenuChosen        _pickedFn = nullptr; // whom to tell it
                WindowItemChosen  _itemPicked = nullptr; // a window's own item chosen: told after the round
                void             *_itemArg = nullptr;
                void             *_pickedArg = nullptr;
                bool              _canMinimize = false; // a client watches the window list
                int               _menuHighlight = -1;
                MenuLayout        _menuLayout;
                MenuLayout::Item  _menuItems[MenuItems];
                // The window menu's icons, the ROM's bmds theme's (devplan/rom.md §3i): Restore,
                // Minimize, Maximize, Roll Up, Roll Down, Close; found at start(), invalid without it.
                Icon                  _windowIcons[7];
                MenuChoice            _menuChoices[MenuItems] = {};
                Switcher             *_switcher = nullptr; // in PSRAM while Alt+Tab's list shows
                uint8_t               _switchId = 0;       // its popup
                uint8_t               _modifiers = 0;      // the keyboard's, as of the last key
                uint8_t               _focusId = 0;        // the focusable window raised last (focusId())
                KeyGrab               _grabs[MaxGrabs];
                List<Service *>       _services{MemSpace::Psram}; // registerService()'s
                uint8_t               _tapUsage = 0;            // a claimed modifier down (a tap if nothing else comes)
                bool                  _tapClean = false;        // nothing else pressed while it was down
                Snap                  _snap = Snap::None;       // the drag's outline is a snap's
                uint8_t               _unsnapId = 0;            // a snapped window grabbed: its size back when it moves
                std::atomic<uint32_t> _requests{0};             // changes made
                std::atomic<uint32_t> _asked[UpdateWords] = {}; // update() calls, taken by the next round
                // Read without the lock while a round holds it (windows.md §4o): each window's client
                // area and whether it is minimized, a seqlock's, refreshed at each round's end and when
                // a query gets the lock; and pointer shapes asked meanwhile, applied by the next round.
                struct Snapshot {
                                Rect    client;
                                uint8_t valid = 0;
                                uint8_t minimized = 0;
                };
                static constexpr size_t MaxIds = UpdateWords * 32;
                Snapshot               *_snapshots = nullptr;     // by number (PSRAM, kept once made)
                uint8_t                *_pointerWanted = nullptr; // by number, with _pointerAsked's bit
                std::atomic<uint32_t>   _snapshotSeq{0};          // odd while written
                std::atomic<uint32_t>   _pointerAsked[UpdateWords] = {};
                std::atomic<uint32_t> _drawn{0};                // changes drawn
                uint32_t              _presented = 0;           // the last present's number
                Stats                 _stats;
                ::bmds::Stats::Group  _counters; // wmN (devplan/sysmon.md §6)

                static void   readCounters(void *arg, Span<int64_t> values);
                Semaphore     _wake{1, 0};
                WaitListOf<4> _idle; // woken after each round
                Task          _task;
};

BMDS_NAMESPACE_END
