/**
 * @file      terminal.h
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
#include <bmds/glyphrenderer.h>
#include <bmds/inputevent.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/screenconsole.h>

BMDS_NAMESPACE_BEGIN

class Painter;
class ProgramLauncher;
class WindowManager;

/**
 * @brief The terminal program: a shell in a window (`devplan/windows.md`
 *        §4e).
 *
 * A terminal is a numbered session: a `ScreenConsole` (its screen in PSRAM)
 * and a shell started on it the first time it opens. A shell runs until
 * reboot, so the session outlives its window: closing the window leaves
 * the shell writing to the screen, and `open()` shows it again. The
 * window's client area sets the size, in 8×16 cells, and a new size
 * (maximized, restored) resizes the screen: the shell sees `SIGWINCH`'s
 * path, the console's resize.
 *
 * Keys typed into its window go to the shell as a terminal sends them
 * (`TerminalKeys`: xterm's sequences, application cursor keys when the
 * program asks).
 *
 * It keeps `HistoryLines` of scrollback (`devplan/windows.md` §4i): the
 * wheel over its window, or Shift+Page Up and Down, scroll back through
 * it; a key typed returns to the bottom. On the alternate screen the wheel
 * sends arrow keys instead.
 *
 * Output asks the window manager for an update; at the next round the
 * window draws the rows that changed, those under any damage, and where
 * the cursor was and is (rows are redrawn, never scrolled: faster here).
 *
 * @par Thread Safety
 * `open()` and the lookups from any task; drawing on the window manager's.
 */
class Terminal {
        public:
                /** @brief Sessions there can be. */
                static constexpr size_t MaxTerminals = 4;

                /** @brief The scrollback kept, in lines. */
                static constexpr uint16_t HistoryLines = 1000;

                /** @brief Lines a notch of the wheel scrolls. */
                static constexpr int WheelLines = 3;

                /** @brief The widest row drawn, in cells (1920 pixels). */
                static constexpr uint16_t MaxColumns = 240;

                /**
                 * @brief Returns terminal @p number (created, without a shell, on
                 *        first use).
                 * @return It, or `NotFound` (past `MaxTerminals`).
                 */
                static Result<Terminal *> terminal(size_t number);

                /** @brief Returns the terminal shown in window @p id, or null. */
                static Terminal *byWindow(uint8_t id);

                /** @brief Sets what runs program files for the terminals' shells (from boot). */
                static void setLauncher(ProgramLauncher *launcher);

                explicit Terminal(uint8_t number);
                Terminal(const Terminal &) = delete;
                Terminal &operator=(const Terminal &) = delete;

                /** @brief Returns the number. */
                uint8_t number() const { return _number; }

                /**
                 * @brief Shows the session in a window at @p rect (outer) on
                 *        @p wm, starting its shell the first time; if shown, raises
                 *        its window.
                 * @return Its window's number, or the window manager's,
                 *         allocation's or shell's error.
                 */
                Result<uint8_t> open(WindowManager &wm, const Rect &rect);

                /** @brief Returns its window's number, or 0 (not shown). */
                uint8_t window() const { return _window.load(std::memory_order_acquire); }

                /** @brief Returns true once its shell runs. */
                bool isStarted() const { return _started; }

                /** @brief Returns how far back the view is scrolled, in lines (0: at the bottom). */
                size_t scrollback() const { return _offset.load(std::memory_order_relaxed); }

                /** @brief Returns the console (typed keys go in with `pushInput()`). */
                ScreenConsole &console() { return _console; }

        private:
                static void redraw(void *arg, Painter &painter);
                static void closed(void *arg);
                static void keyTyped(void *arg, const KeyEvent &event);
                static void resized(void *arg, uint16_t width, uint16_t height);
                static void wheeled(void *arg, int notches);
                void        scrollBy(int lines); // back (positive) or forward, clamped; redrawn
                size_t      send(const KeyEvent &event);
                static void changed(void *arg);
                void        draw(Painter &painter);

                char                 _name[8] = {}; // the console's and shell's name: "termN"
                char                 _title[16] = {};
                ScreenConsole        _console;
                WindowManager       *_wm = nullptr;
                std::atomic<uint8_t> _window{0};
                TextCell            *_cells = nullptr;                                      // a row's, internal RAM
                uint32_t             _cursorRows[(TerminalScreen::MaxSide + 31) / 32] = {}; // rows drawn with it
                std::atomic<uint32_t> _offset{0};   // lines scrolled back (written on the window manager's task)
                uint32_t              _added = 0;   // the history's count of lines added, as last drawn
                bool                  _all = false; // every row to redraw (the view moved)
                uint8_t              _number;
                bool                 _started = false;
};

BMDS_NAMESPACE_END
