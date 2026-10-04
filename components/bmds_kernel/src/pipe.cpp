/**
 * @file      pipe.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <new>
#include <bmds/deadline.h>
#include <bmds/memory.h>
#include <bmds/pipe.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The ring storage follows the Pipe object in the same allocation.
        constexpr size_t HeaderBytes = (sizeof(Pipe) + 7) & ~static_cast<size_t>(7);

        static_assert((Pipe::DefaultBytes & (Pipe::DefaultBytes - 1)) == 0, "pipe size: power of two");

} // namespace

Pipe::Pipe(WritableByteSpan storage, Interrupt *interrupt) : _ring(storage), _interrupt(interrupt) {}

Result<Pipe::Ends> Pipe::create(Interrupt *interrupt, size_t bytes) {
        if (bytes == 0 || (bytes & (bytes - 1)) != 0) return makeError<Ends>(Error::Invalid);
        auto *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, HeaderBytes + bytes));
        if (block == nullptr) return makeError<Ends>(Error::NoMem);
        Pipe *pipe = new (block) Pipe(WritableByteSpan(block + HeaderBytes, bytes), interrupt);

        Ends ends;
        auto [readEnd, readErr] = FileHandle::open(*pipe, FileHandle::Read);
        if (readErr.isError()) {
                pipe->endClosed();
                pipe->endClosed();
                return makeError<Ends>(readErr);
        }
        ends.read = std::move(readEnd);
        auto [writeEnd, writeErr] = FileHandle::open(*pipe, FileHandle::Write);
        if (writeErr.isError()) {
                pipe->endClosed(); // the write end that never was; dropping the read end frees the pipe
                return makeError<Ends>(writeErr);
        }
        ends.write = std::move(writeEnd);
        return makeResult(std::move(ends));
}

Result<size_t> Pipe::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        Deadline deadline(timeoutMs);
        if (_readLock.lock(deadline).isError()) return makeError<size_t>(Error::Timeout);
        Result<size_t> result;
        for (;;) {
                size_t n = _ring.pop(buf);
                if (n == 0 && !_writerOpen.load()) {
                        // The writer may have written, then closed, since the pop.
                        n = _ring.pop(buf);
                        if (n == 0) {
                                result = makeError<size_t>(Error::EndOfFile);
                                break;
                        }
                }
                if (n > 0) {
                        _writable.release();
                        result = makeResult(n);
                        break;
                }
                Error err = Interrupt::wait(_interrupt, _readable, deadline);
                if (err.isError()) {
                        result = makeError<size_t>(err);
                        break;
                }
        }
        _readLock.unlock();
        return result;
}

Result<size_t> Pipe::write(ByteSpan data, unsigned int timeoutMs) {
        if (data.isEmpty()) return makeResult(size_t(0));
        Deadline deadline(timeoutMs);
        if (_writeLock.lock(deadline).isError()) return Result<size_t>(0, Error::Timeout);
        size_t done = 0;
        Error  err;
        for (;;) {
                if (!_readerOpen.load()) {
                        err = Error::BrokenPipe;
                        break;
                }
                const size_t n = _ring.push(data.mid(done));
                if (n > 0) {
                        done += n;
                        _readable.release();
                }
                if (done == data.size()) break;
                err = Interrupt::wait(_interrupt, _writable, deadline);
                if (err.isError()) break;
        }
        _writeLock.unlock();
        return Result<size_t>(done, err);
}

void Pipe::handleClosed(uint8_t mode) {
        if (mode & FileHandle::Read) {
                _readerOpen.store(false);
                _writable.release();
        }
        if (mode & FileHandle::Write) {
                _writerOpen.store(false);
                _readable.release();
        }
        endClosed();
        return;
}

void Pipe::endClosed() {
        if (_openEnds.fetch_sub(1) != 1) return;
        // Both ends are gone, so nobody can be inside read() or write().
        this->~Pipe();
        Memory::free(this);
        return;
}

BMDS_NAMESPACE_END
