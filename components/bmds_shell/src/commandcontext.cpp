/**
 * @file      commandcontext.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <bmds/clock.h>
#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/memory.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

Result<size_t> CommandContext::read(WritableByteSpan buf) {
        FileHandle *in = _fds.handle(FdTable::Stdin);
        if (in == nullptr) return makeError<size_t>(Error::BadDescriptor);
        return in->read(buf, WaitForever);
}

const char *CommandContext::errorText(Error err) {
        switch (err.code()) {
                case Error::NotExist: return "No such file or directory";
                case Error::IsDir: return "Is a directory";
                case Error::NotDir: return "Not a directory";
                case Error::Exists: return "File exists";
                case Error::NotEmpty: return "Directory not empty";
                case Error::PermissionDenied: return "Permission denied";
                case Error::NoSpace: return "No space left on device";
                case Error::NoMedia: return "No medium found";
                case Error::Busy: return "Device or resource busy";
                case Error::ReadOnly: return "Read-only file system";
                case Error::TooManyOpen: return "Too many open files";
                case Error::Invalid: return "Invalid argument";
                case Error::IoError: return "Input/output error";
                default: return err.desc();
        }
}

TimeZone CommandContext::displayZone() const {
        const String *tz = _executor != nullptr ? _executor->state().vars.value("TZ") : nullptr;
        if (tz == nullptr) return Clock::timeZone();
        // A name is looked up in the zone database (the last answer is kept
        // there, so `ls -l` doesn't read it per line); not found is UTC, as in musl.
        const StringView       text = tz->view();
        const Result<TimeZone> zone = TimeZone::isName(text) ? Vfs::zoneRules(text.startsWith(':') ? text.mid(1) : text)
                                                             : TimeZone::parse(text);
        return zone.error().isOk() ? zone.value() : TimeZone();
}

StringView CommandContext::cwd() const {
        if (_executor == nullptr || _executor->state().cwd.isEmpty()) return StringView("/");
        return _executor->state().cwd.view();
}

Vfs::Context CommandContext::vfs() const {
        return Vfs::Context{cwd(), &_shell.console()};
}

Error CommandContext::openInput(StringView path, Input &input) {
        input = Input();
        if (path == "-") {
                input._handle = _fds.handle(FdTable::Stdin);
                return input._handle != nullptr ? Error() : Error(Error::BadDescriptor);
        }
        auto [file, err] = Vfs::open(path, Vfs::Read, vfs());
        if (err.isError()) return err;
        input._file = std::move(file);
        input._handle = input._file.get();
        return Error();
}

Error CommandContext::write(int fd, ByteSpan data) {
        FileHandle *out = _fds.handle(fd);
        if (out == nullptr) return Error::BadDescriptor;
        Error err = out->write(data, WaitForever).error();
        if (err == Error::BrokenPipe && fd == FdTable::Stdout) _outputBroken = true;
        return err;
}

void CommandContext::print(StringView text) {
        (void)write(FdTable::Stdout, ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return;
}

void CommandContext::eprint(StringView text) {
        (void)write(FdTable::Stderr, ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return;
}

void CommandContext::vwrite(int fd, const char *fmt, va_list args) {
        char    buf[PrintBufferBytes];
        va_list again;
        va_copy(again, args);
        const int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
        if (len > 0 && static_cast<size_t>(len) < sizeof(buf)) {
                (void)write(fd, ByteSpan(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(len)));
        } else if (len > 0) {
                // Longer than the stack's buffer (a long usage line): formatted again whole.
                auto *big = static_cast<char *>(Memory::alloc(MemSpace::Psram, static_cast<size_t>(len) + 1));
                if (big != nullptr) {
                        std::vsnprintf(big, static_cast<size_t>(len) + 1, fmt, again);
                        (void)write(fd, ByteSpan(reinterpret_cast<const uint8_t *>(big), static_cast<size_t>(len)));
                        Memory::free(big);
                } else {
                        (void)write(fd, ByteSpan(reinterpret_cast<const uint8_t *>(buf), sizeof(buf) - 1));
                }
        }
        va_end(again);
        return;
}

void CommandContext::printf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vwrite(FdTable::Stdout, fmt, args);
        va_end(args);
        return;
}

void CommandContext::eprintf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vwrite(FdTable::Stderr, fmt, args);
        va_end(args);
        return;
}

ColorDepth CommandContext::colorDepth() {
        FileHandle *out = _fds.handle(FdTable::Stdout);
        if (out == nullptr || !out->stream().isTerminal()) return ColorDepth::None;
        const String *noColor = _executor != nullptr ? _executor->state().vars.value("NO_COLOR") : nullptr;
        if (noColor != nullptr && !noColor->isEmpty()) return ColorDepth::None;
        // Only consoles are terminals.
        return static_cast<Console &>(out->stream()).colorDepth();
}

bool CommandContext::isInterrupted() const {
        _shell.interrupt().park(); // a stopped shell process stops here (devplan/processes.md §3.7.1)
        return _outputBroken || _shell.interrupt().isRaised();
}

int CommandContext::usage() {
        if (_command.usage[0] == '\0') {
                eprintf("usage: %s\n", _command.name);
        } else {
                eprintf("usage: %s %s\n", _command.name, _command.usage);
        }
        return Shell::ExitUsage;
}

BMDS_NAMESPACE_END
