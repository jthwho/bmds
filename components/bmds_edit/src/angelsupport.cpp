/**
 * @file      angelsupport.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * AngelScript's tools for the editor (devplan/editor.md §9): `angel --serve` for checks and the
 * API, `angel --report-fd` for runs, `angel --debug` for debugged ones (§17).
 */

#include "languagesupport.h"

#include <cstdio>
#include <cstdlib>
#include <bmds/fdtable.h>
#include <bmds/pipe.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   ReaderStackBytes = 4096;
        constexpr uint32_t MaxStarts = 3; // the server's starts that may fail before it's given up
        constexpr int      SigTerm = 15;

        // The tab-separated fields of @p line, up to @p max.
        size_t fields(StringView line, StringView *out, size_t max) {
                size_t n = 0;
                while (n < max) {
                        const size_t tab = line.indexOf('\t');
                        out[n++] = tab == StringView::NPos ? line : line.left(tab);
                        if (tab == StringView::NPos) break;
                        line = line.mid(tab + 1);
                }
                return n;
        }

        uint32_t number(StringView text) {
                uint32_t n = 0;
                for (char c : text) {
                        if (c < '0' || c > '9') break;
                        n = n * 10 + uint32_t(c - '0');
                }
                return n;
        }

        // Each line of @p text in turn.
        template <typename Fn> void eachLine(StringView text, Fn fn) {
                while (!text.isEmpty()) {
                        const size_t nl = text.indexOf('\n');
                        fn(nl == StringView::NPos ? text : text.left(nl));
                        if (nl == StringView::NPos) break;
                        text = text.mid(nl + 1);
                }
                return;
        }

        StringView lastComponent(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

} // namespace

AngelSupport::AngelSupport(GuiApplication &app, const Context &context, Listener &listener)
    : _app(app), _context(context), _listener(listener) {
        // angel, found as a shell would (the ROM's /rom/bin is on PATH).
        StringView path;
        for (const StringView &v : context.environment)
                if (v.startsWith(StringView("PATH="))) path = v.mid(5);
        const Vfs::Context vc{context.cwd, nullptr};
        if (Vfs::findProgram("angel", path, vc, _program).isError()) _program.clear();
}

AngelSupport::~AngelSupport() {
        stopServer();
        _app.cancelPosts(this);
}

// ---- The server

Error AngelSupport::startServer() {
        if (_toServer) return Error::Ok;
        if (_program.isEmpty() || _context.launcher == nullptr) return Error::NotExist;
        if (_failures >= MaxStarts) return Error::InvalidState;
        if (_readerStarted) { // the last one's ended: its reader too
                _reader.join();
                _readerStarted = false;
        }
        auto [in, ierr] = Pipe::create();
        auto [out, oerr] = Pipe::create();
        if (ierr.isError() || oerr.isError()) return ierr.isError() ? ierr : oerr;
        auto [null, nerr] = Vfs::open("/dev/null", Vfs::Write, Vfs::Context{});
        FdTable fds;
        (void)fds.set(FdTable::Stdin, std::move(in.read));
        (void)fds.set(FdTable::Stdout, std::move(out.write));
        if (nerr.isOk()) (void)fds.set(FdTable::Stderr, std::move(null));
        static constexpr StringView Args[] = {"angel", "--serve"};
        ProgramLauncher::Request    request;
        request.path = _program.view();
        request.args = Span<const StringView>(Args, 2);
        request.environment = _context.environment;
        request.fds = &fds;
        request.cwd = _context.cwd;
        auto [pid, serr] = _context.launcher->start(request);
        if (serr.isError()) {
                ++_failures;
                return serr;
        }
        _pid = pid;
        _toServer = std::move(in.write);
        _fromServer = std::move(out.read);
        _askedCount = 0;
        Thread::Config config;
        config.name = "angelserve";
        config.stackBytes = ReaderStackBytes;
        config.priority = 5;
        Error err = _reader.start(config, readerMain, this);
        if (err.isError()) {
                stopServer();
                ++_failures;
                return err;
        }
        _readerStarted = true;
        // The API first: completion, help and the type names wait for it.
        err = send("api", StringView());
        if (err.isError()) return err;
        _asked[_askedCount] = Asked::Api;
        _tokens[_askedCount] = 0;
        ++_askedCount;
        return Error::Ok;
}

