/**
 * @file      streampair.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <new>
#include <bmds/deadline.h>
#include <bmds/memory.h>
#include <bmds/streampair.h>

BMDS_NAMESPACE_BEGIN

// One end: it reads its inbound ring and writes its outbound one (the other end's inbound).
class StreamPair::End final : public Stream {
        public:
                End(Shared &shared, WritableByteSpan inbound) : _shared(shared), _in(inbound) {}

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                using Stream::write;
                uint8_t   readiness() override;
                WaitList *readinessWaiters() override { return &_pollers; }
                void      handleClosed(uint8_t mode) override;

                End *peer = nullptr;

        private:
                friend struct StreamPair::Shared;

                Shared           &_shared;
                ByteRing          _in;
                Mutex             _readLock;
                Mutex             _writeLock;
                Semaphore         _readable{1, 0}; // released when data comes in or the other end goes
                Semaphore         _writable{1, 0}; // released when the other end frees room or goes
                WaitListOf<4>     _pollers;
                std::atomic<bool> _open{true};
};

struct StreamPair::Shared {
                std::atomic<uint8_t> openEnds{2};
                void                 endClosed();
};

namespace {

        constexpr size_t align8(size_t n) {
                return (n + 7) & ~static_cast<size_t>(7);
        }

        // The block: Shared, the two ends, then the two rings.
        constexpr size_t SharedAt = 0;

} // namespace

Result<StreamPair::Ends> StreamPair::create(size_t bytes) {
        if (bytes == 0 || (bytes & (bytes - 1)) != 0) return makeError<Ends>(Error::Invalid);
        const size_t endsAt = align8(SharedAt + sizeof(Shared));
        const size_t ringsAt = align8(endsAt + 2 * align8(sizeof(End)));
        auto        *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, ringsAt + 2 * bytes));
        if (block == nullptr) return makeError<Ends>(Error::NoMem);
        Shared *shared = new (block + SharedAt) Shared();
        End    *a = new (block + endsAt) End(*shared, WritableByteSpan(block + ringsAt, bytes));
        End    *b = new (block + endsAt + align8(sizeof(End)))
                End(*shared, WritableByteSpan(block + ringsAt + bytes, bytes));
        a->peer = b;
        b->peer = a;
        Ends ends;
        auto [first, ferr] = FileHandle::open(*a, FileHandle::ReadWrite);
        if (ferr.isError()) {
                a->handleClosed(FileHandle::ReadWrite);
                b->handleClosed(FileHandle::ReadWrite);
                return makeError<Ends>(ferr);
        }
        ends.first = std::move(first);
        auto [second, serr] = FileHandle::open(*b, FileHandle::ReadWrite);
        if (serr.isError()) {
                b->handleClosed(FileHandle::ReadWrite); // the end that never was; dropping the first frees all
                return makeError<Ends>(serr);
        }
        ends.second = std::move(second);
        return makeResult(std::move(ends));
}

Result<size_t> StreamPair::End::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        Deadline deadline(timeoutMs);
        if (_readLock.lock(deadline).isError()) return makeError<size_t>(Error::Timeout);
        Result<size_t> result;
        for (;;) {
                size_t n = _in.pop(buf);
                if (n == 0 && !peer->_open.load()) {
                        n = _in.pop(buf); // written, then closed, since the pop
                        if (n == 0) {
                                result = makeError<size_t>(Error::EndOfFile);
                                break;
                        }
                }
                if (n > 0) {
                        peer->_writable.release();
                        peer->_pollers.notify();
                        result = makeResult(n);
                        break;
                }
                Error err = Interrupt::waitCurrent(_readable, deadline);
                if (err.isError()) {
                        result = makeError<size_t>(err);
                        break;
                }
        }
        _readLock.unlock();
        return result;
}

Result<size_t> StreamPair::End::write(ByteSpan data, unsigned int timeoutMs) {
        if (data.isEmpty()) return makeResult(size_t(0));
        Deadline deadline(timeoutMs);
        if (_writeLock.lock(deadline).isError()) return Result<size_t>(0, Error::Timeout);
        size_t done = 0;
        Error  err;
        for (;;) {
                if (!peer->_open.load()) {
                        err = Error::BrokenPipe;
                        break;
                }
                const size_t n = peer->_in.push(data.mid(done));
                if (n > 0) {
                        done += n;
                        peer->_readable.release();
                        peer->_pollers.notify();
                }
                if (done == data.size()) break;
                err = Interrupt::waitCurrent(_writable, deadline);
                if (err.isError()) break;
        }
        _writeLock.unlock();
        return Result<size_t>(done, err);
}

uint8_t StreamPair::End::readiness() {
        uint8_t    ready = 0;
        const bool other = peer->_open.load();
        if (!other) ready |= Readable | HungUp | Writable | Broken;
        if (_in.available() > 0) ready |= Readable;
        if (peer->_in.freeSpace() > 0) ready |= Writable;
        return ready;
}

void StreamPair::End::handleClosed(uint8_t mode) {
        (void)mode;
        _open.store(false);
        // The other end's reads end and its writes break.
        peer->_readable.release();
        peer->_writable.release();
        peer->_pollers.notify();
        _shared.endClosed();
        return;
}

void StreamPair::Shared::endClosed() {
        if (openEnds.fetch_sub(1) != 1) return;
        // Both ends are gone, so nobody can be inside read() or write().
        auto *block = reinterpret_cast<uint8_t *>(this) - SharedAt;
        End  *a = reinterpret_cast<End *>(block + align8(SharedAt + sizeof(Shared)));
        End  *b = reinterpret_cast<End *>(reinterpret_cast<uint8_t *>(a) + align8(sizeof(End)));
        a->~End();
        b->~End();
        this->~Shared();
        Memory::free(block);
        return;
}

BMDS_NAMESPACE_END
