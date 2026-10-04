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
#include <bmds/executor.h>
#include <bmds/fdtable.h>
#include <bmds/interrupt.h>
#include <bmds/lineeditor.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/workerpool.h>

BMDS_NAMESPACE_BEGIN

class Console;

/**
 * @brief A command shell running on a console.
 *
 * The owner feeds input bytes (or calls `run()`, which reads the console
 * forever). Completed lines are collected until they form complete
 * commands (`PS2` prompts for the rest), which the `Executor` runs one at a
 * time: the shell language of devplan/shell-language.md, with commands
 * looked up as functions, then in the command table.
 *
 * - Pipelines run concurrently: the last stage on the shell's task, the
 *   others on worker threads (PSRAM stacks), each over a copy of the state.
 * - Ctrl-C (the console's interrupt) stops blocked pipes and running
 *   commands; the shell echoes `^C` and the exit status is 130.
 * - `exit` at the prompt ends the running input and sets `$?`; the console
 *   shell itself keeps running.
 * - The prompt is `PS1` (`PS2` while continuing) with bash's backslash
 *   escapes, then parameter expansion; Tab completes command names (and
 *   functions, keywords) where a command starts, and variable names after `$`.
 *
 * @par Thread Safety
 * Thread-affine: one task runs the shell. Pipeline stages on worker
 * threads use their own state copy, the command table and the console.
 */
class Shell final : public LineCompleter {
        public:
                /** @brief Most stages in a pipeline (one interruptible waiter each). */
                static constexpr size_t MaxStages = Interrupt::MaxWaiters;

                /** @brief Stack of each pipeline worker thread (PSRAM). */
#if defined(CONFIG_BMDS_SHELL_WORKER_STACK_BYTES)
                static constexpr size_t WorkerStackBytes = CONFIG_BMDS_SHELL_WORKER_STACK_BYTES;
#else
                static constexpr size_t WorkerStackBytes = 32768;
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
                 * Target only. The shell and its stack are allocated from
                 * `Psram` and run until reboot.
                 * @param console The console to run on.
                 * @param name    Task name.
                 * @return `Ok`, or `NoMem`.
                 */
                static Error spawn(Console &console, const char *name);

                /** @brief The `help` command (registered by the built-ins). */
                static int runHelp(CommandContext &ctx);

                /**
                 * @brief Constructs a shell; call `init()` before use.
                 * @param console    The console to run on.
                 * @param commands   The command table, sorted by name (asserted).
                 * @param stackBytes Stack of the task that will run it (recursion budget).
                 */
                Shell(Console &console, Span<const Command> commands, size_t stackBytes);

                Shell(const Shell &) = delete;
                Shell &operator=(const Shell &) = delete;

                /**
                 * @brief Opens stdin, stdout and stderr on the console and sets the
                 *        initial variables (`PS1`, `PS2`, `IFS`).
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

                /** @brief Returns the shell state: variables, functions, options. */
                ShellState &state() { return _state; }

                /** @brief Returns the worker threads that run pipeline stages. */
                WorkerPool &workers() { return _workers; }

                /** @brief Returns the exit status of the last command (`$?`). */
                int lastExitCode() const { return _state.lastStatus; }

                /** @brief Returns true while a command is incomplete (the `PS2` prompt). */
                bool isContinuing() const { return !_input.isEmpty(); }

                /** @brief Prints the prompt (`PS1`, or `PS2` while continuing). */
                void start();

                /** @brief Processes one input byte; runs commands when a line completes them. */
                void feed(char c);

                /**
                 * @brief Adds one line of input and runs the commands it completes.
                 * @return `$?` afterwards (unchanged while the input is incomplete).
                 */
                int execute(StringView line);

                /**
                 * @brief Runs @p source as a script (not interactively: `exit` ends it).
                 * @return Its exit status.
                 */
                int runScript(StringView source);

                /**
                 * @brief Runs the shell: prompt, read, execute, forever.
                 * @blocking Waits for console input indefinitely.
                 */
                [[noreturn]] void run();

                /**
                 * @brief Renders a prompt string: backslash escapes (`\h \u \w \W \s \v
                 *        \$ \n \e \a \\ \nnn`, `\[` `\]` dropped), then expansion.
                 * @param ps   The prompt variable's value.
                 * @param out  [out] The prompt to show.
                 */
                void renderPrompt(StringView ps, String &out);

                /** @brief Tab completion for the line editor (see `LineCompleter`). */
                void complete(StringView line, size_t wordStart, CompletionSink &sink) override;

        private:
                void runInput();
                void endInterrupt();

                Console            &_console;
                Span<const Command> _commands;
                FdTable             _fds;
                WorkerPool          _workers;
                ShellState          _state;
                Executor            _executor;
                String              _input;  // lines of an incomplete command
                String              _prompt; // what the editor shows
                LineEditor          _editor;
};

BMDS_NAMESPACE_END