void AngelSupport::stopServer() {
        if (_toServer) {
                _toServer.reset(); // its input ends: it quits
                if (_pid > 0 && _context.launcher != nullptr) (void)_context.launcher->sendSignal(_pid, SigTerm);
        }
        _pid = -1;
        if (_readerStarted) {
                _reader.join(); // its output's end
                _readerStarted = false;
        }
        _fromServer.reset();
        _askedCount = 0;
        return;
}

Error AngelSupport::send(StringView header, StringView body) {
        if (!_toServer || _askedCount == 4) return Error::InvalidState;
        const auto write = [&](StringView bytes) {
                size_t done = 0;
                while (done < bytes.size()) {
                        auto [n, err] = _toServer->write(
                                ByteSpan(reinterpret_cast<const uint8_t *>(bytes.data()) + done, bytes.size() - done),
                                WaitForever);
                        if (err.isError() || n == 0) return false;
                        done += n;
                }
                return true;
        };
        if (!write(header) || !write("\n") || !write(body)) {
                // It's gone: started again at the next request.
                _toServer.reset();
                ++_failures;
                return Error::BrokenPipe;
        }
        return Error::Ok;
}

// Its answers, each up to an `end` line, handed to the loop's thread.
void AngelSupport::readerMain(void *arg) {
        auto  *self = static_cast<AngelSupport *>(arg);
        String pending(MemSpace::Psram);
        for (;;) {
                uint8_t buf[512];
                auto [n, err] = self->_fromServer->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                if (err.isError() || n == 0) break;
                if (pending.append(StringView(reinterpret_cast<const char *>(buf), n)).isError()) break;
                // Complete answers: up to each "end" line.
                for (;;) {
                        const StringView text = pending.view();
                        size_t           at = text.startsWith("end\n") ? 0 : text.indexOf("\nend\n");
                        if (at == StringView::NPos) break;
                        const size_t length = at == 0 && text.startsWith("end\n") ? 0 : at + 1;
                        const size_t consumed = length + 4;
                        String       answer(MemSpace::Psram);
                        (void)answer.assign(text.left(length));
                        {
                                Mutex::Locker lock(self->_lock, WaitForever);
                                (void)self->_answers.append(std::move(answer));
                        }
                        pending.remove(0, consumed);
                        if (!self->_posted.exchange(true, std::memory_order_acq_rel))
                                (void)self->_app.post(answerDue, self);
                }
        }
        return;
}

void AngelSupport::answerDue(void *arg) {
        auto *self = static_cast<AngelSupport *>(arg);
        self->_posted.store(false, std::memory_order_release);
        for (;;) {
                String answer(MemSpace::Psram);
                {
                        Mutex::Locker lock(self->_lock, WaitForever);
                        if (self->_answers.isEmpty()) break;
                        answer = std::move(self->_answers[0]);
                        self->_answers.removeAt(0);
                }
                self->take(answer.view());
        }
        return;
}

void AngelSupport::take(StringView answer) {
        if (_askedCount == 0) return;
        const Asked    asked = _asked[0];
        const uint32_t token = _tokens[0];
        for (size_t i = 1; i < _askedCount; ++i) {
                _asked[i - 1] = _asked[i];
                _tokens[i - 1] = _tokens[i];
        }
        --_askedCount;
        if (asked == Asked::Api)
                readApi(answer);
        else
                readCheck(token, answer);
        return;
}

void AngelSupport::readApi(StringView answer) {
        _api.clear();
        eachLine(answer, [&](StringView line) {
                StringView f[5];
                if (fields(line, f, 5) < 4 || f[0] != "api") return;
                Entry e;
                (void)e.kind.assign(f[1]);
                (void)e.name.assign(f[2]);
                (void)e.decl.assign(f[3]);
                if (!f[4].isEmpty()) (void)e.help.assign(f[4]);
                (void)_api.append(std::move(e));
        });
        _listener.namesChanged();
        return;
}

