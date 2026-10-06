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
#include <bmds/programlauncher.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/workerpool.h>

BMDS_NAMESPACE_BEGIN

class Console;
class ProcessControl;
class ProgramLauncher;

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
 * - Ctrl-C (`SIGINT` to the shell's process, or the console's interrupt
 *   without one) stops blocked pipes and running commands; the shell
 *   echoes `^C` and the exit status is 130.
 * - Interactive (`sh -i`, `interact()`): each foreground pipeline of
 *   programs is a job, a process group with the terminal's foreground
 *   while it runs (devplan/processes.md §3.6.1).
 * - `exit` at the prompt ends the shell; pid 1 (the serial shell) gives a
 *   new prompt instead.
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

                static constexpr int ExitSuccess = 0; ///< Success.
                static constexpr int ExitFailure = 1; ///< The command failed.
                static constexpr int ExitUsage = 2;   ///< Bad arguments, or a line that doesn't parse.
                static constexpr int ExitCannotRun =
                        126;                             ///< A file that exists but can't run (bash: "cannot execute").
                static constexpr int ExitNotFound = 127; ///< No such command.
                static constexpr int ExitInterrupted = 130; ///< Stopped by Ctrl-C (128 + SIGINT, as in bash).

                /**
                 * @brief Returns the commands registered with `BMDS_SHELL_COMMAND`, sorted by name.
                 *
                 * Target only (the table is built by the linker).
                 */
                static Span<const Command> registeredCommands();

                /**
                 * @brief What every interactive shell sources as it starts: the
                 *        environment for all of them (`devplan/storage.md` §13).
                 */
                static constexpr const char *ProfilePath = "/boot/profile";

                /** @brief What a login shell (`sh -l`, the serial shell) sources after the profile. */
                static constexpr const char *LoginScriptPath = "/boot/autoexec.sh";

                /**
                 * @brief Runs a shell as a program: `/bin/sh` for `exec`, and so for
                 *        `system()` and `popen()` (a `BuiltinProgram`).
                 *
                 * A shell on the context's descriptors, directory and environment
                 * (all exported) runs the built-in `sh` with the context's
                 * arguments (`sh -c command`, `sh script args`, `sh -i`: an
                 * interactive shell, `interact()`), then is freed.
                 * Target only (the registered commands, the configured stack).
                 * @return Its exit status; 126 without a console or memory.
                 */
                static int programMain(const BuiltinProgram::Context &context);

                /**
                 * @brief `programMain()` with a command table and a stack budget
                 *        (portable; the host tests use it): runs @p command
                 *        (`sh`, or an applet's) with the program's arguments.
                 */
                static int runAsProgram(const BuiltinProgram::Context &context, Span<const Command> commands,
                                        size_t stackBytes, StringView command);

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

                /**
                 * @brief Returns the interrupt that stops commands: the console's
                 *        (Ctrl-C), or the process's when the shell runs as `/bin/sh`
                 *        (its signals). The shell's tasks wait through it.
                 */
                Interrupt &interrupt() { return *_interrupt; }

                /** @brief Returns the command table. */
                Span<const Command> commands() const { return _commands; }

                /** @brief Finds a command by name (binary search), or returns null. */
                const Command *findCommand(StringView name) const;

                /** @brief Returns the shell's own file descriptors (what commands start from). */
                FdTable &fds() { return _fds; }

                /** @brief Returns the shell state: variables, functions, options. */
                ShellState &state() { return _state; }

                /** @brief Returns the top-level executor. */
                Executor &executor() { return _executor; }

                /** @brief Returns the worker threads that run pipeline stages. */
                WorkerPool &workers() { return _workers; }

                /**
                 * @brief Sets what runs a command whose name is a path (`./hello.elf`);
                 *        null (the default): such commands are "command not found".
                 */
                void setLauncher(ProgramLauncher *launcher) {
                        _launcher = launcher;
                        return;
                }

                /** @brief Returns the program launcher, or null. */
                ProgramLauncher *launcher() const { return _launcher; }

                /** @brief Returns the shell's process (running as `/bin/sh`), or null. */
                ProcessControl *process() const { return _process; }

                /** @brief Returns true if foreground pipelines of programs are jobs (interactive). */
                bool hasJobControl() const { return _jobControl; }

                /**
                 * @brief Takes the chance to be the program's interactive shell: true
                 *        once, for the `sh` that `runAsProgram()` runs (a nested `sh`
                 *        is a process of its own).
                 */
                bool takeProgramShell() {
                        const bool can = _programShell;
                        _programShell = false;
                        return can;
                }

                /**
                 * @brief Runs as an interactive shell: job control; `SIGINT` stops
                 *        the command running here, `SIGTERM` and `SIGQUIT` are
                 *        ignored; the profile (and with @p login, after the system
                 *        has booted, `LoginScriptPath`) sourced; then prompts until
                 *        `exit` or the end of input (never, as pid 1).
                 * @return The exit status.
                 * @blocking
                 */
                int interact(bool login);

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
                 * @brief Sources the script at @p path if it exists, as `source`
                 *        would; Ctrl-C stops it. A missing file is not an error.
                 * @return Its status, or 0 if there is none.
                 * @blocking
                 */
                int sourceIfExists(StringView path);

                /**
                 * @brief Runs the shell: prompt, read, execute, until `exit` at
                 *        the prompt or the end of its input (a hung-up terminal).
                 * @return The exit status.
                 * @blocking Waits for console input indefinitely.
                 */
                int run();

                /**
                 * @brief Renders a prompt string: backslash escapes (`\h \u \w \W \s \v
                 *        \$ \n \e \a \\ \nnn`, `\[` `\]` dropped), then expansion.
                 * @param ps   The prompt variable's value.
                 * @param out  [out] The prompt to show.
                 */
                void renderPrompt(StringView ps, String &out);

                /**
                 * @brief Sets the variables programs look at for the terminal from
                 *        the console: `COLORTERM`, `COLUMNS`, `LINES`. Done before each
                 *        typed line when the size changed, and by `term`.
                 */
                void exportTerminal();

                /** @brief Tab completion for the line editor (see `LineCompleter`). */
                void complete(StringView line, size_t wordStart, CompletionSink &sink) override;

        private:
                void completePath(StringView word, CompletionSink &sink);
                void completeCommand(StringView word, CompletionSink &sink);
                void runInput();
                void endInterrupt();
                void prepareInteractive(bool login); // target: the defaults, the boot (shellesp.cpp)

                Console            &_console;
                Interrupt          *_interrupt;
                Span<const Command> _commands;
                FdTable             _fds;
                WorkerPool          _workers;
                ShellState          _state;
                Executor            _executor;
                String              _input;  // lines of an incomplete command
                String              _prompt; // what the editor shows
                ProgramLauncher    *_launcher = nullptr;
                ProcessControl     *_process = nullptr;
                LineEditor          _editor;
                uint32_t            _sizeSeen = UINT32_MAX; // the console's size generation exported
                bool                _exited = false;        // `exit` ran at the prompt
                bool                _jobControl = false;    // interactive: programs run as jobs
                bool                _programShell = false;  // runAsProgram()'s `sh` may interact()
};

BMDS_NAMESPACE_END
