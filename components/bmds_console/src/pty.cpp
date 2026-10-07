/**
 * @file      pty.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pseudo-terminals (devplan/pty.md).
 */

#include <cstdio>
#include <new>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/pty.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t  Chunk = 64; // output converted (CR LF) and pushed at a time

        constinit StaticObject<Pty> ptys[Pty::MaxPtys];

} // namespace

// --- Pty: the pair, and the emulator's end -------------------------------------

Pty::Pty(uint8_t number) : _number(number) {
        std::snprintf(_console._name, sizeof(_console._name), "pty%u", number);
}

Result<Pty *> Pty::open(MemSpace space) {
        for (size_t i = 0; i < MaxPtys; ++i) {
                if (!ptys[i].isCreated()) ptys[i].create(static_cast<uint8_t>(i));
                Pty &pty = *ptys[i];
                bool free = false;
                if (!pty._inUse.compare_exchange_strong(free, true, std::memory_order_acq_rel)) continue;
                const Error err = pty.reset(space);
                if (err.isError()) {
                        pty._inUse.store(false, std::memory_order_release);
                        return makeError<Pty *>(err);
                }
                return makeResult(&pty);
        }
        return makeError<Pty *>(Error::Full);
}

// Fresh rings and state, the console's modes and size as a new terminal's.
Error Pty::reset(MemSpace space) {
        constexpr size_t RingBytes = (sizeof(ByteRing) + 7) & ~size_t(7);
        auto            *block = static_cast<uint8_t *>(Memory::alloc(space, 2 * RingBytes + OutputBytes + InputBytes));
        if (block == nullptr) return Error::NoMem;
        _block = block;
        _output = new (block) ByteRing(WritableByteSpan(block + 2 * RingBytes, OutputBytes));
        _input = new (block + RingBytes) ByteRing(WritableByteSpan(block + 2 * RingBytes + OutputBytes, InputBytes));
        _hungUp.store(false, std::memory_order_relaxed);
        _ended.store(false, std::memory_order_relaxed);
        _console._flush.store(false, std::memory_order_relaxed);
        // Left released by the last pair: drained.
        (void)_outputReady.tryAcquire();
        (void)_outputSpace.tryAcquire();
        (void)_inputReady.tryAcquire();
        // No session first: the size reset signals nobody.
        _console.setSession(0);
        _console.setForeground(0);
        _console.setUnansweredGroup(0);
        _console.setMode(Console::TermMode());
        _console.setSize(Console::Size());
        _console.interrupt().clear();
        (void)_console.takeSignals();
        return Error::Ok;
}

void Pty::release() {
        hangUp();
        programEnded();
        Mutex::Locker writers(_writeLock, WaitForever);
        Mutex::Locker inputs(_console._inputLock, WaitForever);
        _output->~ByteRing();
        _input->~ByteRing();
        Memory::free(_block);
        _block = nullptr;
        _output = nullptr;
        _input = nullptr;
        _inUse.store(false, std::memory_order_release);
        return;
}

