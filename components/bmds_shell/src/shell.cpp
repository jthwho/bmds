/**
 * @file      shell.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <bmds/args.h>
#include <bmds/assert.h>
#include <bmds/console.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Prompt = "bmds> ";

} // namespace

Shell::Shell(Console &console, Span<const Command> commands)
    : _console(console), _commands(commands), _editor(console) {
        // findCommand() binary-searches, so the table must be sorted and unique.
        for (size_t i = 1; i < _commands.size(); ++i) {
                BMDS_ASSERT(StringView(_commands[i - 1].name) < StringView(_commands[i].name));
        }
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
        _editor.start(Prompt);
        return;
}

void Shell::feed(char c) {
        switch (_editor.feed(c)) {
                case LineEditor::Event::Line:
                        (void)execute(_editor.lineData());
                        start();
                        break;
                case LineEditor::Event::Cancel: start(); break;
                case LineEditor::Event::Eof: // nothing to end: the console stays
                case LineEditor::Event::None: break;
        }
        return;
}

int Shell::execute(char *line) {
        char *argv[MaxArgs];
        auto [argc, err] = tokenizeArgs(line, Span<char *>(argv));
        if (err.isError()) {
                if (err == Error::Full) {
                        printf("too many arguments (at most %u)\n", static_cast<unsigned>(MaxArgs));
                } else {
                        print("unterminated quote\n");
                }
                _lastExitCode = ExitUsage;
                return _lastExitCode;
        }
        if (argc == 0) return _lastExitCode;

        const Command *cmd = findCommand(argv[0]);
        if (cmd == nullptr) {
                printf("%s: command not found\n", argv[0]);
                _lastExitCode = ExitNotFound;
                return _lastExitCode;
        }
        _current = cmd;
        _lastExitCode = cmd->run(*this, Span<char *const>(argv, argc));
        _current = nullptr;
        return _lastExitCode;
}

void Shell::run() {
        start();
        for (;;) {
                uint8_t buf[32];
                // An idle shell waits for input indefinitely; it uses no CPU
                // while it waits.
                auto [n, err] = _console.read(WritableByteSpan(buf), WaitForever);
                if (err.isError()) continue;
                for (size_t i = 0; i < n; ++i) feed(static_cast<char>(buf[i]));
        }
}

void Shell::print(StringView text) {
        (void)_console.write(text, OutputTimeoutMs);
        return;
}

void Shell::printf(const char *fmt, ...) {
        char    buf[PrintBufferBytes];
        va_list args;
        va_start(args, fmt);
        const int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (len <= 0) return;
        print(StringView(buf, static_cast<size_t>(len) < sizeof(buf) ? static_cast<size_t>(len) : sizeof(buf) - 1));
        return;
}

int Shell::usage() {
        if (_current != nullptr) {
                if (_current->usage[0] == '\0') {
                        printf("usage: %s\n", _current->name);
                } else {
                        printf("usage: %s %s\n", _current->name, _current->usage);
                }
        }
        return ExitUsage;
}

int Shell::runHelp(Shell &shell, Span<char *const> args) {
        if (args.size() > 2) return shell.usage();
        if (args.size() == 2) {
                const Command *cmd = shell.findCommand(args[1]);
                if (cmd == nullptr) {
                        shell.printf("help: no such command: %s\n", args[1]);
                        return ExitFailure;
                }
                if (cmd->usage[0] == '\0') {
                        shell.printf("usage: %s\n%s\n", cmd->name, cmd->help);
                } else {
                        shell.printf("usage: %s %s\n%s\n", cmd->name, cmd->usage, cmd->help);
                }
                return ExitSuccess;
        }
        int width = 0;
        for (const Command &cmd : shell.commands()) {
                const int len = static_cast<int>(StringView(cmd.name).size());
                if (len > width) width = len;
        }
        for (const Command &cmd : shell.commands()) shell.printf("%-*s  %s\n", width, cmd.name, cmd.help);
        return ExitSuccess;
}

BMDS_NAMESPACE_END
