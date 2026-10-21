/**
 * @file      servedvolume.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A filesystem served by a program (bmds/servedfs.h, devplan/netfs.md step 2).
 * Each call is a request (`Pending`): queued for the server's reads, answered
 * by its writes, waited for up to the timeout. A request's paths, its write
 * data and the buffer its reply goes into are the caller's own, which waits
 * meanwhile: the server's read copies from them and its write into them, both
 * under the volume's lock, and a caller giving up takes its request back
 * under it, so nothing is touched after.
 */

#include <atomic>
#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/servedfs.h>
#include <bmds/string.h>
#include <bmds/waitlist.h>

#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using namespace ServedFs;

        constexpr size_t AttrEntries = 32;
        constexpr size_t DirBuffer = 4096; // a READDIR's entries at most

        uint64_t hashOf(StringView path) {
                uint64_t h = 14695981039346656037ull;
                for (char c : path) h = (h ^ uint8_t(c)) * 1099511628211ull;
                return h;
        }

        StringView lastComponent(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        FileType typeOf(uint32_t type) {
                switch (type) {
                        case TypeDirectory: return FileType::Directory;
                        case TypeDevice: return FileType::Device;
                        case TypeLink: return FileType::Link;
                        default: return FileType::File;
                }
        }

        // A caller's request, while it waits for its reply.
        struct Pending {
                        bool             used = false;
                        bool             sent = false;     // the server has read it
                        bool             answered = false; // its reply came
                        uint32_t         id = 0;
                        Request          request;
                        StringView       path;
                        StringView       path2;
                        ByteSpan         data; // WRITE's
                        WritableByteSpan out;  // the reply's payload goes here
                        Reply            reply;
                        size_t           got = 0; // payload bytes copied into out
                        Error            error;
                        Semaphore        done{1, 0};
        };

        // A STAT answer kept a while.
        struct Attr {
                        uint64_t hash = 0;
                        int64_t  atNs = 0; // 0: empty
                        FileType type = FileType::File;
                        uint64_t size = 0;
                        int64_t  modifiedNs = 0;
        };

        class ServedVolume;

        // The server's end: requests read, replies written.
        class ServerEnd final : public Stream {
                public:
                        explicit ServerEnd(ServedVolume &volume) : _volume(volume) {}
                        Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                        Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                        using Stream::write;
                        uint8_t   readiness() override;
                        WaitList *readinessWaiters() override;
                        void      handleClosed(uint8_t mode) override;

                private:
                        ServedVolume &_volume;
        };

        class ServedVolume final : public Volume {
                public:
                        explicit ServedVolume(const Config &config) : server(*this) {
                                std::memcpy(_type, config.type.data(), config.type.size());
                                _type[config.type.size()] = '\0';
                                maxData = config.maxData;
                                timeoutMs = config.timeoutMs;
                                attrNs = int64_t(config.attrMs) * 1'000'000;
                                errorOf = config.errorOf;
                        }

                        // ---- Volume
                        void release() override {
                                if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
                                this->~ServedVolume();
                                Memory::free(this);
                                return;
                        }
                        bool         locksItself() const override { return true; }
                        bool         hasLinks() const override { return true; }
                        void         retain() override { refs.fetch_add(1, std::memory_order_relaxed); }
                        void         unmounted() override { end(); }
                        BlockDevice *device() const override { return nullptr; }
                        const char  *typeName() const override { return _type; }

                        Result<FsInfo> info() override {
                                Request r;
                                r.op = StatFs;
                                Info        i;
                                Reply       reply;
                                size_t      got = 0;
                                const Error err =
                                        call(r, StringView("/"), StringView(), ByteSpan(),
                                             WritableByteSpan(reinterpret_cast<uint8_t *>(&i), sizeof(i)), reply, got);
                                if (err.isError()) return makeError<FsInfo>(err);
                                if (got < sizeof(i)) return makeError<FsInfo>(Error::IoError);
                                FsInfo f;
                                f.type = _type;
                                f.totalBytes = i.total;
                                f.freeBytes = i.free;
                                f.clusterBytes = maxData;
                                return makeResult(f);
                        }

                        Error                   stat(StringView path, FileInfo &info, bool &noPath) override;
                        Result<FileHandle::Ptr> open(StringView path, uint8_t flags, const FileIdentity &identity,
                                                     bool &noPath, String *link) override;
                        Result<Reader *>        openReader(StringView path) override;
                        void                    closeReader(Reader *reader) override;

                        Error mkdir(StringView path, bool &noPath) override {
                                noPath = false;
                                return simple(MakeDir, path);
                        }
                        Error rmdir(StringView path) override { return simple(RemoveDir, path); }
                        Error remove(StringView path) override { return simple(Remove, path); }

                        Error rename(StringView from, StringView to, Vfs::RenameMode mode, bool &noPath) override {
                                noPath = false;
                                Request r;
                                r.op = Rename;
                                r.flags = mode == Vfs::RenameMode::Replace ? RenameReplace : 0;
                                Reply       reply;
                                size_t      got = 0;
                                const Error err = call(r, from, to, ByteSpan(), WritableByteSpan(), reply, got);
                                forgetAll();
                                return err;
                        }

                        Error setModified(StringView path, Timestamp time) override {
                                Request r;
                                r.op = SetTime;
                                r.timeNs = time.ns;
                                Reply       reply;
                                size_t      got = 0;
                                const Error err =
                                        call(r, path, StringView(), ByteSpan(), WritableByteSpan(), reply, got);
                                forget(path);
                                return err;
                        }

                        // SYMLINK: @p target is the link's target itself (hasLinks()).
                        Error writeLink(StringView path, ByteSpan target, bool &noPath) override {
                                noPath = false;
                                Request r;
                                r.op = SymLink;
                                Reply       reply;
                                size_t      got = 0;
                                const Error err =
                                        call(r, path,
                                             StringView(reinterpret_cast<const char *>(target.data()), target.size()),
                                             ByteSpan(), WritableByteSpan(), reply, got);
                                forget(path);
                                return err;
                        }

                        Error readLink(StringView path, String &target) override {
                                target.clear();
                                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, MaxPathBytes));
                                if (buf == nullptr) return Error::NoMem;
                                Request r;
                                r.op = ReadLink;
                                r.length = MaxPathBytes;
                                Reply  reply;
                                size_t got = 0;
                                Error err = call(r, path, StringView(), ByteSpan(), WritableByteSpan(buf, MaxPathBytes),
                                                 reply, got);
                                if (err.isOk() && (got == 0 || reply.count != got)) err = Error::IoError;
                                if (err.isOk())
                                        err = target.assign(StringView(reinterpret_cast<const char *>(buf), got));
                                Memory::free(buf);
                                return err;
                        }

                        Result<Cursor *> openDirectory(StringView path, bool &noPath) override;
                        void             closeDirectory(Cursor *cursor) override;

                        // ---- Requests

                        // Sends @p r (its op and fields; @p path and @p path2 the paths, @p data a
                        // WRITE's) and waits for its reply, whose payload goes into @p out (@p got
                        // bytes). The reply's error, `NoMedia` (the server gone), `IoError` (no reply
                        // in time, or no slot).
                        Error call(Request &r, StringView path, StringView path2, ByteSpan data, WritableByteSpan out,
                                   Reply &reply, size_t &got) {
                                got = 0;
                                if (dead.load(std::memory_order_acquire)) return Error::NoMedia;
                                if (path.size() > MaxPathBytes || path2.size() > MaxPathBytes) return Error::Invalid;
                                if (slots.acquire(timeoutMs).isError()) return Error::IoError;
                                Pending *p = nullptr;
                                {
                                        Mutex::Locker l(lock, WaitForever);
                                        if (dead.load(std::memory_order_acquire)) {
                                                slots.release();
                                                return Error::NoMedia;
                                        }
                                        for (Pending &c : pending)
                                                if (p == nullptr && !c.used) p = &c;
                                        p->used = true;
                                        p->sent = p->answered = false;
                                        while (p->done.tryAcquire().isOk()) {} // a late release from before
                                        if (nextId == 0) nextId = 1;           // 0 is never an id
                                        p->id = nextId++;
                                        p->request = r;
                                        p->request.id = p->id;
                                        p->path = path;
                                        p->path2 = path2;
                                        p->data = data;
                                        p->out = out;
                                        p->got = 0;
                                        queue[(queueHead + queueCount) % MaxPending] = uint8_t(p - pending);
                                        ++queueCount;
                                }
                                requestReady.release();
                                pollers.notify();
                                (void)p->done.acquire(timeoutMs);
                                Error err;
                                {
                                        Mutex::Locker l(lock, WaitForever);
                                        if (p->answered) {
                                                reply = p->reply;
                                                got = p->got;
                                                err = p->error;
                                        } else {
                                                if (!p->sent) unqueue(uint8_t(p - pending));
                                                err = dead.load(std::memory_order_acquire) ? Error::NoMedia
                                                                                           : Error::IoError;
                                        }
                                        p->used = false;
                                }
                                slots.release();
                                return err;
                        }

                        Error simple(Op op, StringView path) {
                                Request r;
                                r.op = op;
                                Reply       reply;
                                size_t      got = 0;
                                const Error err =
                                        call(r, path, StringView(), ByteSpan(), WritableByteSpan(), reply, got);
                                forgetAll();
                                return err;
                        }

                        // A STAT answer, fresh or kept.
                        Error statOf(StringView path, Attr &out, bool fresh) {
                                const uint64_t h = hashOf(path);
                                const int64_t  now = Clock::monotonicNs();
                                if (!fresh && attrNs > 0) {
                                        Mutex::Locker l(lock, WaitForever);
                                        for (const Attr &a : attrs) {
                                                if (a.atNs != 0 && a.hash == h && now - a.atNs < attrNs) {
                                                        out = a;
                                                        return Error::Ok;
                                                }
                                        }
                                }
                                Request r;
                                r.op = Stat;
                                uint8_t     buf[sizeof(Entry) + 8];
                                Reply       reply;
                                size_t      got = 0;
                                const Error err = call(r, path, StringView(), ByteSpan(),
                                                       WritableByteSpan(buf, sizeof(buf)), reply, got);
                                if (err.isError()) return err;
                                if (got < sizeof(Entry)) return Error::IoError;
                                Entry e;
                                std::memcpy(&e, buf, sizeof(e));
                                out.hash = h;
                                out.atNs = now != 0 ? now : 1;
                                out.type = typeOf(e.type);
                                out.size = e.bytes;
                                out.modifiedNs = e.modifiedNs;
                                if (attrNs > 0) {
                                        Mutex::Locker l(lock, WaitForever);
                                        Attr         *slot = &attrs[0];
                                        for (Attr &a : attrs)
                                                if (a.atNs < slot->atNs) slot = &a; // the oldest, or an empty one
                                        *slot = out;
                                }
                                return Error::Ok;
                        }

                        void forget(StringView path) {
                                const uint64_t h = hashOf(path);
                                Mutex::Locker  l(lock, WaitForever);
                                for (Attr &a : attrs)
                                        if (a.hash == h) a.atNs = 0;
                                return;
                        }

                        void forgetAll() {
                                Mutex::Locker l(lock, WaitForever);
                                for (Attr &a : attrs) a.atNs = 0;
                                return;
                        }

                        // The server's end closed, or the mount went: calls fail from now on, those
                        // waiting at once; the server's reads end.
                        void end() {
                                if (dead.exchange(true, std::memory_order_acq_rel)) return;
                                {
                                        Mutex::Locker l(lock, WaitForever);
                                        queueCount = 0;
                                        for (Pending &p : pending)
                                                if (p.used) p.done.release();
                                }
                                for (size_t i = 0; i < MaxPending; ++i) slots.release(); // those waiting for one
                                requestReady.release();
                                pollers.notify();
                                return;
                        }

                        // Takes request @p index out of the queue (not read yet). Lock held.
                        void unqueue(uint8_t index) {
                                size_t kept = 0;
                                for (size_t i = 0; i < queueCount; ++i) {
                                        const uint8_t q = queue[(queueHead + i) % MaxPending];
                                        if (q != index) queue[(queueHead + kept++) % MaxPending] = q;
                                }
                                queueCount = kept;
                                return;
                        }

                        std::atomic<int>  refs{2}; // the mount's and the server end's
                        std::atomic<bool> dead{false};
                        Mutex             lock;
                        Pending           pending[MaxPending];
                        uint8_t           queue[MaxPending] = {}; // requests not read yet, in order
                        size_t            queueHead = 0;
                        size_t            queueCount = 0;
                        uint32_t          nextId = 1;
                        Semaphore         slots{MaxPending, MaxPending};
                        Semaphore         requestReady{1, 0};
                        WaitListOf<4>     pollers;
                        Attr              attrs[AttrEntries];
                        uint32_t          dropped = 0; // replies to nothing
                        uint32_t          maxData = DefaultData;
                        uint32_t          timeoutMs = 30'000;
                        int64_t           attrNs = 1'000'000'000;
                        ErrorOf           errorOf = nullptr;
                        ServerEnd         server;

                private:
                        char _type[MaxType] = {};
        };

        // ---- The server's end

        Result<size_t> ServerEnd::read(WritableByteSpan buf, unsigned int timeoutMs) {
                ServedVolume &v = _volume;
                if (buf.size() < requestMax(v.maxData)) return makeError<size_t>(Error::Invalid);
                const Deadline deadline(timeoutMs);
                for (;;) {
                        {
                                Mutex::Locker l(v.lock, WaitForever);
                                if (v.queueCount != 0) {
                                        Pending &p = v.pending[v.queue[v.queueHead]];
                                        v.queueHead = (v.queueHead + 1) % MaxPending;
                                        --v.queueCount;
                                        // More: the wake passed on to another reader (a server's
                                        // threads read one fd; the semaphore holds one wake).
                                        if (v.queueCount != 0) v.requestReady.release();
                                        Request r = p.request;
                                        r.pathLength = uint32_t(p.path.size());
                                        r.path2Length = uint32_t(p.path2.size());
                                        r.length = p.request.op == Write ? uint32_t(p.data.size())
                                                                         : uint32_t(p.request.length);
                                        const size_t dataBytes = p.request.op == Write ? p.data.size() : 0;
                                        r.size = uint32_t(sizeof(Request) + p.path.size() + p.path2.size() + dataBytes);
                                        uint8_t *at = buf.data();
                                        std::memcpy(at, &r, sizeof(r));
                                        at += sizeof(r);
                                        std::memcpy(at, p.path.data(), p.path.size());
                                        at += p.path.size();
                                        std::memcpy(at, p.path2.data(), p.path2.size());
                                        at += p.path2.size();
                                        if (dataBytes != 0) std::memcpy(at, p.data.data(), dataBytes);
                                        p.sent = true;
                                        return makeResult(size_t(r.size));
                                }
                                if (v.dead.load(std::memory_order_acquire)) {
                                        v.requestReady.release(); // the end told to the next reader too
                                        return makeError<size_t>(Error::EndOfFile);
                                }
                        }
                        const Error err = Interrupt::waitCurrent(v.requestReady, deadline);
                        if (err.isError()) return makeError<size_t>(err);
                }
        }

        Result<size_t> ServerEnd::write(ByteSpan data, unsigned int) {
                ServedVolume &v = _volume;
                size_t        done = 0;
                while (done < data.size()) {
                        if (data.size() - done < sizeof(Reply)) return Result<size_t>(done, Error::Invalid);
                        Reply r;
                        std::memcpy(&r, data.data() + done, sizeof(r));
                        if (r.size < sizeof(Reply) || r.size > data.size() - done)
                                return Result<size_t>(done, Error::Invalid);
                        const uint8_t *payload = data.data() + done + sizeof(Reply);
                        const size_t   payloadBytes = r.size - sizeof(Reply);
                        {
                                Mutex::Locker l(v.lock, WaitForever);
                                Pending      *p = nullptr;
                                for (Pending &c : v.pending)
                                        if (c.used && c.sent && !c.answered && c.id == r.id) p = &c;
                                if (p == nullptr) {
                                        ++v.dropped; // given up, or never asked
                                } else {
                                        const size_t n = payloadBytes < p->out.size() ? payloadBytes : p->out.size();
                                        if (n != 0) std::memcpy(p->out.data(), payload, n);
                                        p->got = n;
                                        p->reply = r;
                                        p->error = r.error == 0           ? Error(Error::Ok)
                                                   : v.errorOf != nullptr ? v.errorOf(r.error)
                                                                          : Error(Error::IoError);
                                        p->answered = true;
                                        p->done.release();
                                }
                        }
                        done += r.size;
                }
                return makeResult(done);
        }

        uint8_t ServerEnd::readiness() {
                ServedVolume &v = _volume;
                Mutex::Locker l(v.lock, WaitForever);
                const bool    dead = v.dead.load(std::memory_order_acquire);
                return uint8_t(Writable | (v.queueCount != 0 || dead ? Readable : 0) | (dead ? HungUp : 0));
        }

        WaitList *ServerEnd::readinessWaiters() {
                return &_volume.pollers;
        }

        // The server is done: the mount goes, the calls waiting fail, its reference let go.
        void ServerEnd::handleClosed(uint8_t) {
                ServedVolume &v = _volume;
                v.end();
                Fs::unmountVolume(v);
                v.release();
                return;
        }

        // ---- Files

        class ServedFile final : public Stream {
                public:
                        ServedFile(ServedVolume &volume, uint64_t handle, uint8_t flags, const FileIdentity &identity)
                            : _volume(volume), _handle(handle), _identity(identity),
                              _readable((flags & Vfs::Read) != 0), _writable((flags & Vfs::Write) != 0),
                              _append((flags & Vfs::Append) != 0) {
                                _volume.retain();
                        }

                        Error setPath(StringView path) { return _path.assign(path); }
                        void  setPosition(uint64_t position) { _position = position; }

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker l(_lock, WaitForever);
                                if (!_readable) return makeError<size_t>(Error::BadDescriptor);
                                Error err = flushLocked();
                                if (err.isError()) return makeError<size_t>(err);
                                if (buf.isEmpty()) return makeResult(size_t(0));
                                // From the read-ahead buffer, filled when the position leaves it.
                                if (_position < _readStart || _position >= _readStart + _readLength) {
                                        if (_readBuffer == nullptr)
                                                _readBuffer = static_cast<uint8_t *>(
                                                        Memory::alloc(MemSpace::Psram, _volume.maxData));
                                        if (_readBuffer == nullptr) return makeError<size_t>(Error::NoMem);
                                        Request r;
                                        r.op = Read;
                                        r.handle = _handle;
                                        r.offset = _position;
                                        r.length = _volume.maxData;
                                        Reply  reply;
                                        size_t got = 0;
                                        err = _volume.call(r, StringView(), StringView(), ByteSpan(),
                                                           WritableByteSpan(_readBuffer, _volume.maxData), reply, got);
                                        if (err.isError()) return makeError<size_t>(err);
                                        _readStart = _position;
                                        _readLength = got < reply.count ? got : reply.count;
                                        if (_readLength == 0) return makeError<size_t>(Error::EndOfFile);
                                }
                                const size_t at = size_t(_position - _readStart);
                                size_t       n = _readLength - at;
                                if (n > buf.size()) n = buf.size();
                                std::memcpy(buf.data(), _readBuffer + at, n);
                                _position += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan data, unsigned int) override {
                                Mutex::Locker l(_lock, WaitForever);
                                if (!_writable) return makeError<size_t>(Error::BadDescriptor);
                                if (_append && _writeLength == 0) {
                                        Attr        a;
                                        const Error err = _volume.statOf(_path.view(), a, true);
                                        if (err.isError()) return makeError<size_t>(err);
                                        _position = a.size;
                                }
                                _readLength = 0; // what was read ahead may be stale now
                                if (_writeLength != 0 && _position != _writeStart + _writeLength) {
                                        const Error err = flushLocked();
                                        if (err.isError()) return makeError<size_t>(err);
                                }
                                if (_writeBuffer == nullptr)
                                        _writeBuffer =
                                                static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, _volume.maxData));
                                if (_writeBuffer == nullptr) return makeError<size_t>(Error::NoMem);
                                size_t done = 0;
                                while (done < data.size()) {
                                        if (_writeLength == 0) _writeStart = _position;
                                        size_t n = _volume.maxData - _writeLength;
                                        if (n > data.size() - done) n = data.size() - done;
                                        std::memcpy(_writeBuffer + _writeLength, data.data() + done, n);
                                        _writeLength += n;
                                        _position += n;
                                        done += n;
                                        if (_writeLength == _volume.maxData) {
                                                const Error err = flushLocked();
                                                if (err.isError()) return Result<size_t>(done, err);
                                        }
                                }
                                return makeResult(done);
                        }

                        using Stream::write;

                        Error flush(unsigned int) override {
                                Mutex::Locker l(_lock, WaitForever);
                                return flushLocked();
                        }

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker l(_lock, WaitForever);
                                const Error   err = flushLocked();
                                if (err.isError()) return makeError<uint64_t>(err);
                                int64_t base = 0;
                                if (from == SeekFrom::Current) base = int64_t(_position);
                                if (from == SeekFrom::End) {
                                        Attr        a;
                                        const Error serr = _volume.statOf(_path.view(), a, true);
                                        if (serr.isError()) return makeError<uint64_t>(serr);
                                        base = int64_t(a.size);
                                }
                                if (offset < -base) return makeError<uint64_t>(Error::Invalid);
                                _position = uint64_t(base + offset);
                                return makeResult(_position);
                        }

                        Error truncate(uint64_t size) override {
                                Mutex::Locker l(_lock, WaitForever);
                                if (!_writable) return Error::BadDescriptor;
                                Error err = flushLocked();
                                if (err.isError()) return err;
                                Request r;
                                r.op = Truncate;
                                r.handle = _handle;
                                r.offset = size;
                                Reply  reply;
                                size_t got = 0;
                                err = _volume.call(r, StringView(), StringView(), ByteSpan(), WritableByteSpan(), reply,
                                                   got);
                                _readLength = 0;
                                _volume.forget(_path.view());
                                return err;
                        }

                        void handleClosed(uint8_t) override {
                                {
                                        Mutex::Locker l(_lock, WaitForever);
                                        (void)flushLocked();
                                        Request r;
                                        r.op = Close;
                                        r.handle = _handle;
                                        Reply  reply;
                                        size_t got = 0;
                                        (void)_volume.call(r, StringView(), StringView(), ByteSpan(),
                                                           WritableByteSpan(), reply, got);
                                }
                                if (_writable) {
                                        _volume.forget(_path.view());
                                        Mutex::Locker lock(Fs::lock(), WaitForever);
                                        Fs::writtenLocked(_identity);
                                }
                                Memory::free(_readBuffer);
                                Memory::free(_writeBuffer);
                                ServedVolume &v = _volume;
                                this->~ServedFile();
                                Memory::free(this);
                                v.release();
                                return;
                        }

                private:
                        // What is gathered, written. Its lock held.
                        Error flushLocked() {
                                size_t at = 0;
                                while (at < _writeLength) {
                                        Request r;
                                        r.op = Write;
                                        r.handle = _handle;
                                        r.offset = _writeStart + at;
                                        Reply       reply;
                                        size_t      got = 0;
                                        const Error err = _volume.call(r, StringView(), StringView(),
                                                                       ByteSpan(_writeBuffer + at, _writeLength - at),
                                                                       WritableByteSpan(), reply, got);
                                        if (err.isError()) {
                                                _writeLength = 0; // lost: told now, not again
                                                return err;
                                        }
                                        if (reply.count == 0) {
                                                _writeLength = 0;
                                                return Error::IoError;
                                        }
                                        at += reply.count;
                                }
                                _writeLength = 0;
                                return Error::Ok;
                        }

                        ServedVolume      &_volume;
                        const uint64_t     _handle;
                        const FileIdentity _identity;
                        String             _path{MemSpace::Psram};
                        Mutex              _lock;
                        uint64_t           _position = 0;
                        uint8_t           *_readBuffer = nullptr;
                        uint64_t           _readStart = 0;
                        size_t             _readLength = 0;
                        uint8_t           *_writeBuffer = nullptr;
                        uint64_t           _writeStart = 0;
                        size_t             _writeLength = 0;
                        const bool         _readable;
                        const bool         _writable;
                        const bool         _append;
        };

        // A file read plainly (Volume::Reader: the zone database, a link file).
        class ServedReader final : public Volume::Reader {
                public:
                        ServedReader(ServedVolume &owner, uint64_t opened, uint64_t size)
                            : volume(owner), handle(opened), bytes(size) {}

                        uint64_t size() const override { return bytes; }

                        Result<size_t> readAt(uint64_t offset, WritableByteSpan buf) override {
                                size_t done = 0;
                                while (done < buf.size()) {
                                        Request r;
                                        r.op = Read;
                                        r.handle = handle;
                                        r.offset = offset + done;
                                        const size_t want =
                                                buf.size() - done < volume.maxData ? buf.size() - done : volume.maxData;
                                        r.length = uint32_t(want);
                                        Reply       reply;
                                        size_t      got = 0;
                                        const Error err =
                                                volume.call(r, StringView(), StringView(), ByteSpan(),
                                                            WritableByteSpan(buf.data() + done, want), reply, got);
                                        if (err.isError()) return done != 0 ? makeResult(done) : makeError<size_t>(err);
                                        if (got == 0) break;
                                        done += got;
                                }
                                return makeResult(done);
                        }

                        ServedVolume  &volume;
                        const uint64_t handle;
                        const uint64_t bytes;
        };

        // A directory read READDIR by READDIR.
        class ServedCursor final : public Volume::Cursor {
                public:
                        explicit ServedCursor(ServedVolume &owner) : volume(owner) {}

                        Error next(FileInfo &info) override {
                                for (;;) {
                                        if (at < length) {
                                                Entry e;
                                                std::memcpy(&e, buffer + at, sizeof(e));
                                                if (e.size < sizeof(Entry) || e.size > length - at ||
                                                    e.nameLength > e.size - sizeof(Entry))
                                                        return Error::IoError;
                                                const StringView name(
                                                        reinterpret_cast<const char *>(buffer + at + sizeof(Entry)),
                                                        e.nameLength);
                                                at += e.size;
                                                ++index;
                                                if (name == "." || name == "..") continue;
                                                info = FileInfo();
                                                (void)info.name.assign(name);
                                                info.type = typeOf(e.type);
                                                info.size = info.type == FileType::Directory ? 0 : e.bytes;
                                                info.modified = Timestamp{e.modifiedNs};
                                                return Error::Ok;
                                        }
                                        if (ended) return Error::EndOfFile;
                                        Request r;
                                        r.op = ReadDir;
                                        r.offset = index;
                                        r.length = DirBuffer;
                                        Reply       reply;
                                        size_t      got = 0;
                                        const Error err = volume.call(r, path.view(), StringView(), ByteSpan(),
                                                                      WritableByteSpan(buffer, DirBuffer), reply, got);
                                        if (err.isError()) return err;
                                        at = 0;
                                        length = got;
                                        if (reply.count == 0 || got == 0) ended = true;
                                }
                        }

                        ServedVolume &volume;
                        String        path{MemSpace::Psram};
                        uint8_t       buffer[DirBuffer];
                        size_t        at = 0;
                        size_t        length = 0;
                        uint64_t      index = 0; // entries given so far, the next READDIR's offset
                        bool          ended = false;
        };

        // ---- The volume's calls

        Error ServedVolume::stat(StringView path, FileInfo &info, bool &noPath) {
                noPath = false;
                Attr        a;
                const Error err = statOf(path, a, false);
                if (err.isError()) return err;
                info = FileInfo();
                (void)info.name.assign(lastComponent(path));
                info.type = a.type;
                info.size = a.type == FileType::Directory ? 0 : a.size; // a link's: its target's length
                info.modified = Timestamp{a.modifiedNs};
                return Error::Ok;
        }

        Result<FileHandle::Ptr> ServedVolume::open(StringView path, uint8_t flags, const FileIdentity &identity,
                                                   bool &noPath, String *link) {
                noPath = false;
                Request r;
                r.op = Open;
                r.flags = ((flags & Vfs::Read) ? OpenRead : 0) | ((flags & Vfs::Write) ? OpenWrite : 0) |
                          ((flags & Vfs::Create) ? OpenCreate : 0) | ((flags & Vfs::Exclusive) ? OpenExclusive : 0) |
                          ((flags & Vfs::Truncate) ? OpenTruncate : 0);
                Reply  reply;
                size_t got = 0;
                Error  err = call(r, path, StringView(), ByteSpan(), WritableByteSpan(), reply, got);
                if ((flags & (Vfs::Write | Vfs::Create | Vfs::Truncate)) != 0) forget(path);
                // A link (the server opens none: ELOOP): its target, for the VFS to follow.
                if (err == Error::TooManyLinks && link != nullptr && readLink(path, *link).isOk())
                        return makeError<FileHandle::Ptr>(Error::Invalid);
                if (err.isError()) return makeError<FileHandle::Ptr>(err);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ServedFile));
                if (mem == nullptr) {
                        Request c;
                        c.op = Close;
                        c.handle = reply.handle;
                        (void)call(c, StringView(), StringView(), ByteSpan(), WritableByteSpan(), reply, got);
                        return makeError<FileHandle::Ptr>(Error::NoMem);
                }
                auto *file = ::new (mem) ServedFile(*this, reply.handle, flags, identity);
                err = file->setPath(path);
                const uint8_t mode =
                        ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
                Result<FileHandle::Ptr> handle =
                        err.isOk() ? FileHandle::open(*file, mode) : makeError<FileHandle::Ptr>(err);
                if (handle.error().isError()) file->handleClosed(0); // closes it, frees it
                return handle;
        }

        Result<Volume::Reader *> ServedVolume::openReader(StringView path) {
                Attr  a;
                Error err = statOf(path, a, false);
                if (err.isError()) return makeError<Reader *>(err);
                if (a.type == FileType::Directory) return makeError<Reader *>(Error::IsDir);
                Request r;
                r.op = Open;
                r.flags = OpenRead;
                Reply  reply;
                size_t got = 0;
                err = call(r, path, StringView(), ByteSpan(), WritableByteSpan(), reply, got);
                if (err.isError()) return makeError<Reader *>(err);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ServedReader));
                if (mem == nullptr) return makeError<Reader *>(Error::NoMem);
                retain();
                return makeResult<Reader *>(::new (mem) ServedReader(*this, reply.handle, a.size));
        }

        void ServedVolume::closeReader(Reader *reader) {
                auto   *r = static_cast<ServedReader *>(reader);
                Request c;
                c.op = Close;
                c.handle = r->handle;
                Reply  reply;
                size_t got = 0;
                (void)call(c, StringView(), StringView(), ByteSpan(), WritableByteSpan(), reply, got);
                r->~ServedReader();
                Memory::free(r);
                release();
                return;
        }

        Result<Volume::Cursor *> ServedVolume::openDirectory(StringView path, bool &noPath) {
                noPath = false;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ServedCursor));
                if (mem == nullptr) return makeError<Cursor *>(Error::NoMem);
                auto *c = ::new (mem) ServedCursor(*this);
                Error err = c->path.assign(path.isEmpty() ? StringView("/") : path);
                // The first READDIR now: a directory that isn't one fails here, not at the first entry.
                if (err.isOk()) {
                        Request r;
                        r.op = ReadDir;
                        r.offset = 0;
                        r.length = DirBuffer;
                        Reply  reply;
                        size_t got = 0;
                        err = call(r, c->path.view(), StringView(), ByteSpan(), WritableByteSpan(c->buffer, DirBuffer),
                                   reply, got);
                        c->length = got;
                        c->ended = reply.count == 0 || got == 0;
                }
                if (err.isError()) {
                        c->~ServedCursor();
                        Memory::free(mem);
                        return makeError<Cursor *>(err);
                }
                return makeResult<Cursor *>(c);
        }

        void ServedVolume::closeDirectory(Cursor *cursor) {
                auto *c = static_cast<ServedCursor *>(cursor);
                c->~ServedCursor();
                Memory::free(c);
                return;
        }

} // namespace

Result<FileHandle::Ptr> ServedFs::serve(const Config &config) {
        if (config.maxData < MinData || config.maxData > MaxData || config.type.isEmpty() ||
            config.type.size() >= MaxType || config.errorOf == nullptr)
                return makeError<FileHandle::Ptr>(Error::Invalid);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(ServedVolume));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *v = ::new (mem) ServedVolume(config);
        // The mount takes one reference (released if refused), the server's end the other.
        Error err = Fs::mountVolume(config.path, *v);
        if (err.isError()) {
                v->release();
                return makeError<FileHandle::Ptr>(err);
        }
        Result<FileHandle::Ptr> handle = FileHandle::open(v->server, FileHandle::Read | FileHandle::Write);
        if (handle.error().isError()) {
                v->end();
                Fs::unmountVolume(*v);
                v->release();
        }
        return handle;
}

BMDS_NAMESPACE_END
