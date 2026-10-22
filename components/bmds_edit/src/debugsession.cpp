/**
 * @file      debugsession.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The editor's side of a debugged run (devplan/editor.md §17).
 */

#include "debugsession.h"

#include <cstdio>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t ReaderStackBytes = 4096;

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

        // A path or a file name as a field: no tabs or newlines in it.
        bool isField(StringView text) {
                return !text.contains('\t') && !text.contains('\n');
        }

} // namespace

DebugSession::DebugSession(GuiApplication &app, Listener &listener) : _app(app), _listener(listener) {}

DebugSession::~DebugSession() {
        (void)finish();
        _app.cancelPosts(this);
}

Error DebugSession::start(FileHandle::Ptr events, FileHandle::Ptr commands) {
        if (_active) return Error::Busy;
        _events = std::move(events);
        _commands = std::move(commands);
        _stopped = false;
        _askedCount = 0;
        _answer.clear();
        _report.clear();
        Thread::Config config;
        config.name = "debugread";
        config.stackBytes = ReaderStackBytes;
        config.priority = 5;
        const Error err = _reader.start(config, readerMain, this);
        if (err.isError()) {
                _events.reset();
                _commands.reset();
                return err;
        }
        _readerStarted = true;
        _active = true;
        return Error::Ok;
}

String DebugSession::finish() {
        _commands.reset(); // the program, if it still runs, goes on undebugged
        if (_readerStarted) {
                _reader.join(); // the program gone: its end of the pipe closed
                _readerStarted = false;
        }
        _events.reset();
        takeLines();
        _active = false;
        _stopped = false;
        _askedCount = 0;
        _answer.clear();
        String report(MemSpace::Psram);
        report = std::move(_report);
        return report;
}

// ---- Commands

Error DebugSession::send(StringView line) {
        if (!_commands) return Error::InvalidState;
        const auto write = [&](StringView bytes) {
                size_t done = 0;
                while (done < bytes.size()) {
                        auto [n, err] = _commands->write(
                                ByteSpan(reinterpret_cast<const uint8_t *>(bytes.data()) + done, bytes.size() - done),
                                WaitForever);
                        if (err.isError() || n == 0) return false;
                        done += n;
                }
                return true;
        };
        if (!write(line) || !write("\n")) {
                _commands.reset(); // the program is gone
                return Error::BrokenPipe;
        }
        return Error::Ok;
}

Error DebugSession::ask(Asked asked, uint32_t token, StringView line) {
        if (!_stopped) return Error::InvalidState;
        if (_askedCount == MaxAsked) return Error::Busy;
        const Error err = send(line);
        if (err.isError()) return err;
        _asked[_askedCount] = asked;
        _tokens[_askedCount] = token;
        ++_askedCount;
        return Error::Ok;
}

Error DebugSession::setBreakpoint(StringView file, uint32_t line, bool on) {
        if (!isField(file)) return Error::Invalid;
        char text[320];
        std::snprintf(text, sizeof(text), "%s\t%.*s\t%u", on ? "break" : "clear", int(file.size()), file.data(),
                      unsigned(line));
        return send(StringView(text));
}

Error DebugSession::resume(Resume how) {
        static constexpr const char *Commands[] = {"go", "step", "next", "out"};
        const Error                  err = send(StringView(Commands[size_t(how)]));
        if (err.isOk()) _stopped = false;
        return err;
}

Error DebugSession::pause() {
        return _stopped ? Error::Ok : send("pause");
}

Error DebugSession::askStack() {
        return ask(Asked::Stack, 0, "stack");
}

Error DebugSession::askVars(int level, StringView path, uint32_t token) {
        if (!isField(path)) return Error::Invalid;
        String line(MemSpace::Psram);
        if (level == Globals)
                (void)line.appendf("vars\tg\t%.*s", int(path.size()), path.data());
        else
                (void)line.appendf("vars\t%d\t%.*s", level, int(path.size()), path.data());
        return ask(Asked::Vars, token, line.view());
}

