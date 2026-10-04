/**
 * @file      shell.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdint>
#include <bmds/assert.h>
#include <bmds/console.h>
#include <bmds/parser.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr char CtrlC = 0x03;

        WorkerPool::Config workerConfig() {
                WorkerPool::Config config;
                config.thread.name = "pipe";
                config.thread.stackBytes = Shell::WorkerStackBytes;
                config.thread.priority = Shell::WorkerPriority;
                config.thread.stackSpace = MemSpace::Psram;
                config.maxWorkers = Shell::MaxStages - 1;
                config.maxIdle = 2;
                return config;
        }

} // namespace

Shell::Shell(Console &console, Span<const Command> commands, size_t stackBytes)
    : _console(console), _commands(commands), _workers(workerConfig()), _executor(*this, _state, _fds, stackBytes),
      _editor(console) {
        // findCommand() binary-searches, so the table must be sorted and unique.
        for (size_t i = 1; i < _commands.size(); ++i) {
                BMDS_ASSERT(StringView(_commands[i - 1].name) < StringView(_commands[i].name));
        }
}

Error Shell::init() {
        // One open of the console for all three, as a terminal's are.
        auto [handle, err] = FileHandle::open(_console, FileHandle::ReadWrite);
        if (err.isError()) return err;
        (void)_fds.set(FdTable::Stdin, handle);
        (void)_fds.set(FdTable::Stdout, handle);
        (void)_fds.set(FdTable::Stderr, handle);
        if (err.isOk()) err = _state.vars.set("PS1", "bmds> ");
        if (err.isOk()) err = _state.vars.set("PS2", "> ");
        if (err.isOk()) err = _state.vars.set("IFS", " \t\n");
        return err;
}

const Command *Shell::findCommand(StringView name) const {
        size_t lo = 0;
        size_t hi = _commands.size();
        while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                const auto   cmp = name <=> StringView(_commands[mid].name);
                if (cmp == 0) return &_commands[mid];
                if (cmp < 0) {
                        hi = mid;
                } else {
                        lo = mid + 1;
                }
        }
        return nullptr;
}

void Shell::start() {
        const String *prompt = _state.vars.value(isContinuing() ? "PS2" : "PS1");
        // The editor keeps a view, so keep our own copy (the variable may change).
        (void)_prompt.assign(prompt != nullptr ? prompt->view() : StringView());
        _editor.start(_prompt);
        return;
}

void Shell::feed(char c) {
        switch (_editor.feed(c)) {
                case LineEditor::Event::Line:
                        (void)execute(_editor.line());
                        start();
                        break;
                case LineEditor::Event::Cancel:
                        _input.clear(); // abandon an incomplete command too
                        start();
                        break;
                case LineEditor::Event::Eof: // nothing to end: the console stays
                case LineEditor::Event::None: break;
        }
        return;
}

int Shell::execute(StringView line) {
        if (_input.append(line).isError() || _input.append('\n').isError()) {
                _executor.report("out of memory");
                _input.clear();
                _state.lastStatus = ExitFailure;
                return _state.lastStatus;
        }
        runInput();
        return _state.lastStatus;
}

// Runs the complete commands in _input, keeping an incomplete tail for PS2.
void Shell::runInput() {
        Arena &arena = _executor.arena();
        {
                Parser parser(_input, arena);
                size_t consumed = 0;
                for (;;) {
                        auto [node, err] = parser.next();
                        if (err == Error::NotReady) break;
                        if (err.isError()) {
                                if (err == Error::NoMem) {
                                        _executor.report("out of memory");
                                } else {
                                        String message;
                                        (void)appendSyntaxError(parser.error(), message);
                                        _executor.report("%s", message.cStr());
                                }
                                _state.lastStatus = ExitUsage;
                                consumed = _input.size();
                                break;
                        }
                        consumed = parser.position();
                        if (node == nullptr) break;
                        (void)_executor.execute(node);
                        if (_executor.flow() == Executor::Flow::Interrupt || _console.interrupt().isRaised()) {
                                endInterrupt();
                                consumed = _input.size();
                                break;
                        }
                        // A failed arithmetic or assignment abandons just that command.
                        if (_executor.flow() == Executor::Flow::Abort) {
                                _executor.clearFlow();
                                continue;
                        }
                        // At the prompt, `exit` and stray break/continue/return end the input here.
                        if (_executor.flow() != Executor::Flow::Normal) {
                                _executor.clearFlow();
                                consumed = _input.size();
                                break;
                        }
                }
                _input.remove(0, consumed);
        }
        arena.reset();
        return;
}

void Shell::endInterrupt() {
        // The console swallowed the ^C, so echo it here, as a terminal would.
        (void)_console.write(StringView("^C\n"), WaitForever);
        _executor.clearFlow();
        _state.lastStatus = ExitInterrupted;
        _console.interrupt().clear();
        return;
}

int Shell::runScript(StringView source) {
        const int status = _executor.runSource(source, true);
        if (_executor.flow() == Executor::Flow::Interrupt || _console.interrupt().isRaised()) {
                endInterrupt();
        } else {
                _executor.clearFlow();
        }
        (void)status;
        _executor.arena().reset();
        return _state.lastStatus;
}

void Shell::run() {
        start();
        for (;;) {
                uint8_t buf[32];
                // An idle shell waits for input indefinitely; it uses no CPU
                // while it waits.
                auto [n, err] = _console.read(WritableByteSpan(buf), WaitForever);
                if (err == Error::Interrupted) {
                        // Ctrl-C at the prompt: the console turned it into an
                        // interrupt; the editor cancels the line as for a raw ^C.
                        _console.interrupt().clear();
                        feed(CtrlC);
                        continue;
                }
                if (err.isError()) continue;
                for (size_t i = 0; i < n; ++i) feed(static_cast<char>(buf[i]));
        }
}

int Shell::runHelp(CommandContext &ctx) {
        Span<char *const> args = ctx.args();
        if (args.size() > 2) return ctx.usage();
        Shell &shell = ctx.shell();
        if (args.size() == 2) {
                const Command *cmd = shell.findCommand(args[1]);
                if (cmd == nullptr) {
                        ctx.eprintf("help: no such command: %s\n", args[1]);
                        return ExitFailure;
                }
                if (cmd->usage[0] == '\0') {
                        ctx.printf("usage: %s\n%s\n", cmd->name, cmd->help);
                } else {
                        ctx.printf("usage: %s %s\n%s\n", cmd->name, cmd->usage, cmd->help);
                }
                return ExitSuccess;
        }
        int width = 0;
        for (const Command &cmd : shell.commands()) {
                const int len = static_cast<int>(StringView(cmd.name).size());
                if (len > width) width = len;
        }
        for (const Command &cmd : shell.commands()) {
                if (ctx.isInterrupted()) return ExitInterrupted;
                ctx.printf("%-*s  %s\n", width, cmd.name, cmd.help);
        }
        return ExitSuccess;
}

BMDS_NAMESPACE_END