Result<size_t> Pty::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        const Deadline deadline(timeoutMs);
        for (;;) {
                const size_t n = _output->pop(buf);
                if (n > 0) {
                        _outputSpace.release(); // a writer waiting for room
                        return makeResult(n);
                }
                if (_ended.load(std::memory_order_acquire)) {
                        // Output written just before the end is read first.
                        if (!_output->isEmpty()) continue;
                        return makeError<size_t>(Error::EndOfFile);
                }
                const Error err = Interrupt::waitCurrent(_outputReady, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

Result<size_t> Pty::write(ByteSpan data, unsigned int) {
        if (_ended.load(std::memory_order_acquire)) return makeError<size_t>(Error::BrokenPipe);
        return makeResult(_console.push(data));
}

Result<size_t> Pty::input(ByteSpan data, unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        size_t         taken = 0;
        for (;;) {
                if (_ended.load(std::memory_order_acquire)) return Result<size_t>(taken, Error::BrokenPipe);
                if (_hungUp.load(std::memory_order_acquire)) return Result<size_t>(taken, Error::EndOfFile);
                taken += _console.push(ByteSpan(data.data() + taken, data.size() - taken));
                if (taken == data.size()) return makeResult(taken);
                if (_inputSpace.acquire(deadline).isError()) return Result<size_t>(taken, Error::Timeout);
        }
}

uint8_t Pty::readiness() {
        if (_ended.load(std::memory_order_acquire)) return Readable | HungUp;
        return static_cast<uint8_t>(Writable | (!_output->isEmpty() ? Readable : 0));
}

void Pty::hangUp() {
        // Once: the session hears of it (SIGHUP), and readers get the end of input.
        if (!_hungUp.exchange(true, std::memory_order_acq_rel)) _console.sendSignal(Console::SignalHangUp);
        _inputReady.release();  // a reader of the program's end: end of input
        _outputSpace.release(); // a writer waiting for room: its output is dropped
        _inputSpace.release();  // a paste waiting for room: it ends
        _console.inputArrived();
        return;
}

void Pty::programEnded() {
        _ended.store(true, std::memory_order_release);
        _outputReady.release();
        _inputSpace.release();
        _outputWaiters.notify();
        return;
}

// --- PtyConsole: the program's end ---------------------------------------------

Result<size_t> PtyConsole::write(ByteSpan data, unsigned int timeoutMs) {
        Mutex::Locker  lock(_pty._writeLock, WaitForever);
        const Deadline deadline(timeoutMs);
        const bool     crlf = mode().nlToCrNl;
        size_t         done = 0;
        while (done < data.size()) {
                if (_pty._hungUp.load(std::memory_order_acquire)) return makeResult(data.size()); // dropped
                // A chunk, \n as CR LF; what fits now.
                uint8_t chunk[Chunk + 1];
                size_t  n = 0, used = 0;
                while (done + used < data.size() && n < Chunk) {
                        const uint8_t c = data[done + used];
                        if (crlf && c == '\n') chunk[n++] = '\r';
                        chunk[n++] = c;
                        ++used;
                }
                if (_pty._output->freeSpace() < n) {
                        // Full: the emulator reads, or the time runs out (what was written stays).
                        const Error err = _pty._outputSpace.acquire(deadline);
                        if (err.isError()) return Result<size_t>(done, Error::Timeout);
                        continue;
                }
                (void)_pty._output->push(ByteSpan(chunk, n));
                done += used;
                _pty._outputReady.release();
                _pty._outputWaiters.notify();
        }
        return makeResult(done);
}

Error PtyConsole::flush(unsigned int timeoutMs) {
        // Handed over once the emulator has read it all (or hung up).
        const Deadline deadline(timeoutMs);
        while (!_pty._output->isEmpty() && !_pty._hungUp.load(std::memory_order_acquire)) {
                if (_pty._outputSpace.acquire(deadline).isError()) return Error::Timeout;
        }
        return Error::Ok;
}

size_t PtyConsole::push(ByteSpan data) {
        Mutex::Locker lock(_inputLock, WaitForever);
        size_t        taken = 0;
        size_t        from = 0;
        if (signalsEnabled()) {
                // Ctrl-C, Ctrl-\ and Ctrl-Z signal the foreground group now, and drop
                // the input before them (what the ring holds, at the next read).
                for (size_t i = 0; i < data.size(); ++i) {
                        const int sig = signalFor(data[i]);
                        if (sig == 0) continue;
                        taken += i + 1 - from;
                        from = i + 1;
                        _flushTo.store(_pty._input->writePosition(), std::memory_order_relaxed);
                        _flush.store(true, std::memory_order_release);
                        sendSignal(sig);
                }
        }
        taken += _pty._input->push(ByteSpan(data.data() + from, data.size() - from));
        if (taken > 0) {
                _pty._inputReady.release();
                inputArrived();
        }
        return taken;
}

bool PtyConsole::hasInput() const {
        return !_pty._input->isEmpty() || _pty._hungUp.load(std::memory_order_acquire);
}

Result<size_t> PtyConsole::tryReadInput(WritableByteSpan buf) {
        if (_flush.exchange(false, std::memory_order_acquire))
                _pty._input->discardTo(_flushTo.load(std::memory_order_relaxed));
        const size_t n = _pty._input->pop(buf);
        if (n > 0) {
                _pty._inputSpace.release(); // room for a paste waiting
                return makeResult(n);
        }
        if (_pty._hungUp.load(std::memory_order_acquire)) return makeError<size_t>(Error::EndOfFile);
        return makeError<size_t>(Error::WouldBlock);
}

Result<size_t> PtyConsole::readInput(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        const Deadline deadline(timeoutMs);
        Interrupt     *interrupt = Interrupt::current();
        for (;;) {
                // An interrupt comes before input typed after it.
                if (interrupt != nullptr && interrupt->isRaised()) return makeError<size_t>(Error::Interrupted);
                Result<size_t> r = tryReadInput(buf);
                if (r.isOk() || r.error() == Error::EndOfFile) return r;
                const Error err = Interrupt::wait(interrupt, _pty._inputReady, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

BMDS_NAMESPACE_END