Error DebugSession::askValue(int level, StringView path, uint32_t token) {
        if (!isField(path) || path.isEmpty()) return Error::Invalid;
        String line(MemSpace::Psram);
        if (level == Globals)
                (void)line.appendf("value\tg\t%.*s", int(path.size()), path.data());
        else
                (void)line.appendf("value\t%d\t%.*s", level, int(path.size()), path.data());
        return ask(Asked::Vars, token, line.view());
}

// ---- What the program writes

// Its lines, handed to the loop's thread.
void DebugSession::readerMain(void *arg) {
        auto  *self = static_cast<DebugSession *>(arg);
        String pending(MemSpace::Psram);
        for (;;) {
                uint8_t buf[512];
                auto [n, err] = self->_events->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                if (err.isError() || n == 0) break;
                if (pending.append(StringView(reinterpret_cast<const char *>(buf), n)).isError()) break;
                bool added = false;
                for (;;) {
                        const size_t nl = pending.view().indexOf('\n');
                        if (nl == StringView::NPos) break;
                        String line(MemSpace::Psram);
                        (void)line.assign(pending.view().left(nl));
                        pending.remove(0, nl + 1);
                        Mutex::Locker lock(self->_lock, WaitForever);
                        (void)self->_lines.append(std::move(line));
                        added = true;
                }
                if (added && !self->_posted.exchange(true, std::memory_order_acq_rel))
                        (void)self->_app.post(linesDue, self);
        }
        return;
}

void DebugSession::linesDue(void *arg) {
        auto *self = static_cast<DebugSession *>(arg);
        self->_posted.store(false, std::memory_order_release);
        self->takeLines();
        return;
}

void DebugSession::takeLines() {
        for (;;) {
                String line(MemSpace::Psram);
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        if (_lines.isEmpty()) break;
                        line = std::move(_lines[0]);
                        _lines.removeAt(0);
                }
                take(line.view());
        }
        return;
}

void DebugSession::take(StringView line) {
        // An answer's lines, up to its `end`.
        if (_askedCount > 0) {
                if (line == "end") {
                        answered();
                        return;
                }
                String copy(MemSpace::Psram);
                (void)copy.assign(line);
                (void)_answer.append(std::move(copy));
                return;
        }
        StringView   f[5];
        const size_t n = fields(line, f, 5);
        if (n >= 4 && f[0] == "stopped") {
                _stopped = true;
                _listener.debugStopped(f[1], f[2], number(f[3]), n == 5 ? f[4] : StringView());
                return;
        }
        // The report's own (an error, an exception), for the run's end.
        (void)_report.append(line);
        (void)_report.append('\n');
        return;
}

void DebugSession::answered() {
        const Asked    asked = _asked[0];
        const uint32_t token = _tokens[0];
        for (size_t i = 1; i < _askedCount; ++i) {
                _asked[i - 1] = _asked[i];
                _tokens[i - 1] = _tokens[i];
        }
        --_askedCount;
        List<String> lines = std::move(_answer);
        _answer = List<String>(MemSpace::Psram);
        StringView error;
        if (asked == Asked::Stack) {
                List<Frame> frames(MemSpace::Psram);
                for (const String &l : lines) {
                        StringView f[5];
                        if (fields(l.view(), f, 5) != 5 || f[0] != "frame") continue;
                        Frame frame;
                        frame.level = number(f[1]);
                        (void)frame.function.assign(f[2]);
                        (void)frame.file.assign(f[3]);
                        frame.line = number(f[4]);
                        (void)frames.append(std::move(frame));
                }
                _listener.debugStack(frames.span());
                return;
        }
        List<Var> vars(MemSpace::Psram);
        for (const String &l : lines) {
                StringView   f[5];
                const size_t n = fields(l.view(), f, 5);
                if (n >= 2 && f[0] == "error") {
                        error = f[1];
                        continue;
                }
                if (n != 5 || f[0] != "var") continue;
                Var v;
                (void)v.name.assign(f[1]);
                (void)v.type.assign(f[2]);
                (void)v.value.assign(f[3]);
                v.children = number(f[4]);
                (void)vars.append(std::move(v));
        }
        String message(MemSpace::Psram); // `error` points into lines, which outlive the call
        (void)message.assign(error);
        _listener.debugVars(token, vars.span(), message.view());
        return;
}

BMDS_NAMESPACE_END
