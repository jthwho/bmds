/**
 * @file      sysconfwatch.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A watch as a file (devplan/sysconf.md §12.2): the SDK's sysconf_watch. A
 * read gives the ids of the parameters changed as uint32_t records, and
 * waits while there are none; io_poll sees it readable when there are.
 */

#include <cstring>
#include <new>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/sysconf.h>

BMDS_NAMESPACE_BEGIN

namespace SysConf {

        namespace {

                class WatchStream final : public Stream {
                        public:
                                Error start(StringView prefix) { return _watcher.start(prefix, changed, this); }

                                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override {
                                        if (buf.size() < sizeof(uint32_t) || buf.size() % sizeof(uint32_t) != 0)
                                                return makeError<size_t>(Error::Invalid);
                                        Deadline deadline(timeoutMs);
                                        if (_readLock.lock(deadline).isError())
                                                return makeError<size_t>(Error::Timeout);
                                        Result<size_t> result;
                                        for (;;) {
                                                const size_t n = take(buf);
                                                if (n > 0) {
                                                        result = makeResult(n);
                                                        break;
                                                }
                                                const Error err = Interrupt::waitCurrent(_changed, deadline);
                                                if (err.isError()) {
                                                        result = makeError<size_t>(err);
                                                        break;
                                                }
                                        }
                                        _readLock.unlock();
                                        return result;
                                }

                                Result<size_t> write(ByteSpan, unsigned int) override {
                                        return makeError<size_t>(Error::NotSupported);
                                }

                                uint8_t readiness() override { return _watcher.isPending() ? Readable : 0; }

                                WaitList *readinessWaiters() override { return &_pollers; }

                                // Its only handle (it is opened once) went: the watch ends.
                                void handleClosed(uint8_t) override {
                                        _watcher.stop();
                                        this->~WatchStream();
                                        Memory::free(this);
                                        return;
                                }

                        private:
                                // The records that fit in @p buf: the overflow first, then ids.
                                size_t take(WritableByteSpan buf) {
                                        size_t n = 0;
                                        auto   put = [&](uint32_t record) {
                                                std::memcpy(buf.data() + n, &record, sizeof(record));
                                                n += sizeof(record);
                                        };
                                        if (_watcher.overflowed()) put(OverflowRecord);
                                        Id id;
                                        while (n + sizeof(uint32_t) <= buf.size() && _watcher.next(id))
                                                put(uint32_t(id));
                                        return n;
                                }

                                // On the task that changed a value (must not block).
                                static void changed(void *arg) {
                                        auto *self = static_cast<WatchStream *>(arg);
                                        self->_changed.release();
                                        self->_pollers.notify();
                                        return;
                                }

                                Watcher       _watcher;
                                Mutex         _readLock;
                                Semaphore     _changed{1, 0}; // released at each change told of
                                WaitListOf<4> _pollers;
                };

        } // namespace

        Result<FileHandle::Ptr> openWatch(StringView prefix) {
                if (!isReady()) return makeError<FileHandle::Ptr>(Error::InvalidState);
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(WatchStream));
                if (memory == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                auto *stream = ::new (memory) WatchStream();
                Error err = stream->start(prefix);
                if (err.isOk()) {
                        auto [handle, herr] = FileHandle::open(*stream, FileHandle::Read);
                        if (herr.isOk()) return makeResult(std::move(handle));
                        err = herr;
                }
                stream->~WatchStream();
                Memory::free(stream);
                return makeError<FileHandle::Ptr>(err);
        }

} // namespace SysConf

BMDS_NAMESPACE_END
