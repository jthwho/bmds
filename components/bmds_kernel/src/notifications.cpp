/**
 * @file      notifications.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's notifications (devplan/notifications.md §3.2): the log behind a lock, the server
 * told of changes, the senders' listeners told of events, and programs' events fds.
 */

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/notifications.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

namespace Notifications {

        namespace {

                struct Registry {
                                struct Entry {
                                                uint32_t  id = 0;
                                                Listener *listener = nullptr;
                                };

                                Mutex           lock;
                                NotificationLog log{MemSpace::Psram};
                                Changed         server = nullptr;
                                void           *serverArg = nullptr;
                                Entry           listeners[MaxListeners];
                                uint32_t        lastListener = 0;

                                // The server told (the lock held: it only posts to its loop).
                                void changed() {
                                        if (server != nullptr) server(serverArg);
                                        return;
                                }

                                // @p listener's (an id) told of @p event (the lock held).
                                void tell(uint32_t listener, uint32_t id, Event event, uint32_t action) {
                                        if (listener == 0) return;
                                        for (const Entry &e : listeners)
                                                if (e.id == listener && e.listener != nullptr)
                                                        e.listener->tell(id, event, action);
                                        return;
                                }
                };

                constinit StaticObject<Registry> registry;

                // An events fd (notify_open): the events of the notifications posted with it, as
                // EventRecords, a few queued (the oldest dropped when full).
                class EventStream final : public Stream {
                        public:
                                Error start() { return _listener.start(); }

                                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override {
                                        if (buf.size() < sizeof(EventRecord) || buf.size() % sizeof(EventRecord) != 0)
                                                return makeError<size_t>(Error::Invalid);
                                        Deadline deadline(timeoutMs);
                                        for (;;) {
                                                const size_t n = take(buf);
                                                if (n > 0) return makeResult(n);
                                                const Error err = Interrupt::waitCurrent(_ready, deadline);
                                                if (err.isError()) return makeError<size_t>(err);
                                        }
                                }

                                Result<size_t> write(ByteSpan, unsigned int) override {
                                        return makeError<size_t>(Error::NotSupported);
                                }

                                uint8_t readiness() override {
                                        Mutex::Locker locker(_queueLock, WaitForever);
                                        return _count > 0 ? Readable : 0;
                                }

                                WaitList *readinessWaiters() override { return &_pollers; }

                                Listener *notificationListener() override { return &_listener; }

                                // Its only handle went: the events stop, the notifications stay.
                                void handleClosed(uint8_t) override {
                                        _listener.stop();
                                        this->~EventStream();
                                        Memory::free(this);
                                        return;
                                }

                        private:
                                static constexpr size_t QueueRecords = 16;

                                size_t take(WritableByteSpan buf) {
                                        Mutex::Locker locker(_queueLock, WaitForever);
                                        size_t        n = 0;
                                        while (_count > 0 && n + sizeof(EventRecord) <= buf.size()) {
                                                std::memcpy(buf.data() + n, &_queue[_head], sizeof(EventRecord));
                                                _head = (_head + 1) % QueueRecords;
                                                --_count;
                                                n += sizeof(EventRecord);
                                        }
                                        return n;
                                }

                                // On the server's thread, the registry's lock held (must not block for long).
                                static void told(void *arg, uint32_t id, Event event, uint32_t action) {
                                        auto *self = static_cast<EventStream *>(arg);
                                        {
                                                Mutex::Locker locker(self->_queueLock, WaitForever);
                                                if (self->_count == QueueRecords) { // full: the oldest goes
                                                        self->_head = (self->_head + 1) % QueueRecords;
                                                        --self->_count;
                                                }
                                                EventRecord &r =
                                                        self->_queue[(self->_head + self->_count) % QueueRecords];
                                                r = EventRecord{sizeof(EventRecord), id, uint32_t(event), action};
                                                ++self->_count;
                                        }
                                        self->_ready.release();
                                        self->_pollers.notify();
                                        return;
                                }

                                Listener      _listener{told, this};
                                Mutex         _queueLock;
                                EventRecord   _queue[QueueRecords];
                                size_t        _head = 0;
                                size_t        _count = 0;
                                Semaphore     _ready{1, 0};
                                WaitListOf<4> _pollers;
                };

        } // namespace

        Error Listener::start() {
                if (!registry.isCreated()) return Error::InvalidState;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (_id != 0) return Error::Ok;
                for (Registry::Entry &e : r.listeners) {
                        if (e.listener != nullptr) continue;
                        if (++r.lastListener == 0) ++r.lastListener;
                        e = Registry::Entry{r.lastListener, this};
                        _id = r.lastListener;
                        return Error::Ok;
                }
                return Error::Full;
        }

