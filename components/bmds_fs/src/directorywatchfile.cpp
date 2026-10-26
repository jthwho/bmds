/**
 * @file      directorywatchfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A folder's watch as a file (devplan/guiprograms.md §14): the SDK's fs_watch. A read gives the
 * changes since the last read as one uint32_t, and waits while there are none.
 */

#include <cstring>
#include <new>
#include <bmds/deadline.h>
#include <bmds/directorywatch.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

namespace {

        class WatchStream final : public Stream {
                public:
                        Error start(StringView path, const Vfs::Context &context) {
                                _watch.setNotify(changed, this);
                                return _watch.watch(path, context);
                        }

                        Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override {
                                if (buf.size() < sizeof(uint32_t) || buf.size() % sizeof(uint32_t) != 0)
                                        return makeError<size_t>(Error::Invalid);
                                Deadline deadline(timeoutMs);
                                if (_readLock.lock(deadline).isError()) return makeError<size_t>(Error::Timeout);
                                Result<size_t> result;
                                for (;;) {
                                        const uint32_t changes = _watch.takeChanges();
                                        if (changes != 0) {
                                                std::memcpy(buf.data(), &changes, sizeof(changes));
                                                result = makeResult(sizeof(changes));
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

                        uint8_t readiness() override { return _watch.hasChanges() ? Readable : 0; }

                        WaitList *readinessWaiters() override { return &_pollers; }

                        // Its only handle (it is opened once) went: the watch ends.
                        void handleClosed(uint8_t) override {
                                _watch.stop(); // no notify after this: then the rest may go
                                this->~WatchStream();
                                Memory::free(this);
                                return;
                        }

                private:
                        // In the task that made the change, the storage lock held (must not block).
                        static void changed(void *arg) {
                                auto *self = static_cast<WatchStream *>(arg);
                                self->_changed.release();
                                self->_pollers.notify();
                                return;
                        }

                        DirectoryWatch _watch; // stops in its destructor
                        Mutex          _readLock;
                        Semaphore      _changed{1, 0}; // released at each change told of
                        WaitListOf<4>  _pollers;
        };

} // namespace

Result<FileHandle::Ptr> DirectoryWatch::openFile(StringView path, const Vfs::Context &context) {
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(WatchStream));
        if (memory == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *stream = ::new (memory) WatchStream();
        Error err = stream->start(path, context);
        if (err.isOk()) {
                auto [handle, herr] = FileHandle::open(*stream, FileHandle::Read);
                if (herr.isOk()) return makeResult(std::move(handle));
                err = herr;
        }
        stream->~WatchStream();
        Memory::free(stream);
        return makeError<FileHandle::Ptr>(err);
}

BMDS_NAMESPACE_END
