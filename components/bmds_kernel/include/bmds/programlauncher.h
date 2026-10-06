/**
 * @file      programlauncher.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/fdtable.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class Console;
class Interrupt;

/**
 * @brief Runs program files for the shell (bmds_loader implements it; the
 *        interface lives here so neither depends on the other).
 *
 * The shell hands over a command whose name is a path: the file, its
 * arguments (`args[0]` as typed), the exported environment as "NAME=value"
 * strings, the command's file descriptors and current directory, its
 * terminal. The call returns when the program has ended; the terminal's
 * modes are as they were.
 *
 * @par Thread Safety
 * Called from the task running the command (a shell or a pipeline worker);
 * implementations must allow several at once.
 */
class ProgramLauncher {
        public:
                /** @brief What a launch needs. */
                struct Request {
                                StringView             path;                ///< The file, as typed.
                                Span<const StringView> args;                ///< argv, args[0] included.
                                Span<const StringView> environment;         ///< "NAME=value" each.
                                const FdTable         *fds = nullptr;       ///< The command's descriptors.
                                StringView             cwd;                 ///< Its current directory.
                                Interrupt             *interrupt = nullptr; ///< Ctrl-C (the console's).
                                Console               *console = nullptr;   ///< The terminal (`/dev/console`).
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
                 *        started from a command, `devplan/pty.md`).
                 * @return Its process id, or the errors of `run()`; `NotSupported`
                 *         by a launcher that can't.
                 */
                virtual Result<int32_t> start(const Request &request) {
                        (void)request;
                        return makeError<int32_t>(Error::NotSupported);
                }

        protected:
                ~ProgramLauncher() = default;
};

/**
 * @brief A program built into the firmware, run as a process: the shell as
 *        `/bin/sh` (devplan/programs.md §13, step 8b).
 *
 * The loader runs `main` in a task of its own, as it runs a program file,
 * and the process ends with what it returns. It sees what a program gets
 * at its start; it calls no system calls, so it must notice Ctrl-C itself
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
                                Interrupt             *interrupt = nullptr; ///< Ctrl-C (may be null).
                                ProgramLauncher       *launcher = nullptr;  ///< Runs the programs it starts.
                                StringView             program; ///< Its name in `/bin` (an applet: its command).
                };

                /** @brief The program: returns its exit status (0-255). */
                using Main = int (*)(const Context &context);

                Main   main = nullptr; ///< Null: none.
                size_t stackBytes = 0; ///< Its task's stack.
};

BMDS_NAMESPACE_END
