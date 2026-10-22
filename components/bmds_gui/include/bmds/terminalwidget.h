/**
 * @file      terminalwidget.h
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
#include <bmds/mutex.h>
#include <bmds/programlauncher.h>
#include <bmds/string.h>
#include <bmds/terminalscreen.h>
#include <bmds/terminalselection.h>
#include <bmds/terminalview.h>
#include <bmds/thread.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class Pty;
struct TextCell;

/**
 * @brief A terminal in a widget (`devplan/editor.md` §7): a program run on a pseudo-terminal,
 *        its output on a screen drawn as the terminal application draws it, the keys typed to
 *        it, a scroll-back, selection and the clipboard.
 *
 * Each `start()` runs its command on a new pty, its output following what the screen shows
 * already (`clear()` empties it), so a run's output stays above the next's, as in a console.
 * The command's end is told with its status (`setEnded()`); `stop()` ends it (SIGTERM, then
 * SIGKILL). Extra descriptors can be given to the command (`Command::extraFds`: a pipe for its
 * reports, as the editor's runs of a script).
 *
 * **Keys:** what a terminal sends (`TerminalKeys`); Shift+Page Up and Down scroll back;
 * Ctrl+Shift+C copies the selection, Ctrl+Shift+V and Shift+Insert paste. **The mouse:** the
 * left button selects (two clicks words, three lines), the middle one pastes the primary
 * selection, the wheel scrolls back.
 *
 * Its colours are the terminal application's (`terminal/colors/...`).
 *
 * @par Thread Safety
 * As `Widget`; its reader and runner threads post to the GUI loop.
 */
class TerminalWidget : public Widget {
        public:
                /** @brief Told on the GUI loop's thread that the command ended, with its exit status. */
                using Ended = void (*)(void *arg, TerminalWidget &terminal, int status);

                /** @brief A descriptor the command gets beyond 0, 1 and 2. */
                struct ExtraFd {
                                int             fd = 3;
                                FileHandle::Ptr handle;
                };

                /** @brief What to run. */
                struct Command {
                                StringView             path;        ///< The program's file.
                                Span<const StringView> args;        ///< argv, args[0] included.
                                Span<const StringView> environment; ///< "NAME=value" each.
                                StringView             cwd;
                                ProgramLauncher       *launcher = nullptr;
                                ProcessControl        *process = nullptr; ///< The caller's, to wait for the command.
                                Span<ExtraFd>          extraFds;          ///< Moved into the command's table.
                };

                static constexpr uint16_t HistoryLines = 1000;
                static constexpr uint16_t MaxColumns = 240;

                explicit TerminalWidget(Widget *parent);
                ~TerminalWidget() override;

                /** @brief Runs @p command on a new pty. @return `Ok`, `Busy` (one runs), or why it can't start. */
                Error start(const Command &command);
                /** @brief Returns true while a command runs. */
                bool isRunning() const { return _running.load(std::memory_order_acquire); }
                /** @brief Ends the command: SIGTERM, or SIGKILL if @p kill. */
                Error stop(bool kill);
                /** @brief Empties the screen and its history. */
                void clear();
                /** @brief Shows @p text as if the program had written it (a note: "[ended with 1]"). */
                void show(StringView text);
                /** @brief Sets who is told of the end. */
                void setEnded(Ended fn, void *arg) {
                        _ended = fn;
                        _endedArg = arg;
                        return;
                }

                GuiSize                sizeHint() const override;
                void                   paint(Painter &painter) override;
                bool                   keyPressed(const KeyEvent &event) override;
                void                   mousePressed(const WindowManager::MouseEvent &event) override;
                void                   mouseMoved(const WindowManager::MouseEvent &event) override;
                void                   mouseReleased(const WindowManager::MouseEvent &event) override;
                bool                   wheelTurned(int notches) override;
                WindowManager::Pointer pointerAt(int x, int y) const override;
                void                   focusIn() override;
                void                   focusOut() override;

        protected:
                void resized() override;

        private:
                static void                readerMain(void *arg);
                static void                runnerMain(void *arg);
                static void                outputDue(void *arg);
                static void                endDue(void *arg);
                static void                reply(void *context, StringView text);
                static void                pasted(void *arg, Error err, StringView type, StringView data);
                static const TerminalCell *selectionLine(void *arg, int64_t line, uint16_t &length);
                static Error               collect(void *arg, StringView piece);

                void                     readColors();
                void                     fit(); // the screen and the pty to the widget's size
                void                     scrollBy(int lines);
                void                     joinThreads();
                void                     copySelection(Clipboard::Selection to);
                TerminalSelection::Point pointAt(int x, int y, bool cell); // the lock held

                Mutex             _lock; // the screen, the selection
                TerminalScreen    _screen{MemSpace::Psram};
                TerminalSelection _selection;
                TerminalColors    _colors;
                TextCell         *_cells = nullptr; // a row's, made once
                Pty              *_pty = nullptr;
                Thread            _reader;
                Thread            _runner;
                bool              _threads = false; // started, to be joined
                std::atomic<bool> _running{false};
                std::atomic<bool> _outputPosted{false};
                std::atomic<int>  _status{0};
                int32_t           _pid = -1;
                ProgramLauncher  *_launcher = nullptr;
                ProcessControl   *_process = nullptr;
                // The command, kept for the runner (its strings copied).
                String                 _path{MemSpace::Psram};
                String                 _cwd{MemSpace::Psram};
                String                 _argText{MemSpace::Psram}; // the arguments, NUL-separated
                List<StringView>       _args{MemSpace::Psram};
                Span<const StringView> _environment;
                ExtraFd                _extra[2];
                size_t                 _extraCount = 0;
                uint32_t               _offset = 0; // lines scrolled back
                uint32_t               _added = 0;  // the history's lines added, as drawn
                bool                   _selecting = false;
                uint8_t                _clicks = 0;
                uint64_t               _pressUs = 0;
                Ended                  _ended = nullptr;
                void                  *_endedArg = nullptr;
                String                *_copy = nullptr; // collect()'s target
};

BMDS_NAMESPACE_END
