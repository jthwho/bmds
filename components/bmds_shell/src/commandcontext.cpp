/**
 * @file      commandcontext.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

Result<size_t> CommandContext::read(WritableByteSpan buf) {
        FileHandle *in = _fds.handle(FdTable::Stdin);
        if (in == nullptr) return makeError<size_t>(Error::BadDescriptor);
        return in->read(buf, WaitForever);
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
        char      buf[PrintBufferBytes];
        const int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
        if (len <= 0) return;
        const size_t n = static_cast<size_t>(len) < sizeof(buf) ? static_cast<size_t>(len) : sizeof(buf) - 1;
        (void)write(fd, ByteSpan(reinterpret_cast<const uint8_t *>(buf), n));
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
        return _outputBroken || _shell.console().interrupt().isRaised();
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