void AngelSupport::readCheck(uint32_t token, StringView answer) {
        List<CodeEdit::Diagnostic> diagnostics(MemSpace::Psram);
        List<Entry>                symbols(MemSpace::Psram);
        eachLine(answer, [&](StringView line) {
                StringView   f[6];
                const size_t n = fields(line, f, 6);
                if (n == 6 && f[0] == "diag") {
                        CodeEdit::Diagnostic d;
                        d.severity = f[1] == "error"     ? CodeEdit::Severity::Error
                                     : f[1] == "warning" ? CodeEdit::Severity::Warning
                                                         : CodeEdit::Severity::Note;
                        const uint32_t row = number(f[3]);
                        const uint32_t col = number(f[4]);
                        if (f[2] == _checkPath.view()) {
                                d.line = row > 0 ? row - 1 : 0;
                                d.start = col > 0 ? col - 1 : 0;
                                (void)d.message.assign(f[5]);
                        } else {
                                // In a file it includes: shown at the top, saying where.
                                const StringView file = lastComponent(f[2]);
                                (void)d.message.appendf("%.*s:%u: %.*s", int(file.size()), file.data(), unsigned(row),
                                                        int(f[5].size()), f[5].data());
                        }
                        d.end = d.start; // the word there
                        (void)diagnostics.append(std::move(d));
                } else if (n >= 4 && f[0] == "symbol") {
                        Entry e;
                        (void)e.kind.assign(f[1]);
                        (void)e.name.assign(f[2]);
                        (void)e.decl.assign(f[3]);
                        (void)symbols.append(std::move(e));
                }
        });
        // A failed build gives no symbols: the last good ones stay.
        if (!symbols.isEmpty()) {
                _symbols = std::move(symbols);
                _listener.namesChanged();
        }
        _listener.checked(token, diagnostics.span());
        return;
}

Error AngelSupport::check(uint32_t token, StringView path, StringView text) {
        for (size_t i = 0; i < _askedCount; ++i)
                if (_asked[i] == Asked::Check) return Error::Busy;
        Error err = startServer();
        if (err.isError()) return err;
        (void)_checkPath.assign(path);
        char header[300];
        std::snprintf(header, sizeof(header), "check\t%.*s\t%u", int(path.size()), path.data(), unsigned(text.size()));
        err = send(StringView(header), text);
        if (err.isError()) return err;
        _asked[_askedCount] = Asked::Check;
        _tokens[_askedCount] = token;
        ++_askedCount;
        return Error::Ok;
}

// ---- Names

String AngelSupport::typeOf(StringView owner) const {
        String type(MemSpace::Psram);
        // A variable: its declaration's type ("Window w", "const int x", "Ball@ b").
        const auto fromDecl = [&](StringView decl, StringView name) {
                if (decl.startsWith("const ")) decl = decl.mid(6);
                const size_t space = decl.indexOf(' ');
                if (space == StringView::NPos) return false;
                StringView t = decl.left(space);
                while (t.endsWith('@') || t.endsWith('&')) t = t.left(t.size() - 1);
                if (!decl.mid(space + 1).trimmed().endsWith(name)) return false;
                (void)type.assign(t);
                return true;
        };
        for (const Entry &e : _symbols)
                if (e.kind.view() == "variable" && e.name.view() == owner && fromDecl(e.decl.view(), owner))
                        return type;
        for (const Entry &e : _api)
                if (e.kind.view() == "variable" && e.name.view() == owner && fromDecl(e.decl.view(), owner))
                        return type;
        (void)type.assign(owner); // a type's or a namespace's own name
        return type;
}

void AngelSupport::complete(StringView owner, StringView prefix, List<const Entry *> &out) const {
        String qualifier(MemSpace::Psram);
        String scope(MemSpace::Psram);
        if (!owner.isEmpty()) {
                const String type = typeOf(owner);
                (void)qualifier.assign(type.view());
                (void)qualifier.append('.');
                (void)scope.assign(owner);
                (void)scope.append("::");
        }
        const auto consider = [&](const Entry &e) {
                StringView name = e.name.view();
                if (owner.isEmpty()) {
                        if (name.contains('.') || name.contains("::") || e.kind.view() == "constructor") return;
                } else if (name.startsWith(qualifier.view())) {
                        name = name.mid(qualifier.size());
                } else if (name.startsWith(scope.view()) && !name.mid(scope.size()).contains("::")) {
                        name = name.mid(scope.size());
                } else {
                        return;
                }
                if (!name.startsWith(prefix)) return;
                for (const Entry *have : out) { // overloads once
                        StringView   h = have->name.view();
                        const size_t cut = h.lastIndexOf('.') != StringView::NPos ? h.lastIndexOf('.') + 1 : 0;
                        h = h.mid(cut);
                        if (h.startsWith(scope.view())) h = h.mid(scope.size());
                        if (h == name) return;
                }
                (void)out.append(&e);
        };
        for (const Entry &e : _symbols) consider(e);
        for (const Entry &e : _api) consider(e);
        return;
}

