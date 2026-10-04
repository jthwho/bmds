/**
 * @file      command.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Shell commands and what they run with.
 *
 * Commands are registered at compile time, from any component:
 *
 * @code
 * #include <bmds/shell.h>
 *
 * namespace {
 *         int runUptime(bmds::CommandContext &ctx) {
 *                 if (ctx.args().size() != 1) return ctx.usage();
 *                 ctx.printf("up %u s\n", seconds);
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

#include <cstdarg>
#include <cstddef>
#include <bmds/color.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fdtable.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class CommandContext;
class Executor;
class Shell;

/**
 * @brief A shell command. Create with `BMDS_SHELL_COMMAND`; instances live in rodata.
 */
struct Command {
                const char *name;  ///< What the user types.
                const char *usage; ///< Argument synopsis (may be empty), e.g. `"[command]"`.
                const char *help;  ///< One-line description.

                /**
                 * @brief Runs the command.
                 * @param ctx Arguments, standard I/O and the shell.
                 * @return The exit code (`Shell::ExitSuccess`, ...).
                 */
                int (*run)(CommandContext &ctx);
};

/**
 * @brief What a running command sees: its arguments and its file descriptors.
 *
 * Commands read stdin (fd 0) and write stdout (fd 1) and stderr (fd 2)
 * through the context, never "the console", so redirection and pipelines
 * work for every command. Output waits as long as it takes (a full pipe
 * holds the writer back; a console nobody reads fails fast by itself).
 *
 * A command that loops or prints a lot checks `isInterrupted()` and
 * returns: it turns true on Ctrl-C, and once stdout is a pipe nobody
 * reads any more (what SIGPIPE does on Unix).
 *
 * @par Thread Safety
 * Belongs to the command's task. In a pipeline, stages run concurrently on
 * different tasks, each with its own context; through `shell()` they may
 * use only the command table and the console.
 */
class CommandContext {
        public:
                /** @brief Size of the `printf()` buffer; longer output is truncated. */
                static constexpr size_t PrintBufferBytes = 256;

                /**
                 * @brief Constructs a context.
                 * @param shell   The shell running the command.
                 * @param command The command.
                 * @param args    Its arguments; `args[0]` is the name.
                 * @param fds      Its file descriptors.
                 * @param executor The interpreter running it (variables, control flow).
                 */
                CommandContext(Shell &shell, const Command &command, Span<char *const> args, FdTable &fds,
                               Executor *executor)
                    : _shell(shell), _command(command), _args(args), _fds(fds), _executor(executor) {}

                CommandContext(const CommandContext &) = delete;
                CommandContext &operator=(const CommandContext &) = delete;

                /** @brief Returns the arguments; `args()[0]` is the command name. */
                Span<char *const> args() const { return _args; }

                /** @brief Returns the command being run. */
                const Command &command() const { return _command; }

                /** @brief Returns the shell (see the thread-safety note). */
                Shell &shell() { return _shell; }

                /** @brief Returns the file descriptors. */
                FdTable &fds() { return _fds; }

                /**
                 * @brief Returns the interpreter running the command.
                 *
                 * Language built-ins (`set`, `export`, `return`...) work on its
                 * state and control flow.
                 */
                Executor &executor() { return *_executor; }

                /**
                 * @brief Reads from stdin, waiting for input.
                 * @return Bytes read; 0 with `EndOfFile` at the end; `Interrupted`;
                 *         or `BadDescriptor` if stdin is closed.
                 * @blocking
                 */
                Result<size_t> read(WritableByteSpan buf);

                /**
                 * @brief Writes to descriptor @p fd, waiting until it is taken.
                 * @return `Ok`, or the error (`BrokenPipe`, `Interrupted`, `BadDescriptor`, ...).
                 * @blocking
                 */
                Error write(int fd, ByteSpan data);

                /** @brief Writes @p text to stdout. Errors show up in `isInterrupted()`. */
                void print(StringView text);

                /** @brief Writes formatted text (up to `PrintBufferBytes`) to stdout. */
                void printf(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

                /** @brief Writes @p text to stderr. */
                void eprint(StringView text);

                /** @brief Writes formatted text (up to `PrintBufferBytes`) to stderr. */
                void eprintf(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

                /**
                 * @brief Returns the colors to use on stdout: `None` unless stdout is
                 *        a terminal and `NO_COLOR` is unset, else the console's depth.
                 *
                 * For commands that color their own output (the `ls --color=auto`
                 * rule). Output sent anyway is still fitted by the console.
                 */
                ColorDepth colorDepth();

                /** @brief Returns true after Ctrl-C, or once stdout is a broken pipe. */
                bool isInterrupted() const;

                /**
                 * @brief Prints the command's usage line to stderr.
                 * @return `Shell::ExitUsage`, so a command can `return ctx.usage();`.
                 */
                int usage();

        private:
                void vwrite(int fd, const char *fmt, va_list args);

                Shell            &_shell;
                const Command    &_command;
                Span<char *const> _args;
                FdTable          &_fds;
                Executor         *_executor;
                bool              _outputBroken = false;
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
 * @param Fn    `int Fn(bmds::CommandContext &)`.
 * @param Usage Argument synopsis string (may be empty).
 * @param Help  One-line description string.
 */
#define BMDS_SHELL_COMMAND(Name, Fn, Usage, Help)                                                                      \
        [[gnu::used, gnu::section(".bmds_cmd." #Name)]] constexpr ::bmds::Command bmdsShellCommand_##Name = {          \
                #Name, Usage, Help, Fn};

/**
 * @def BMDS_SHELL_COMMAND_AS
 * @brief Registers a command whose name isn't an identifier (`:`, `[`, `.`).
 * @param Id   An identifier for the object.
 * @param Name The name, a string literal.
 */
#define BMDS_SHELL_COMMAND_AS(Id, Name, Fn, Usage, Help)                                                               \
        [[gnu::used, gnu::section(".bmds_cmd." Name)]] constexpr ::bmds::Command bmdsShellCommand_##Id = {Name, Usage, \
                                                                                                          Help, Fn};
