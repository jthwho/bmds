/**
 * @file      programlauncher.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/fdtable.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class Console;
class Interrupt;
struct ProgramEntry;

/**
 * @brief Runs program files for the shell (bmds_loader implements it; the
 *        interface lives here so neither depends on the other).
 *
 * The shell hands over a command whose name is a path: the file, its
 * arguments (`args[0]` as typed), the exported environment as "NAME=value"
 * strings, the command's file descriptors and current directory, its
 * terminal, its process group. The call returns when the program has
 * ended; the terminal's modes are as they were.
 *
 * @par Thread Safety
 * Called from the task running the command (a shell or a pipeline worker);
 * implementations must allow several at once.
 */
class ProgramLauncher {
        public:
                /**
                 * @brief A job's process group, shared by the programs of one
                 *        pipeline (a shell's job control, devplan/processes.md §3.6).
                 *
                 * The first program started with it makes the group; the others
                 * join it. Lives until the last of them has been started.
                 */
                struct JobGroup {
                                int32_t id = 0;            ///< The group (0: the next program makes it).
                                bool    foreground = true; ///< The terminal's foreground while it runs.
                };

                /** @brief What a launch needs. */
                struct Request {
                                StringView             path;                ///< The file, as typed.
                                Span<const StringView> args;                ///< argv, args[0] included.
                                Span<const StringView> environment;         ///< "NAME=value" each.
                                const FdTable         *fds = nullptr;       ///< The command's descriptors.
                                StringView             cwd;                 ///< Its current directory.
                                Interrupt             *interrupt = nullptr; ///< The caller's (stops the wait).
                                Console               *console = nullptr;   ///< The terminal (`/dev/console`).
                                /**
                                 * @brief Its group: null, the caller's; else the job's (see
                                 *        `JobGroup`). Ignored with `session`.
                                 */
                                JobGroup *group = nullptr;
                                /**
                                 * @brief A new session with `console` as its controlling
                                 *        terminal, the program leading it and in the foreground
                                 *        (a shell the system starts on a terminal).
                                 */
                                bool session = false;
                                /**
                                 * @brief `start()`: the caller's child, kept once it ends until
                                 *        the caller takes it (`ProcessControl::waitChild()`, a
                                 *        shell's job); otherwise nobody waits for it.
                                 */
                                bool waited = false;
                                /**
                                 * @brief Kernel code to run as the process instead of the file
                                 *        at `path`, which names it (a shell's background
                                 *        subshell, devplan/processes.md §3.8.1); null: the file.
                                 */
                                const ProgramEntry *builtin = nullptr;
                                /** @brief `builtin`'s `Context::data`: its own once it starts. */
                                void *data = nullptr;
                };

                /**
                 * @brief Runs the program in @p request and waits for it to end.
                 *
                 * Problems are reported on the request's stderr, as `path: reason`.
                 *
                 * @return The exit status (0-255), or the error: `NotExist` (no such
                 *         file), `IsDir`, `NotSupported` (not a program it can run),
                 *         `NoMem`, ... (the shell maps them to 127 and 126).
                 * @blocking
                 */
                virtual Result<int> run(const Request &request) = 0;

                /**
                 * @brief Starts the program in @p request without waiting for it:
                 *        nobody waits, and it is freed when it ends (a GUI program
                 *        started from a command, `devplan/pty.md`); or, with `waited`,
                 *        the caller takes its end later (a shell's job).
                 *
                 * Without a group, it gets one of its own, not in the foreground.
                 *
                 * @return Its process id, or the errors of `run()`; `NotSupported`
                 *         by a launcher that can't, `Invalid` for `waited` from
                 *         outside a process. When it fails, `data` stays the caller's.
                 */
                virtual Result<int32_t> start(const Request &request) {
                        (void)request;
                        return makeError<int32_t>(Error::NotSupported);
                }

        protected:
                ~ProgramLauncher() = default;
};

/**
 * @brief What a built-in program can ask of its own process (the loader
 *        implements it; devplan/processes.md §3.6.1).
 *
 * @par Thread Safety
 * Any of the process's threads.
 */
class ProcessControl {
        public:
                /** @brief What a signal does to a built-in program. */
                enum class SignalAction : uint8_t {
                        Default,   ///< As for any process: dropped, or it ends the process.
                        Ignore,    ///< Dropped.
                        Interrupt, ///< Raises the process's interrupt (kernel code's catch).
                };

                /** @brief Returns the process's id. */
                virtual int32_t processId() const = 0;

                /**
                 * @brief Sets what @p sig (1-31) does.
                 * @return `Ok`, or `Invalid` (`SIGKILL`, `SIGSTOP`, not a signal).
                 */
                virtual Error setSignalAction(int sig, SignalAction action) = 0;

                /** @brief Returns true once the process is being ended (a signal, a kill): stop. */
                virtual bool isEnding() const = 0;

                /**
                 * @brief Takes the end of child @p pid, one started with `waited`.
                 * @param pid    The child.
                 * @param block  Wait until it ends (the process's interrupt stops the wait).
                 * @param status [out] How it ended, when taken: a wait status
                 *               (Linux's layout, as `proc_wait` gives it).
                 * @return Its pid once taken; 0 if it hasn't ended (not blocking);
                 *         `NotExist` (not a child, or already taken), `Interrupted`.
                 * @blocking With @p block.
                 */
                virtual Result<int32_t> waitChild(int32_t pid, bool block, int32_t &status) = 0;

                /** @brief Leaves child @p pid to end on its own: freed when it ends, not kept. */
                virtual void disownChild(int32_t pid) = 0;

        protected:
                ~ProcessControl() = default;
};

/**
 * @brief A program built into the firmware, run as a process: the shell as
 *        `/bin/sh` (devplan/programs.md §13, step 8b).
 *
 * The loader runs `main` in a task of its own, as it runs a program file,
 * and the process ends with what it returns. It sees what a program gets
 * at its start; it calls no system calls, so it must notice signals itself
 * (the interrupt), as the shell does.
 */
struct BuiltinProgram {
                /** @brief What the program starts with (valid until `main` returns). */
                struct Context {
                                const FdTable         *fds = nullptr;       ///< Its descriptors.
                                StringView             cwd;                 ///< Its directory.
                                Span<const StringView> args;                ///< argv, args[0] included.
                                Span<const StringView> environment;         ///< "NAME=value" each.
                                Console               *console = nullptr;   ///< The terminal (may be null).
                                Interrupt             *interrupt = nullptr; ///< Its signals (may be null).
                                ProgramLauncher       *launcher = nullptr;  ///< Runs the programs it starts.
                                StringView             program; ///< Its name in `/bin` (an applet: its command).
                                ProcessControl        *process = nullptr; ///< Its process (may be null).
                                void                  *data = nullptr; ///< The request's `data` (kernel code it named).
                };

                /** @brief The program: returns its exit status (0-255). */
                using Main = int (*)(const Context &context);

                Main   main = nullptr; ///< Null: none.
                size_t stackBytes = 0; ///< Its task's stack.
};

BMDS_NAMESPACE_END
