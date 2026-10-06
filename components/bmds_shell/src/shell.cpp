/**
 * @file      shell.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/assert.h>
#include <bmds/console.h>
#include <bmds/glob.h>
#include <bmds/memory.h>
#include <bmds/parser.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr char CtrlC = 0x03;

        // Characters a completed file name escapes with a backslash.
        constexpr StringView EscapedInNames = " \t'\"\\$`|&;()<>*?[]!{}";

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
    : _console(console), _interrupt(&console.interrupt()), _commands(commands), _workers(workerConfig()),
      _executor(*this, _state, _fds, stackBytes), _editor(console) {
        _editor.setCompleter(this);
        // findCommand() binary-searches, so the table must be sorted and unique.
        for (size_t i = 1; i < _commands.size(); ++i) {
                BMDS_ASSERT(StringView(_commands[i - 1].name) < StringView(_commands[i].name));
        }
}

int Shell::runAsProgram(const BuiltinProgram::Context &context, Span<const Command> commands, size_t stackBytes,
                        StringView command) {
        FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
        auto        fail = [&](const char *message) {
                if (err != nullptr)
                        (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(message), std::strlen(message)),
                                                WaitForever);
                return ExitCannotRun;
        };
        if (context.console == nullptr || context.fds == nullptr) return fail("sh: no terminal\n");
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(Shell));
        if (memory == nullptr) return fail("sh: out of memory\n");
        Shell       *shell = new (memory) Shell(*context.console, commands, stackBytes);
        ShellState  &state = shell->state();
        Error        e = shell->init();
        List<String> args;
        if (e.isOk()) {
                shell->_fds = *context.fds;
                shell->setLauncher(context.launcher);
                if (context.interrupt != nullptr) shell->_interrupt = context.interrupt;
                if (!context.cwd.isEmpty()) e = state.cwd.assign(context.cwd);
                if (e.isOk()) e = state.vars.set("PWD", state.cwd.view());
        }
        for (StringView entry : context.environment) {
                const size_t eq = entry.indexOf('=');
                if (e.isError() || eq == StringView::NPos || eq == 0) continue;
                const StringView name = entry.left(eq);
                e = state.vars.set(name, entry.mid(eq + 1));
                if (e.isOk()) e = state.vars.addFlags(name, Variables::Exported);
        }
        // The command does the rest (`sh`: `-c`, a script, options; an applet: its own).
        if (e.isOk()) e = args.reserve(context.args.size() + 1);
        if (e.isOk()) e = args.append(String());
        if (e.isOk()) e = args[0].assign(command);
        for (size_t i = 1; e.isOk() && i < context.args.size(); ++i) {
                auto [arg, aerr] = String::fromView(context.args[i]);
                e = aerr.isOk() ? args.append(std::move(arg)) : aerr;
        }
        int status = 0;
        {
                Interrupt::Scope scope(shell->_interrupt);
                status = e.isOk() ? shell->_executor.invoke(args, false) : fail("sh: out of memory\n");
        }
        args = List<String>();
        shell->~Shell();
        Memory::free(memory);
        return status;
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
        // The current directory starts at the root; `cd` alone goes $HOME.
        if (err.isOk()) err = _state.cwd.assign("/");
        if (err.isOk()) err = _state.vars.set("PWD", "/");
        if (err.isOk()) err = _state.vars.set("HOME", "/");
        // What programs look at to decide on escape sequences and colors.
        if (err.isOk()) err = _state.vars.set("TERM", "xterm-256color");
        if (err.isOk()) err = _state.vars.addFlags("TERM", Variables::Exported);
        if (err.isOk() && _console.colorDepth() == ColorDepth::TrueColor) {
                err = _state.vars.set("COLORTERM", "truecolor");
                if (err.isOk()) err = _state.vars.addFlags("COLORTERM", Variables::Exported);
        }
        return err;
}

void Shell::exportTerminal() {
        Variables &vars = _state.vars;
        if (_console.colorDepth() == ColorDepth::TrueColor) {
                (void)vars.set("COLORTERM", "truecolor");
                (void)vars.addFlags("COLORTERM", Variables::Exported);
        } else {
                (void)vars.unset("COLORTERM");
        }
        _sizeSeen = _console.sizeGeneration();
        const Console::Size size = _console.size();
        char                buf[8];
        std::snprintf(buf, sizeof(buf), "%u", size.columns);
        (void)vars.set("COLUMNS", buf);
        std::snprintf(buf, sizeof(buf), "%u", size.rows);
        (void)vars.set("LINES", buf);
        return;
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
        // A serial terminal may have been resized since: ask again (the answer
        // is taken out of the input as the line is typed).
        _console.requestSize();
        const String *prompt = _state.vars.value(isContinuing() ? "PS2" : "PS1");
        // The editor keeps a view, so keep our own copy (the variable may change).
        _prompt.clear();
        renderPrompt(prompt != nullptr ? prompt->view() : StringView(), _prompt);
        _editor.start(_prompt);
        return;
}

void Shell::renderPrompt(StringView ps, String &out) {
        auto var = [this](const char *name, StringView fallback) {
                const String *v = _state.vars.value(name);
                return v != nullptr ? v->view() : fallback;
        };
        String raw;
        for (size_t i = 0; i < ps.size(); ++i) {
                if (ps[i] != '\\' || i + 1 == ps.size()) {
                        (void)raw.append(ps[i]);
                        continue;
                }
                const char c = ps[++i];
                switch (c) {
                        case 'a': (void)raw.append('\a'); break;
                        case 'e': (void)raw.append('\x1b'); break;
                        case 'n': (void)raw.append('\n'); break;
                        case 'r': (void)raw.append('\r'); break;
                        case '\\': (void)raw.append("\\\\"); break; // stays escaped for the expansion below
                        case '[':
                        case ']': break; // non-printing markers: the editor doesn't need widths
                        case 'h':
                        case 'H': (void)raw.append(var("HOSTNAME", "bmds")); break;
                        case 'u': (void)raw.append(var("USER", "user")); break;
                        case 'w': {
                                // As bash: $HOME and below shown as ~.
                                const StringView pwd = var("PWD", "/");
                                const StringView home = var("HOME", "");
                                if (home.size() > 1 && pwd.startsWith(home) &&
                                    (pwd.size() == home.size() || pwd[home.size()] == '/')) {
                                        (void)raw.append('~');
                                        (void)raw.append(pwd.mid(home.size()));
                                } else {
                                        (void)raw.append(pwd);
                                }
                                break;
                        }
                        case 'W': {
                                const StringView pwd = var("PWD", "/");
                                const size_t     slash = pwd.lastIndexOf('/');
                                (void)raw.append(pwd.size() > 1 && slash != StringView::NPos ? pwd.mid(slash + 1)
                                                                                             : pwd);
                                break;
                        }
                        case 's': (void)raw.append("bmds"); break;
                        case 'v':
                        case 'V': (void)raw.append(var("BMDS_VERSION", "")); break;
                        case '$': (void)raw.append('$'); break;
                        case 'j': (void)raw.append('0'); break;
                        case 'l': (void)raw.append(_console.name()); break;
                        default:
                                if (c >= '0' && c <= '7') {
                                        int v = 0, n = 0;
                                        for (--i; n < 3 && i + 1 < ps.size() && ps[i + 1] >= '0' && ps[i + 1] <= '7';
                                             ++n)
                                                v = v * 8 + (ps[++i] - '0');
                                        (void)raw.append(static_cast<char>(v));
                                } else {
                                        (void)raw.append('\\');
                                        (void)raw.append(c);
                                }
                                break;
                }
        }
        // Then expansion, as in double quotes (bash's promptvars). On any failure,
        // show the escaped text as is.
        Arena            &arena = _executor.arena();
        const Arena::Mark mark = arena.mark();
        SyntaxError       syntax;
        auto [word, err] = parseQuotedText(raw, arena, syntax);
        String expanded;
        if (err.isOk()) err = _executor.expandString(word, expanded);
        arena.rewind(mark);
        if (err.isOk() && _executor.flow() == Executor::Flow::Normal) {
                out = std::move(expanded);
        } else {
                _executor.clearFlow();
                out = std::move(raw);
        }
        return;
}

void Shell::complete(StringView line, size_t wordStart, CompletionSink &sink) {
        const StringView word = line.mid(wordStart);
        if (word.startsWith('$')) {
                const bool       brace = word.startsWith("${");
                const StringView prefix = word.mid(brace ? 2 : 1);
                for (const Variables::Entry &e : _state.vars.entries()) {
                        if (!e.name.view().startsWith(prefix)) continue;
                        String candidate;
                        (void)candidate.append(brace ? "${" : "$");
                        (void)candidate.append(e.name);
                        if (brace) (void)candidate.append('}');
                        sink.add(candidate);
                }
                return;
        }
        // Command position: the start, after an operator, or after a word that
        // takes a command.
        size_t k = wordStart;
        while (k > 0 && (line[k - 1] == ' ' || line[k - 1] == '\t')) --k;
        bool command = k == 0 || StringView("|;&(").contains(line[k - 1]);
        if (!command) {
                size_t s = k;
                while (s > 0 && line[s - 1] != ' ' && line[s - 1] != '\t' && !StringView("|;&(").contains(line[s - 1]))
                        --s;
                const StringView previous = line.mid(s, k - s);
                for (const char *w :
                     {"then", "do", "else", "elif", "if", "while", "until", "!", "{", "command", "type", "help"}) {
                        command = command || previous == w;
                }
        }
        if (!command || word.contains('/')) {
                completePath(word, sink);
                return;
        }
        completeCommand(word, sink);
        return;
}

// Command names: built-ins, functions, keywords and the files in $PATH's
// directories (`name.elf` and `name.sh` as `name`, what runs them), sorted,
// each once. Only the names are read: telling programs from other files
// would mean opening every one.
void Shell::completeCommand(StringView word, CompletionSink &sink) {
        List<String> names;
        auto         offer = [&](StringView name) {
                if (!name.startsWith(word)) return;
                String candidate;
                for (char c : name) {
                        if (EscapedInNames.contains(c)) (void)candidate.append('\\');
                        (void)candidate.append(c);
                }
                (void)names.append(std::move(candidate));
        };
        for (const Command &c : _commands) offer(c.name);
        for (const ShellFunction &f : _state.functions) offer(f.name);
        for (const char *k : {"case", "do", "done", "elif", "else", "esac", "fi", "for", "function", "if", "in", "then",
                              "until", "while"}) {
                offer(k);
        }
        const String    *path = _state.vars.value("PATH");
        const StringView dirs = path != nullptr ? path->view() : StringView();
        size_t           at = 0;
        while (path != nullptr) {
                size_t colon = dirs.indexOf(':', at);
                if (colon == StringView::NPos) colon = dirs.size();
                const StringView dir = colon > at ? dirs.mid(at, colon - at) : StringView(".");
                auto [listing, err] = Vfs::openDirectory(dir, _executor.vfs());
                while (err.isOk()) {
                        auto [info, nerr] = listing.next();
                        if (nerr.isError()) break;
                        StringView name = info.name;
                        if (info.isDirectory() || name.startsWith('.')) continue;
                        if (name.endsWith(StringView(".elf"))) {
                                name = name.left(name.size() - 4);
                        } else if (name.endsWith(StringView(".sh"))) {
                                name = name.left(name.size() - 3);
                        }
                        offer(name);
                }
                if (err.isOk()) listing.close();
                if (colon == dirs.size()) break;
                at = colon + 1;
        }
        Glob::sortNames(names, 0);
        for (size_t i = 0; i < names.size(); ++i) {
                if (i == 0 || names[i].view() != names[i - 1].view()) sink.add(names[i]);
        }
        return;
}

// File names: the word's directory part as typed, then each matching name
// (escaped), directories with a trailing '/'; sorted as ls sorts.
void Shell::completePath(StringView word, CompletionSink &sink) {
        const size_t     slash = word.lastIndexOf('/');
        const StringView typedDir = slash == StringView::NPos ? StringView() : word.left(slash + 1);
        String           dir;
        String           prefix;
        if (typedDir.startsWith("~/")) {
                const String *home = _state.vars.value("HOME");
                if (home != nullptr) (void)dir.append(*home);
        }
        const StringView dirRest = dir.isEmpty() ? typedDir : typedDir.mid(1);
        const StringView nameRest = slash == StringView::NPos ? word : word.mid(slash + 1);
        auto             unescape = [](StringView text, String &out) {
                for (size_t i = 0; i < text.size(); ++i) {
                        if (text[i] == '\\' && i + 1 < text.size()) ++i;
                        (void)out.append(text[i]);
                }
        };
        unescape(dirRest, dir);
        unescape(nameRest, prefix);
        auto [listing, err] = Vfs::openDirectory(dir.isEmpty() ? StringView(".") : dir.view(), _executor.vfs());
        if (err.isError()) return;
        List<String> candidates;
        for (;;) {
                auto [info, nerr] = listing.next();
                if (nerr.isError()) break;
                const StringView name = info.name;
                if (name == "." || name == ".." || !name.startsWith(prefix)) continue;
                if (name.startsWith('.') && !prefix.view().startsWith('.')) continue;
                String candidate;
                (void)candidate.append(typedDir);
                for (char c : name) {
                        if (EscapedInNames.contains(c)) (void)candidate.append('\\');
                        (void)candidate.append(c);
                }
                if (info.isDirectory()) (void)candidate.append('/');
                if (candidates.append(std::move(candidate)).isError()) break;
        }
        listing.close();
        Glob::sortNames(candidates, 0);
        for (const String &c : candidates) sink.add(c);
        return;
}

void Shell::feed(char c) {
        switch (_editor.feed(c)) {
                case LineEditor::Event::Line:
                        if (_console.sizeGeneration() != _sizeSeen) exportTerminal();
                        (void)execute(_editor.line());
                        if (!_exited) start(); // no prompt after `exit`
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
        Interrupt::Scope scope(_interrupt);
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
                        if (_executor.flow() == Executor::Flow::Interrupt || interrupt().isRaised()) {
                                endInterrupt();
                                consumed = _input.size();
                                break;
                        }
                        // A failed arithmetic or assignment abandons just that command.
                        if (_executor.flow() == Executor::Flow::Abort) {
                                _executor.clearFlow();
                                continue;
                        }
                        // At the prompt, `exit` and stray break/continue/return end the input here;
                        // `exit` ends run() too.
                        if (_executor.flow() != Executor::Flow::Normal) {
                                if (_executor.flow() == Executor::Flow::Exit) _exited = true;
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
        interrupt().clear();
        return;
}

int Shell::runScript(StringView source) {
        Interrupt::Scope scope(_interrupt);
        const int        status = _executor.runSource(source, true);
        if (_executor.flow() == Executor::Flow::Interrupt || interrupt().isRaised()) {
                endInterrupt();
        } else {
                _executor.clearFlow();
        }
        (void)status;
        _executor.arena().reset();
        return _state.lastStatus;
}

int Shell::sourceIfExists(StringView path) {
        Interrupt::Scope scope(_interrupt);
        String           script;
        Error            err = _executor.readFile(path, script);
        if (err == Error::NotExist || err == Error::NoMedia) return ExitSuccess;
        if (err.isError()) {
                _executor.report("%.*s: %s", static_cast<int>(path.size()), path.data(),
                                 CommandContext::errorText(err));
                return ExitFailure;
        }
        (void)_executor.runSourced(script, Span<char *const>());
        if (_executor.flow() == Executor::Flow::Interrupt || interrupt().isRaised()) {
                endInterrupt();
        } else {
                _executor.clearFlow();
        }
        _executor.arena().reset();
        return _state.lastStatus;
}

int Shell::run() {
        Interrupt::setCurrent(_interrupt);
        start();
        while (!_exited) {
                uint8_t buf[32];
                // An idle shell waits for input indefinitely; it uses no CPU
                // while it waits.
                auto [n, err] = _console.read(WritableByteSpan(buf), WaitForever);
                if (err == Error::Interrupted) {
                        // Ctrl-C at the prompt: the console turned it into an
                        // interrupt; the editor cancels the line as for a raw ^C.
                        interrupt().clear();
                        feed(CtrlC);
                        continue;
                }
                // The terminal hung up (a pseudo-terminal's window closed): the shell ends, as on exit.
                if (err == Error::EndOfFile) break;
                if (err.isError()) continue;
                for (size_t i = 0; i < n && !_exited; ++i) feed(static_cast<char>(buf[i]));
        }
        _exited = false;
        return _state.lastStatus;
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

#if !defined(ESP_PLATFORM)
// The host has no registered command table: applets aren't run there (shellesp.cpp on the target).
int shellAppletMain(const BuiltinProgram::Context &) {
        return Shell::ExitCannotRun;
}
#endif

BMDS_NAMESPACE_END
