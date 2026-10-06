/**
 * @file      screenconsole.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A console whose terminal is a screen in memory (devplan/windows.md §4e).
 */

#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/screenconsole.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t  Chunk = 128; // output translated a piece at a time

} // namespace

ScreenConsole::ScreenConsole(const char *name, MemSpace space)
    : _name(name), _screen(space), _input(WritableByteSpan(_inputStorage, InputBytes)) {
        _screen.setReplySink(reply, this);
}

Error ScreenConsole::resize(uint16_t columns, uint16_t rows) {
        Error err;
        {
                Mutex::Locker lock(_lock, WaitForever);
                err = _screen.resize(columns, rows);
        }
        if (err.isOk()) setSize(Size{columns, rows});
        return err;
}

void ScreenConsole::setChangedHook(Changed hook, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        _changed = hook;
        _changedArg = arg;
        return;
}

void ScreenConsole::setTitleHook(Changed hook, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        _titleHook = hook;
        _titleArg = arg;
        return;
}

Result<size_t> ScreenConsole::write(ByteSpan data, unsigned int) {
        Changed hook = nullptr;
        void   *arg = nullptr;
        Changed titleHook = nullptr;
        void   *titleArg = nullptr;
        {
                Mutex::Locker  lock(_lock, WaitForever);
                const uint32_t title = _screen.titleGeneration();
                const bool     crlf = mode().nlToCrNl;
                const uint16_t column = _screen.cursorColumn(), row = _screen.cursorRow();
                char           out[Chunk + 1];
                size_t         n = 0;
                for (size_t i = 0; i < data.size(); ++i) {
                        if (n >= Chunk) {
                                _screen.write(StringView(out, n));
                                n = 0;
                        }
                        if (crlf && data[i] == '\n') out[n++] = '\r';
                        out[n++] = static_cast<char>(data[i]);
                }
                if (n > 0) _screen.write(StringView(out, n));
                const bool changed =
                        _screen.hasDamage() || _screen.cursorColumn() != column || _screen.cursorRow() != row;
                if (changed && _changed != nullptr && !_asked.exchange(true, std::memory_order_acq_rel)) {
                        hook = _changed;
                        arg = _changedArg;
                }
                if (_screen.titleGeneration() != title) {
                        titleHook = _titleHook;
                        titleArg = _titleArg;
                }
        }
        // Without the lock: the hooks may wait for whoever draws, who takes it.
        if (hook != nullptr) hook(arg);
        if (titleHook != nullptr) titleHook(titleArg);
        return makeResult(data.size());
}

size_t ScreenConsole::pushInput(ByteSpan data) {
        Mutex::Locker lock(_inputLock, WaitForever);
        return push(data);
}

size_t ScreenConsole::push(ByteSpan data) {
        size_t taken = 0;
        size_t from = 0;
        if (signalsEnabled()) {
                // Ctrl-C, Ctrl-\ and Ctrl-Z signal the foreground group now, and drop
                // the input before them (what the ring holds, at the next read).
                for (size_t i = 0; i < data.size(); ++i) {
                        const int sig = signalFor(data[i]);
                        if (sig == 0) continue;
                        taken += i + 1 - from;
                        from = i + 1;
                        _flushTo.store(_input.writePosition(), std::memory_order_relaxed);
                        _flush.store(true, std::memory_order_release);
                        sendSignal(sig);
                }
        }
        taken += _input.push(ByteSpan(data.data() + from, data.size() - from));
        if (taken > 0) {
                _ready.release();
                inputArrived();
        }
        return taken;
}

void ScreenConsole::reply(void *context, StringView text) {
        // On the writer's task, with the screen locked: only the input's lock is taken.
        auto         *self = static_cast<ScreenConsole *>(context);
        Mutex::Locker lock(self->_inputLock, WaitForever);
        (void)self->push(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return;
}

Result<size_t> ScreenConsole::tryReadInput(WritableByteSpan buf) {
        if (_flush.exchange(false, std::memory_order_acquire))
                _input.discardTo(_flushTo.load(std::memory_order_relaxed));
        const size_t n = _input.pop(buf);
        if (n == 0) return makeError<size_t>(Error::WouldBlock);
        return makeResult(n);
}

Result<size_t> ScreenConsole::readInput(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        Deadline   deadline(timeoutMs);
        Interrupt *interrupt = Interrupt::current();
        for (;;) {
                // An interrupt comes before input typed after it.
                if (interrupt != nullptr && interrupt->isRaised()) return makeError<size_t>(Error::Interrupted);
                Result<size_t> r = tryReadInput(buf);
                if (r.isOk()) return r;
                const Error err = Interrupt::wait(interrupt, _ready, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

BMDS_NAMESPACE_END