        void Listener::stop() {
                if (_id == 0 || !registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever); // a function running ends first
                for (Registry::Entry &e : r.listeners)
                        if (e.listener == this) e = Registry::Entry{};
                r.log.dropListener(_id);
                _id = 0;
                return;
        }

        Error init() {
                if (!registry.isCreated()) registry.create();
                return Error::Ok;
        }

        Result<uint32_t> post(const Notification &n, int32_t owner, Listener *listener) {
                if (!registry.isCreated()) return makeError<uint32_t>(Error::InvalidState);
                Registry        &r = *registry;
                Mutex::Locker    locker(r.lock, WaitForever);
                const uint32_t   listenerId = listener != nullptr ? listener->id() : 0;
                Result<uint32_t> id = r.log.post(n, owner, listenerId, Clock::now().ns);
                if (id.isOk()) r.changed();
                return id;
        }

        Error close(uint32_t id, int32_t owner) {
                if (!registry.isCreated()) return Error::NotExist;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                const Error   err = r.log.close(id, owner);
                if (err.isOk()) r.changed();
                return err;
        }

        Error setServer(Changed fn, void *arg) {
                if (!registry.isCreated()) return Error::InvalidState;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (r.server != nullptr) return Error::Exists;
                r.server = fn;
                r.serverArg = arg;
                return Error::Ok;
        }

        void clearServer(void *arg) {
                if (!registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (r.serverArg != arg) return;
                r.server = nullptr;
                r.serverArg = nullptr;
                return;
        }

        bool hasServer() {
                if (!registry.isCreated()) return false;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                return r.server != nullptr;
        }

        uint64_t lastChange() {
                if (!registry.isCreated()) return 0;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                return r.log.lastChange();
        }

        size_t unread() {
                if (!registry.isCreated()) return 0;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                return r.log.unread();
        }

        size_t ids(Span<uint32_t> out) {
                if (!registry.isCreated()) return 0;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                return r.log.ids(out);
        }

        Error get(uint32_t id, Notification &out) {
                if (!registry.isCreated()) return Error::NotExist;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                return r.log.get(id, out);
        }

        void endBalloon(uint32_t id, NotificationLog::Ended how, uint32_t action) {
                if (!registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                Notification  n;
                if (r.log.get(id, n).isError() || !n.balloon) return;
                (void)r.log.endBalloon(id, how);
                const Event event = how == NotificationLog::Ended::Activated   ? Event::Activated
                                    : how == NotificationLog::Ended::Dismissed ? Event::Dismissed
                                                                               : Event::Expired;
                r.tell(n.listener, id, event, how == NotificationLog::Ended::Activated ? action : 0);
                r.changed();
                return;
        }

        void activate(uint32_t id, uint32_t action) {
                if (!registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                Notification  n;
                if (r.log.get(id, n).isError()) return;
                r.log.markRead(id);
                r.tell(n.listener, id, Event::Activated, action);
                r.changed();
                return;
        }

        void remove(uint32_t id) {
                if (!registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (r.log.close(id, -1).isOk()) r.changed();
                return;
        }

        void clear() {
                if (!registry.isCreated()) return;
                Registry      &r = *registry;
                Mutex::Locker  locker(r.lock, WaitForever);
                const uint64_t before = r.log.lastChange();
                r.log.clear();
                if (r.log.lastChange() != before) r.changed();
                return;
        }

        void markRead(uint32_t id) {
                if (!registry.isCreated()) return;
                Registry      &r = *registry;
                Mutex::Locker  locker(r.lock, WaitForever);
                const uint64_t before = r.log.lastChange();
                r.log.markRead(id);
                if (r.log.lastChange() != before) r.changed();
                return;
        }

        Result<FileHandle::Ptr> openEvents() {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(EventStream));
                if (memory == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                auto *stream = ::new (memory) EventStream();
                Error err = stream->start();
                if (err.isOk()) {
                        auto [handle, herr] = FileHandle::open(*stream, FileHandle::Read);
                        if (herr.isOk()) return makeResult(std::move(handle));
                        err = herr;
                }
                stream->~EventStream();
                Memory::free(stream);
                return makeError<FileHandle::Ptr>(err);
        }

        Listener *listenerOf(Stream &stream) {
                return stream.notificationListener();
        }

} // namespace Notifications

BMDS_NAMESPACE_END
