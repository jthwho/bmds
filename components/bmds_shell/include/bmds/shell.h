/**
 * @file      shell.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The BMDS command shell.
 *
 * Commands are registered at compile time, from any component:
 *
 * @code
 * #include <bmds/shell.h>
 *
 * namespace {
 *         int runUptime(bmds::Shell &shell, bmds::Span<char *const> args) {
 *                 if (args.size() != 1) return shell.usage();
 *                 shell.printf("up %u s\n", seconds);
 *                 return bmds::Shell::ExitSuccess;
 *         }
 * }
 * BMDS_SHELL_COMMAND(uptime, runUptime, "", "Show time since boot")
 * @endcode
 *
 * Command names, arguments and output formats are userspace interfaces
 * (Prime Directive 1): once shipped they only ever grow.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/lineeditor.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class Console;
class Shell;

/**
 * @brief A shell command. Create with `BMDS_SHELL_COMMAND`; instances live in rodata.
 */
struct Command {
                const char *name;  ///< What the user types.
                const char *usage; ///< Argument synopsis (may be empty), e.g. `"[command]"`.
                const char *help;  ///< One-line description.

                /**
                 * @brief Runs the command on the shell's task.
                 * @param shell The shell, for output and context.
                 * @param args  The arguments; `args[0]` is the command name.
                 * @return The exit code (`Shell::ExitSuccess`, ...).
                 */
                int (*run)(Shell &shell, Span<char *const> args);
};

/**
 * @brief A command shell running on a console.
 *
 * The owner feeds input bytes (or calls `run()`, which reads the console
 * forever). Completed lines are tokenized in place and dispatched to the
 * matching `Command`. Nothing is allocated; the shell is about 1.7 KB,
 * most of it the line editor.
 *
 * @par Thread Safety
 * Thread-affine: one task runs the shell. Commands run on that task.
 */
class Shell {
        public:
                /** @brief Most arguments in one command line, including the name. */
                static constexpr size_t MaxArgs = 16;

                /** @brief Size of the `printf()` buffer; longer output is truncated. */
                static constexpr size_t PrintBufferBytes = 256;

                /** @brief Timeout for output writes. */
                static constexpr unsigned int OutputTimeoutMs = 100;

                static constexpr int ExitSuccess = 0;    ///< Success.
                static constexpr int ExitFailure = 1;    ///< The command failed.
                static constexpr int ExitUsage = 2;      ///< Bad arguments, or a line that doesn't parse.
                static constexpr int ExitNotFound = 127; ///< No such command.

                /**
                 * @brief Returns the commands registered with `BMDS_SHELL_COMMAND`, sorted by name.
                 *
                 * Target only: the table is assembled by the linker.
                 */
                static Span<const Command> registeredCommands();

                /**
                 * @brief Starts a shell on @p console in its own task.
                 *
                 * Allocates the shell from `MemSpace::Psram` and its task stack
                 * (`CONFIG_BMDS_SHELL_STACK_BYTES`) from `MemSpace::Internal`. The
                 * shell runs until reboot. Target only.
                 *
                 * @param console The console; must outlive the shell.
                 * @param name    Task name.
                 * @return `Error::Ok` or `Error::NoMem`.
                 */
                static Error spawn(Console &console, const char *name);

                /** @brief The `help` command (registered by the built-ins). */
                static int runHelp(Shell &shell, Span<char *const> args);

                /**
                 * @brief Creates a shell.
                 * @param console  Where input comes from and output goes.
                 * @param commands Command table, sorted by name, no duplicates.
                 */
                Shell(Console &console, Span<const Command> commands);

                Shell(const Shell &) = delete;
                Shell &operator=(const Shell &) = delete;

                /** @brief Returns the console. */
                Console &console() { return _console; }

                /** @brief Returns the command table. */
                Span<const Command> commands() const { return _commands; }

                /** @brief Finds a command by name (binary search), or returns null. */
                const Command *findCommand(StringView name) const;

                /** @brief Returns the exit code of the last command run. */
                int lastExitCode() const { return _lastExitCode; }

                /** @brief Prints the first prompt. */
                void start();

                /** @brief Processes one input byte; runs a command when a line completes. */
                void feed(char c);

                /**
                 * @brief Tokenizes @p line in place and runs it.
                 * @return The exit code. An empty line returns `lastExitCode()`.
                 */
                int execute(char *line);

                /**
                 * @brief Runs the shell: prompt, read, execute, forever.
                 * @blocking Waits for console input indefinitely.
                 */
                [[noreturn]] void run();

                /** @brief Writes text to the console. */
                void print(StringView text);

                /** @brief Writes formatted text (up to `PrintBufferBytes`) to the console. */
                void printf(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

                /**
                 * @brief Prints the running command's usage.
                 * @return `ExitUsage`, so a command can `return shell.usage();`.
                 */
                int usage();

        private:
                Console            &_console;
                Span<const Command> _commands;
                const Command      *_current = nullptr;
                int                 _lastExitCode = 0;
                LineEditor          _editor;
};

BMDS_NAMESPACE_END

/**
 * @def BMDS_SHELL_COMMAND
 * @brief Registers a shell command. Use at namespace scope in a source file.
 *
 * Each command gets its own input section, `.bmds_cmd.<Name>`; the
 * `bmds_shell` linker fragment collects them sorted by name into one
 * rodata table. No constructor and no RAM.
 *
 * The object file that defines the command must be linked: nothing refers
 * to the command by symbol, so a file that holds only commands is dropped
 * from a static archive. Build such a component with `WHOLE_ARCHIVE`
 * (as `bmds_shell` does), or define commands in a file that is linked
 * anyway.
 *
 * @param Name  Command name (an identifier; it is what the user types).
 * @param Fn    `int Fn(bmds::Shell &, bmds::Span<char *const>)`.
 * @param Usage Argument synopsis string (may be empty).
 * @param Help  One-line description.
 */
#define BMDS_SHELL_COMMAND(Name, Fn, Usage, Help)                                                                      \
        [[gnu::used, gnu::section(".bmds_cmd." #Name)]] constexpr ::bmds::Command bmdsShellCommand_##Name = {          \
                #Name, Usage, Help, Fn};
