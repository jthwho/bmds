/**
 * @file      debugsession.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/filehandle.h>
#include <bmds/guiapplication.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/string.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The editor's side of a debugged run (`devplan/editor.md` §17): commands written to the
 *        program, its events and answers read back.
 *
 * The protocol is the editor's, not a language's: a language that can debug starts its program
 * speaking it (`LanguageSupport::debugCommand()`). A reader thread splits what the program
 * writes into lines and posts them to the GUI loop; there, a `stopped` line is told, answers are
 * matched to the questions asked (in order), and the report's own lines (`error`, `exception`)
 * are kept for the run's end.
 *
 * @par Thread Safety
 * The GUI loop's thread; the reader posts to it.
 */
class DebugSession {
        public:
                /** @brief How the program goes on from a stop. */
                enum class Resume : uint8_t {
                        Go,   ///< Until a breakpoint.
                        Step, ///< Into calls.
                        Next, ///< Over calls.
                        Out,  ///< Out of the function.
                };

                /** @brief A frame of the call stack, the innermost first. */
                struct Frame {
                                uint32_t level = 0;
                                String   function{MemSpace::Psram};
                                String   file{MemSpace::Psram};
                                uint32_t line = 0; ///< From 1.
                };

                /** @brief A variable, a member or an element. */
                struct Var {
                                String   name{MemSpace::Psram};
                                String   type{MemSpace::Psram};
                                String   value{MemSpace::Psram};
                                uint32_t children = 0; ///< Members or elements to expand.
                };

                /** @brief The level asked for the globals (`askVars()`). */
                static constexpr int Globals = -1;

                /** @brief Told on the loop's thread. */
                class Listener {
                        public:
                                /** @brief The program stopped at @p file's @p line (from 1); @p message for an exception. */
                                virtual void debugStopped(StringView reason, StringView file, uint32_t line,
                                                          StringView message) = 0;
                                /** @brief The call stack asked for. */
                                virtual void debugStack(Span<const Frame> frames) = 0;
                                /** @brief Variables asked for as @p token; @p error if they couldn't be found. */
                                virtual void debugVars(uint32_t token, Span<const Var> vars, StringView error) = 0;

                        protected:
                                ~Listener() = default;
                };

                DebugSession(GuiApplication &app, Listener &listener);
                ~DebugSession();
                DebugSession(const DebugSession &) = delete;
                DebugSession &operator=(const DebugSession &) = delete;

                /**
                 * @brief Starts reading @p events (the program's report descriptor's other end) and
                 *        writing commands to @p commands.
                 * @return `Ok`, `Busy` (a session runs), or the thread's error.
                 */
                Error start(FileHandle::Ptr events, FileHandle::Ptr commands);

                /**
                 * @brief The program ended: its last lines taken, the reader joined, the session over.
                 * @return The report's lines (an error's or an exception's), as a run's report.
                 */
                String finish();

                /** @brief Returns true while a session runs. */
                bool isActive() const { return _active; }
                /** @brief Returns true while the program is stopped (questions may be asked). */
                bool isStopped() const { return _stopped; }

                /** @brief Sets or clears a breakpoint at @p file's @p line (from 1). */
                Error setBreakpoint(StringView file, uint32_t line, bool on);
                /** @brief Resumes the program (when stopped; before the first line too). */
                Error resume(Resume how);
                /** @brief Stops the program at its next line. */
                Error pause();
                /** @brief Asks for the call stack (`Listener::debugStack()`). */
                Error askStack();
                /**
                 * @brief Asks for frame @p level's variables (`Globals`: the globals), or @p path's
                 *        members if not empty; told with @p token.
                 */
                Error askVars(int level, StringView path, uint32_t token);
                /** @brief Asks for @p path's own value (a watch): one `Var`, told with @p token. */
                Error askValue(int level, StringView path, uint32_t token);

        private:
                enum class Asked : uint8_t {
                        Stack,
                        Vars,
                };

                static constexpr size_t MaxAsked = 32;

                static void readerMain(void *arg);
                static void linesDue(void *arg);

                Error send(StringView line);
                Error ask(Asked asked, uint32_t token, StringView line);
                void  take(StringView line); // on the loop's thread
                void  answered();            // the answer's lines, up to `end`
                void  takeLines();           // the reader's lines so far

                GuiApplication   &_app;
                Listener         &_listener;
                FileHandle::Ptr   _events;
                FileHandle::Ptr   _commands;
                Thread            _reader;
                bool              _readerStarted = false;
                bool              _active = false;
                bool              _stopped = false;
                Mutex             _lock; // _lines
                List<String>      _lines{MemSpace::Psram};
                std::atomic<bool> _posted{false};
                Asked             _asked[MaxAsked] = {};
                uint32_t          _tokens[MaxAsked] = {};
                size_t            _askedCount = 0;
                List<String>      _answer{MemSpace::Psram}; // the answer's lines so far
                String            _report{MemSpace::Psram};
};

BMDS_NAMESPACE_END