void AngelSupport::help(StringView owner, StringView name, String &out) const {
        out.clear();
        String full(MemSpace::Psram), scoped(MemSpace::Psram);
        if (!owner.isEmpty()) {
                const String type = typeOf(owner);
                (void)full.appendf("%.*s.%.*s", int(type.size()), type.data(), int(name.size()), name.data());
                (void)scoped.appendf("%.*s::%.*s", int(owner.size()), owner.data(), int(name.size()), name.data());
        }
        const auto matches = [&](const Entry &e) {
                if (!owner.isEmpty()) return e.name.view() == full.view() || e.name.view() == scoped.view();
                return e.name.view() == name || (e.kind.view() == "constructor" && e.name.view() == name);
        };
        for (const List<Entry> *list : {&_symbols, &_api})
                for (const Entry &e : *list) {
                        if (!matches(e)) continue;
                        (void)out.append(e.decl.view());
                        (void)out.append('\n');
                        if (!e.help.isEmpty()) {
                                (void)out.append("    ");
                                (void)out.append(e.help.view());
                                (void)out.append('\n');
                        }
                }
        return;
}

void AngelSupport::typeNames(List<String> &out) const {
        out.clear();
        for (const List<Entry> *list : {&_api, &_symbols})
                for (const Entry &e : *list) {
                        const StringView k = e.kind.view();
                        if (k != "type" && k != "class" && k != "enum") continue;
                        String       name(MemSpace::Psram);
                        StringView   n = e.name.view();
                        const size_t scope = n.lastIndexOf(':');
                        if (scope != StringView::NPos) n = n.mid(scope + 1);
                        (void)name.assign(n);
                        // Sorted, once each.
                        size_t at = 0;
                        while (at < out.size() && out[at].view() < name.view()) ++at;
                        if (at < out.size() && out[at].view() == name.view()) continue;
                        (void)out.insert(at, std::move(name));
                }
        return;
}

// ---- Running

Error AngelSupport::runCommand(StringView path, int reportFd, List<String> &argv) const {
        if (_program.isEmpty()) return Error::NotExist;
        argv.clear();
        char fd[8];
        std::snprintf(fd, sizeof(fd), "%d", reportFd);
        for (StringView a : {_program.view(), StringView("--report-fd"), StringView(fd), path}) {
                String s(MemSpace::Psram);
                if (s.assign(a).isError() || argv.append(std::move(s)).isError()) return Error::NoMem;
        }
        return Error::Ok;
}

Error AngelSupport::debugCommand(StringView path, int reportFd, int commandFd, List<String> &argv) const {
        if (_program.isEmpty()) return Error::NotExist;
        argv.clear();
        char report[8], commands[8];
        std::snprintf(report, sizeof(report), "%d", reportFd);
        std::snprintf(commands, sizeof(commands), "%d", commandFd);
        for (StringView a : {_program.view(), StringView("--report-fd"), StringView(report), StringView("--debug"),
                             StringView(commands), path}) {
                String s(MemSpace::Psram);
                if (s.assign(a).isError() || argv.append(std::move(s)).isError()) return Error::NoMem;
        }
        return Error::Ok;
}

bool AngelSupport::failure(StringView report, Failure &out) const {
        bool found = false;
        eachLine(report, [&](StringView line) {
                StringView f[5];
                if (found || fields(line, f, 5) != 5) return;
                if (f[0] != "error" && f[0] != "exception") return;
                (void)out.file.assign(f[1]);
                out.line = number(f[2]);
                out.column = number(f[3]);
                (void)out.message.assign(f[4]);
                out.exception = f[0] == "exception";
                found = true;
        });
        return found;
}

BMDS_NAMESPACE_END
