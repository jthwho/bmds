/**
 * @file      executor.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <bmds/arena.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fdtable.h>
#include <bmds/list.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/syntax.h>
#include <bmds/variables.h>

BMDS_NAMESPACE_BEGIN

class Shell;

/**
 * @brief Appends @p value quoted for re-input to the shell, as bash's `set`
 *        and `set -x` show values: as is when safe, otherwise in '...'.
 */
void appendShellQuoted(String &out, StringView value);

/** @brief A shell function: its name and the source of its body. */
struct ShellFunction {
                String name;
                String source; ///< The body and its redirections, parsed again on each call.
};

/** @brief `set -e/-u/-x`. */
struct ShellOptions {
                bool errexit = false; ///< `-e`: exit when a command fails.
                bool nounset = false; ///< `-u`: expanding an unset variable is an error.
                bool xtrace = false;  ///< `-x`: trace commands to stderr.
};

/**
 * @brief Everything a subshell gets a copy of: variables, functions,
 *        positional parameters, options, `$?` and the current directory.
 */
class ShellState {
        public:
                Variables           vars;
                List<ShellFunction> functions; ///< Sorted by name.
                List<String>        positional;
                ShellOptions        options;
                int                 lastStatus = 0;
                String              cwd; ///< Current directory (absolute, normalized); empty means `/`.

                ShellState() = default;
                ShellState(ShellState &&) = default;
                ShellState &operator=(ShellState &&) = default;

                /** @brief Returns a deep copy (for a subshell), or `NoMem`. */
                Result<ShellState> clone() const;

                /** @brief Returns the function called @p name, or null. */
                const ShellFunction *findFunction(StringView name) const;

                /** @brief Defines (or redefines) a function. */
                Error defineFunction(StringView name, StringView source);

                /** @brief Removes a function (no error if there is none). */
                void removeFunction(StringView name);

                /** @brief Returns `$-`: the option letters that are on. */
                void optionLetters(String &out) const;
};

/**
 * @brief Runs syntax trees: the shell language's interpreter.
 *
 * An Executor works on a `ShellState` and a table of file descriptors.
 * Subshells (`( ... )`, `$( ... )`) run on a child Executor over a copy of
 * the state, on the same task; earlier pipeline stages run on a child
 * Executor on a worker thread. Commands are looked up as functions, then
 * in the shell's command table.
 *
 * `break`, `continue`, `return` and `exit` (and Ctrl-C) are control flow,
 * not exceptions: they set `flow()`, and every loop and list checks it.
 *
 * Recursion is bounded twice: function calls by `MaxCallDepth`, and stack
 * use by a budget measured from where the executor started, so a runaway
 * recursion is an error message, not a stack overflow.
 *
 * @par Thread Safety
 * Thread-affine: one task runs an Executor.
 */
class Executor {
        public:
                /** @brief Control flow in progress. */
                enum class Flow : uint8_t {
                        Normal,    ///< Run the next command.
                        Break,     ///< `break n`: leave `flowCount()` loops.
                        Continue,  ///< `continue n`
                        Return,    ///< `return`: leave the function.
                        Exit,      ///< `exit`, `${x?}`, an unset variable with `-u`: leave the shell (or subshell).
                        Abort,     ///< A failed arithmetic or assignment: abandon the top-level command (as bash).
                        Interrupt, ///< Ctrl-C: abandon everything up to the prompt.
                };

                /** @brief Deepest function recursion. */
                static constexpr int MaxCallDepth = 100;

                /** @brief Stack the executor leaves free (for the commands it runs). */
                static constexpr size_t StackReserveBytes = 3072;

                /**
                 * @brief Constructs a top-level executor.
                 * @param shell       Commands, console (interrupt), workers.
                 * @param state       The state it works on.
                 * @param fds         Its file descriptors.
                 * @param stackBytes  Stack of the task it runs on (recursion budget).
                 */
                Executor(Shell &shell, ShellState &state, FdTable &fds, size_t stackBytes);

                /**
                 * @brief Constructs a child for a subshell on the same task.
                 *
                 * Shares the parent's stack budget and parse arena.
                 */
                Executor(Executor &parent, ShellState &state, FdTable &fds);

