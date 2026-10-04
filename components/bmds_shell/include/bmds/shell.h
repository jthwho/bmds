/**
 * @file      shell.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The BMDS command shell. Commands are declared in command.h.
 */

#pragma once

#include <cstddef>
#include <bmds/command.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fdtable.h>
#include <bmds/interrupt.h>
#include <bmds/lineeditor.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/workerpool.h>

BMDS_NAMESPACE_BEGIN

class Console;
struct Pipeline;

/**
 * @brief A command shell running on a console.
 *
 * The owner feeds input bytes (or calls `run()`, which reads the console
 * forever). Each completed line is parsed into a pipeline (see
 * `parsePipeline()`) and run:
 *
 * - Every stage gets a copy of the shell's file descriptors (0, 1 and 2 on
 *   the console), then its pipes, then its redirections, left to right.
 * - The stages run concurrently. The last runs on the shell's own task;
 *   the others on worker threads from a pool (PSRAM stacks). The exit code
 *   is the last stage's.
 * - Ctrl-C (the console's interrupt) stops blocked pipes and is seen by
 *   commands through `CommandContext::isInterrupted()`. An interrupted
 *   line exits with `ExitInterrupted` and the shell echoes `^C`.
 *
 * @par Thread Safety
 * Thread-affine: one task runs the shell. Pipeline stages on worker
 * threads use only the command table and the console.
 */
class Shell {
        public:
                /** @brief Most stages in a pipeline (one interruptible waiter each). */
                static constexpr size_t MaxStages = Interrupt::MaxWaiters;

                /** @brief Stack of each pipeline worker thread (PSRAM). */
#if defined(CONFIG_BMDS_SHELL_WORKER_STACK_BYTES)
                static constexpr size_t WorkerStackBytes = CONFIG_BMDS_SHELL_WORKER_STACK_BYTES;
#else
                static constexpr size_t WorkerStackBytes = 8192;
#endif

                /** @brief Priority of pipeline worker threads (the shell's own). */
#if defined(CONFIG_BMDS_SHELL_PRIORITY)
                static constexpr unsigned int WorkerPriority = CONFIG_BMDS_SHELL_PRIORITY;
#else
                static constexpr unsigned int WorkerPriority = 2;
#endif

                static constexpr int ExitSuccess = 0;       ///< Success.
                static constexpr int ExitFailure = 1;       ///< The command failed.
                static constexpr int ExitUsage = 2;         ///< Bad arguments, or a line that doesn't parse.
                static constexpr int ExitNotFound = 127;    ///< No such command.
                static constexpr int ExitInterrupted = 130; ///< Stopped by Ctrl-C (128 + SIGINT, as in bash).

                /**
                 * @brief Returns the commands registered with `BMDS_SHELL_COMMAND`, sorted by name.
                 *
                 * Target only (the table is built by the linker).
                 */
                static Span<const Command> registeredCommands();

                /**
                 * @brief Starts a shell on @p console in its own task.
                 *
                 * Target only. The shell is allocated from `Psram` and runs
                 * until reboot.
                 * @param console The console to run on.
                 * @param name    Task name.
                 * @return `Ok`, or `NoMem`.
                 */
                static Error spawn(Console &console, const char *name);

                /** @brief The `help` command (registered by the built-ins). */
                static int runHelp(CommandContext &ctx);

                /**
                 * @brief Constructs a shell; call `init()` before use.
                 * @param console  The console to run on.
                 * @param commands The command table, sorted by name (asserted).
                 */
                Shell(Console &console, Span<const Command> commands);

                Shell(const Shell &) = delete;
                Shell &operator=(const Shell &) = delete;

                /**
                 * @brief Opens stdin, stdout and stderr on the console.
                 * @return `Ok`, or `NoMem`.
                 */
                Error init();

                /** @brief Returns the console. */
                Console &console() { return _console; }

                /** @brief Returns the command table. */
                Span<const Command> commands() const { return _commands; }

                /** @brief Finds a command by name (binary search), or returns null. */
                const Command *findCommand(StringView name) const;

                /** @brief Returns the shell's own file descriptors (what commands start from). */
                FdTable &fds() { return _fds; }

                /** @brief Returns the exit code of the last command line run. */
                int lastExitCode() const { return _lastExitCode; }

                /** @brief Prints the first prompt. */
                void start();

                /** @brief Processes one input byte; runs a command line when one completes. */
                void feed(char c);

                /**
                 * @brief Parses @p line in place and runs it.
                 * @return The exit code. An empty line returns `lastExitCode()`.
                 */
                int execute(char *line);

                /**
                 * @brief Runs the shell: prompt, read, execute, forever.
                 * @blocking Waits for console input indefinitely.
                 */
                [[noreturn]] void run();

        private:
                int  runPipeline(Pipeline &pipeline);
                void report(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

                Console            &_console;
                Span<const Command> _commands;
                FdTable             _fds;
                WorkerPool          _workers;
                int                 _lastExitCode = 0;
                LineEditor          _editor;
};

BMDS_NAMESPACE_END