                Executor(const Executor &) = delete;
                Executor &operator=(const Executor &) = delete;

                /** @brief Runs one tree; returns its exit status (also left in `$?`). */
                int execute(const Node *node);

                /**
                 * @brief Parses and runs @p source one command at a time, as a script.
                 *
                 * Used for `eval`, function bodies and command substitution. A
                 * syntax error is reported and gives status 2.
                 *
                 * @param script True for a whole script: an `Abort` ends just the
                 *               command it happened in, and the script goes on.
                 */
                int runSource(StringView source, bool script = false);

                /** @brief Returns the control flow in progress. */
                Flow flow() const { return _flow; }

                /** @brief Returns the count of a `break`/`continue` in progress. */
                int flowCount() const { return _flowCount; }

                /** @brief Starts control flow (from `break`, `return`, `exit`...). */
                void setFlow(Flow flow, int count = 1) {
                        _flow = flow;
                        _flowCount = count;
                        return;
                }

                /** @brief Ends control flow (the top level, after `exit`). */
                void clearFlow() {
                        _flow = Flow::Normal;
                        _overflowed = false;
                        return;
                }

                /** @brief Returns the number of loops around the running command. */
                int loopDepth() const { return _loopDepth; }

                /** @brief Returns true while a function runs. */
                bool inFunction() const { return _callDepth > 0; }

                /** @brief Returns the state. */
                ShellState &state() { return _state; }

                /** @brief Returns the current file descriptors. */
                FdTable &fds() { return *_fds; }

                /** @brief Returns the shell. */
                Shell &shell() { return _shell; }

                /** @brief Returns the parse arena (shared with same-task children). */
                Arena &arena() { return *_arena; }

                /**
                 * @brief `local name[=value]`: saves @p name for restoring when the function returns.
                 * @param value The new value, or null to leave it unset.
                 * @return `Ok`, `InvalidState` outside a function, `ReadOnly`, or `NoMem`.
                 */
                Error declareLocal(StringView name, const StringView *value);

                /** @brief Writes an error message to stderr (a newline is added). */
                void report(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

                /** @brief Expands words to fields (splitting, `"$@"`); appends to @p out. */
                Error expandWords(const Word *first, List<String> &out);

                /** @brief Expands a word to one string (no splitting). */
                Error expandString(const Word *word, String &out);

        private:
                friend class Expander;
                struct LocalSave;

                bool           stackExhausted();
                int            expansionFailed();
                void           xtrace(StringView text);
                void           inheritAbort(const Executor &child);
                int            finish(int status, bool errexitApplies);
                int            runSimple(const SimpleNode *n);
                int            runPipeline(const PipelineNode *p);
                int            runCompound(const Node *n);
                int            runCompoundBody(const Node *n);
                int            runLoop(const LoopNode *n);
                int            runFor(const ForNode *n);
                int            runArithFor(const ArithForNode *n);
                int            runCase(const CaseNode *n);
                int            runSubshell(const Node *body);
                int            runCommand(List<String> &args, const Redirection *redirects);
                int            callFunction(const ShellFunction &fn, List<String> &args);
                bool           loopIteration(bool &stop);
                Error          applyRedirects(const Redirection *r, FdTable &fds);
                Error          expandPattern(const Word *word, String &out);
                static void    stageMain(void *arg);
                Error          evaluate(StringView expression, int64_t &value);
                Result<String> substitute(StringView source);
                bool           interrupted();

                Shell           &_shell;
                ShellState      &_state;
                FdTable         *_fds;
                Arena            _ownArena;
                Arena           *_arena;
                List<LocalSave> *_locals = nullptr;
                uintptr_t        _stackBase = 0;
                size_t           _stackBudget;
                Flow             _flow = Flow::Normal;
                int              _flowCount = 0;
                int              _loopDepth = 0;
                int              _callDepth = 0;
                int              _errexitSuppressed = 0;
                int              _substStatus = 0; // status of the last command substitution
                bool             _substRan = false;
                bool             _overflowed = false; // ran out of stack: abandon everything
                int              _errorStatus = 1;    // status of the last fatal expansion error
};

BMDS_NAMESPACE_END
