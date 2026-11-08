/**
 * @file      midifile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /dev/midiN, /dev/midi, /dev/midinew: a program's end of a MIDI port
 * (devplan/audio.md §21).
 */

#include <climits>
#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midifile.h>
#include <bmds/staticobject.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>
#include <bmds/waketimer.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The ring follows the MidiFile in the same allocation.
        constexpr size_t HeaderBytes = (sizeof(MidiFile) + 15) & ~static_cast<size_t>(15);

        static_assert((MidiFile::RingEvents & (MidiFile::RingEvents - 1)) == 0, "the ring: a power of two");

        constexpr uint8_t SystemPort = 0xfe; // /dev/midi: `system`'s
        constexpr uint8_t NewPort = MidiRouter::NoPort;

        // midi0 to midi15's names and numbers, then /dev/midi's and /dev/midinew's.
        struct FileArg {
                        const char *name;
                        uint8_t     port;
        };

        constexpr FileArg Files[] = {
                {"midi0", 0},   {"midi1", 1},         {"midi2", 2},         {"midi3", 3},   {"midi4", 4},
                {"midi5", 5},   {"midi6", 6},         {"midi7", 7},         {"midi8", 8},   {"midi9", 9},
                {"midi10", 10}, {"midi11", 11},       {"midi12", 12},       {"midi13", 13}, {"midi14", 14},
                {"midi15", 15}, {"midi", SystemPort}, {"midinew", NewPort},
        };
        static_assert(sizeof(Files) / sizeof(Files[0]) == MidiRouter::MaxPorts + 2, "a file for every port");

        // The MIDI clock (scheduled output, devplan/audio.md §25): the fds with a queue, and the
        // thread that sends their events at their times. Its lock covers the queues, both ends.
        constexpr size_t MaxScheduling = 16;                   // fds with a queue at once
        constexpr size_t RecordRoom = 2 * MidiEvent::MaxBytes; // a record's events at most

        struct MidiClock {
                        Mutex     lock;
                        MidiFile *files[MaxScheduling] = {};
                        Interrupt wake; // a new event: the thread looks again
                        Thread    thread;
                        bool      started = false; // (lock)
        };

        constinit StaticObject<MidiClock> midiClock;

        // Under the VFS's lock: the router only, never the settings.
        bool portExists(void *arg) {
                const auto *fa = static_cast<const FileArg *>(arg);
                return Midi::router().port(fa->port).error().isOk();
        }

} // namespace

// ---- Opening

Error MidiFile::addFiles() {
        midiClock.create();
        for (const FileArg &a : Files) {
                Vfs::DeviceFile file;
                file.name = a.name;
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        const auto *fa = static_cast<const FileArg *>(arg);
                        Midi::applySettings(); // called without the VFS's lock
                        uint8_t port = fa->port;
                        if (port == SystemPort) port = Midi::systemPort();
                        return open(port, flags);
                };
                file.arg = const_cast<FileArg *>(&a);
                file.isDevice = true;
                if (a.port < MidiRouter::MaxPorts) file.present = portExists;
                const Error err = Vfs::addDeviceFile(file);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

Result<FileHandle::Ptr> MidiFile::open(uint8_t port, uint8_t flags) {
        if ((flags & (Vfs::Read | Vfs::Write)) == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
        const uint8_t mode =
                ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
        MidiRouter &router = Midi::router();
        bool        asSource = false;
        if (port != NewPort) {
                auto [info, err] = router.port(port);
                if (err.isError()) return makeError<FileHandle::Ptr>(Error::NotExist);
                // A system port with no destination (`system`) takes writes as its source's.
                asSource = info.kind == MidiRouter::Kind::System && (info.sides & MidiRouter::Destination) == 0;
                if (((flags & Vfs::Read) != 0 && (info.sides & MidiRouter::Source) == 0) ||
                    ((flags & Vfs::Write) != 0 && (info.sides & MidiRouter::Destination) == 0 && !asSource))
                        return makeError<FileHandle::Ptr>(Error::NotSupported);
        }
        // A ring for what it reads: an fd on a port read, or a new port (its destination's).
        const bool   ring = port == NewPort || (flags & Vfs::Read) != 0;
        const size_t bytes = HeaderBytes + (ring ? RingEvents * sizeof(MidiEvent) : 0);
        auto        *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes));
        if (block == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        Span<MidiEvent> events(ring ? reinterpret_cast<MidiEvent *>(block + HeaderBytes) : nullptr,
                               ring ? RingEvents : 0);
        auto *file = ::new (block) MidiFile(port, events);
        file->_asSource = asSource;
        if (port != NewPort) {
                // On the port: told when it goes, and given its source's events if read.
                const Error err = router.attach(port, *file, (flags & Vfs::Read) != 0);
                if (err.isError()) {
                        file->_port = NewPort; // nothing to let go of
                        file->handleClosed(0);
                        return makeError<FileHandle::Ptr>(err == Error::Full ? Error::Busy : err);
                }
        }
        Result<FileHandle::Ptr> handle = FileHandle::open(*file, mode);
        if (handle.error().isError()) file->handleClosed(0); // frees it
        return handle;
}

MidiFile::MidiFile(uint8_t port, Span<MidiEvent> ring) : _port(port) {
        (void)_in.setStorage(ring);
}

void MidiFile::handleClosed(uint8_t) {
        if (_scheduledStorage != nullptr) {
                // Out of the clock's hands first: what it scheduled and didn't send goes.
                Mutex::Locker lock(midiClock->lock, WaitForever);
                for (MidiFile *&f : midiClock->files)
                        if (f == this) f = nullptr;
                (void)_scheduled.setStorage(Span<MidiEvent>());
                Memory::free(_scheduledStorage);
                _scheduledStorage = nullptr;
        }
        releaseHeld();
        if (hasPort() && _allowedNs.load(std::memory_order_relaxed) != 0)
                Midi::router().allowLatency(_port, _owner || _asSource, this, 0);
        if (hasPort()) {
                // After these, the router calls it no more.
                if (_owner) {
                        Midi::router().removePort(_port);
                } else {
                        Midi::router().detach(*this);
                }
        }
        this->~MidiFile();
        Memory::free(this);
        return;
}

// ---- Configuring

Error MidiFile::configure(const Config &c, Config &got) {
        const Error err = applyConfig(c);
        if (err.isError()) return err;
        got = config();
        return Error::Ok;
}

Error MidiFile::applyConfig(const Config &c) {
        Mutex::Locker lock(_configLock, WaitForever);
        if ((c.flags & ~(Events | Create | Source | Destination)) != 0) return Error::Invalid;
        if ((c.flags & Events) != 0 && (c.eventSize < MinEventSize || c.eventSize > MaxEventSize))
                return Error::Invalid;
        const bool create = (c.flags & Create) != 0;
        if (!create && (c.flags & (Source | Destination)) != 0) return Error::Invalid;
        if (create) {
                // Only /dev/midinew, once, with a side and a name.
                const size_t nameLength = strnlen(c.name, sizeof(c.name));
                if (hasPort() || (c.flags & (Source | Destination)) == 0 || nameLength == 0 ||
                    nameLength == sizeof(c.name))
                        return Error::Invalid;
                const uint8_t sides = ((c.flags & Source) != 0 ? MidiRouter::Source : 0) |
                                      ((c.flags & Destination) != 0 ? MidiRouter::Destination : 0);
                Midi::applySettings();
                auto [n, err] = Midi::router().addPort(MidiRouter::Kind::Virtual, sides, StringView(c.name),
                                                       (sides & MidiRouter::Destination) != 0 ? this : nullptr);
                if (err.isError()) return err;
                _port = n;
                _owner = true;
                _sides = sides;
        }
        // Between reads (it waits for one under way): bytes left over are dropped by the switch.
        {
                Mutex::Locker readLock(_readLock, WaitForever);
                _flags = c.flags & Events;
                _eventSize = (c.flags & Events) != 0 ? c.eventSize : MinEventSize;
                _pendingSize = 0;
                _pendingAt = 0;
        }
        return Error::Ok;
}

MidiFile::Config MidiFile::config() {
        Mutex::Locker lock(_configLock, WaitForever);
        Config        c;
        c.flags = _flags | (_owner ? Create : 0) | ((_sides & MidiRouter::Source) != 0 ? Source : 0) |
                  ((_sides & MidiRouter::Destination) != 0 ? Destination : 0);
        c.eventSize = (_flags & Events) != 0 ? _eventSize : 0;
        c.port = _port;
        if (hasPort()) {
                auto [info, err] = Midi::router().port(_port);
                if (err.isOk()) std::memcpy(c.name, info.name, sizeof(c.name));
        }
        return c;
}

MidiFile::Status MidiFile::status() {
        Status s;
        if (hasPort()) {
                auto [info, err] = Midi::router().port(_port);
                if (err.isOk()) s.port = info;
        }
        s.port.number = _port;
        s.gone = _gone.load(std::memory_order_acquire);
        {
                Mutex::Locker lock(_readLock, WaitForever);
                s.eventsRead = _eventsRead;
        }
        s.dropped = _dropped.load(std::memory_order_relaxed);
        return s;
}

Error MidiFile::connect(uint8_t destination) {
        if (!hasPort()) return Error::Invalid;
        const Error err = Midi::router().connect(_port, destination);
        if (err.isOk() && _allowedNs.load(std::memory_order_relaxed) != 0) (void)setAllowedLatency(_allowedNs.load());
        return err;
}

Error MidiFile::disconnect(uint8_t destination) {
        if (!hasPort()) return Error::Invalid;
        const Error err = Midi::router().disconnect(_port, destination);
        if (err.isOk() && _allowedNs.load(std::memory_order_relaxed) != 0) (void)setAllowedLatency(_allowedNs.load());
        return err;
}

Error MidiFile::setAllowedLatency(int64_t ns) {
        if (!hasPort() || ns < 0) return Error::Invalid;
        _allowedNs.store(ns, std::memory_order_relaxed);
        Midi::router().allowLatency(_port, _owner || _asSource, this, ns);
        return Error::Ok;
}

void MidiFile::cancelScheduled() {
        {
                Mutex::Locker lock(midiClock->lock, WaitForever);
                _scheduled.clear();
        }
        // What was sent ahead to a synth rendering ahead for it is dropped there too (§27).
        if (_allowedNs.load(std::memory_order_relaxed) != 0) Midi::router().cancel(this);
        return;
}

void MidiFile::dropInput() {
        Mutex::Locker lock(_readLock, WaitForever);
        _in.clear();
        _pendingSize = 0;
        _pendingAt = 0;
        return;
}

// ---- The router's side

MidiSink::Delivery MidiFile::deliver(const MidiEvent &event) {
        if (!_in.push(event)) {
                _dropped.fetch_add(1, std::memory_order_relaxed);
                return Delivery::Dropped;
        }
        _readable.release();
        _pollers.notify();
        return Delivery::Taken;
}

void MidiFile::portGone() {
        _gone.store(true, std::memory_order_release);
        _readable.release();
        _pollers.notify();
        return;
}

// ---- Reading

Result<size_t> MidiFile::readEvents(WritableByteSpan buf) {
        size_t    done = 0;
        MidiEvent e;
        while (buf.size() - done >= _eventSize && _in.pop(e)) {
                std::memcpy(buf.data() + done, &e, sizeof(e));
                std::memset(buf.data() + done + sizeof(e), 0, _eventSize - sizeof(e)); // fields to come
                done += _eventSize;
                ++_eventsRead;
        }
        return makeResult(done);
}

Result<size_t> MidiFile::readBytes(WritableByteSpan buf) {
        size_t done = 0;
        while (done < buf.size()) {
                if (_pendingAt == _pendingSize) {
                        MidiEvent e;
                        if (!_in.pop(e)) break;
                        std::memcpy(_pending, e.data, e.size);
                        _pendingSize = e.size;
                        _pendingAt = 0;
                        ++_eventsRead;
                }
                while (done < buf.size() && _pendingAt < _pendingSize) buf[done++] = _pending[_pendingAt++];
        }
        return makeResult(done);
}

Result<size_t> MidiFile::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        // /dev/midinew before Create, or a port it made without a destination: nothing to read.
        if (!hasPort() || (_owner && (_sides & MidiRouter::Destination) == 0)) return makeError<size_t>(Error::Invalid);
        Deadline deadline(timeoutMs);
        if (_readLock.lock(deadline).isError()) return makeError<size_t>(Error::Timeout);
        Result<size_t> result;
        for (;;) {
                if ((_flags & Events) != 0 && buf.size() < _eventSize) {
                        result = makeError<size_t>(Error::Invalid);
                        break;
                }
                result = (_flags & Events) != 0 ? readEvents(buf) : readBytes(buf);
                if (result.value() > 0) break;
                if (_gone.load(std::memory_order_acquire) && _in.isEmpty()) {
                        result = makeError<size_t>(Error::EndOfFile);
                        break;
                }
                const Error err = Interrupt::waitCurrent(_readable, deadline);
                if (err.isError()) {
                        result = makeError<size_t>(err);
                        break;
                }
        }
        _readLock.unlock();
        return result;
}

// ---- Writing

// One byte through the parser; what it completes, to the port.
Error MidiFile::put(uint8_t byte) {
        MidiEvent    events[MidiParser::MaxEvents];
        const size_t n = _parser.feed(byte, events);
        for (size_t i = 0; i < n; ++i) {
                MidiEvent &e = events[i];
                e.timeNs = Clock::monotonicNs();
                if (emit(e) == Error::NotExist) return Error::BrokenPipe;
                // WouldBlock: a full destination dropped it, counted there; writes never wait.
        }
        return Error::Ok;
}

// An event this fd sends, to its port as its writes go; the notes it holds on kept (closing lets
// them go: a program that ends or crashes mid-note leaves none sounding on a shared synth).
Error MidiFile::emit(MidiEvent &e) {
        noteHeld(e);
        if (_owner || _asSource) return Midi::router().send(_port, e);
        e.port = MidiRouter::NoPort; // from no port: a write to the port's file
        return Midi::router().deliver(_port, e);
}

int64_t MidiFile::latencyNs() const {
        return Midi::router().latencyNs(_port, _owner || _asSource);
}

void MidiFile::noteHeld(const MidiEvent &e) {
        if (e.size < 3) return;
        const uint8_t    kind = e.data[0] & 0xf0;
        const uint8_t    channel = e.data[0] & 0x0f;
        const uint8_t    note = e.data[1] & 0x7f;
        SpinLock::Locker lock(_heldLock);
        if (kind == 0x90 && e.data[2] != 0) {
                _held[channel][note >> 3] = uint8_t(_held[channel][note >> 3] | (1u << (note & 7)));
        } else if (kind == 0x80 || kind == 0x90) {
                _held[channel][note >> 3] = uint8_t(_held[channel][note >> 3] & ~(1u << (note & 7)));
        } else if (kind == 0xb0 && (e.data[1] == 120 || e.data[1] == 123)) {
                std::memset(_held[channel], 0, sizeof(_held[channel])); // all sound off, all notes off
        }
        return;
}

// Closing: a note-off for each note still held.
void MidiFile::releaseHeld() {
        if (!hasPort() || _gone.load(std::memory_order_acquire)) return;
        for (uint8_t c = 0; c < 16; ++c) {
                for (uint8_t note = 0; note < 128; ++note) {
                        bool held;
                        {
                                SpinLock::Locker lock(_heldLock);
                                held = (_held[c][note >> 3] & (1u << (note & 7))) != 0;
                        }
                        if (!held) continue;
                        MidiEvent off;
                        off.timeNs = Clock::monotonicNs();
                        off.size = 3;
                        off.data[0] = uint8_t(0x80 | c);
                        off.data[1] = note;
                        off.data[2] = 0;
                        (void)emit(off);
                }
        }
        return;
}

Result<size_t> MidiFile::write(ByteSpan data, unsigned int timeoutMs) {
        if (data.isEmpty()) return makeResult(size_t(0));
        if (!hasPort() || (_owner && (_sides & MidiRouter::Source) == 0)) return Result<size_t>(0, Error::Invalid);
        Deadline deadline(timeoutMs);
        if (_writeLock.lock(deadline).isError()) return Result<size_t>(0, Error::Timeout);
        size_t       done = 0;
        Error        err;
        const bool   events = (_flags & Events) != 0;
        const size_t step = events ? _eventSize : 1;
        if (events && data.size() % step != 0) err = Error::Invalid;
        while (err.isOk() && done < data.size()) {
                if (_gone.load(std::memory_order_acquire)) {
                        err = Error::BrokenPipe;
                        break;
                }
                if (events) {
                        // A record: 1 to 5 bytes, its time 0 (now) or when to send it.
                        MidiEvent e;
                        std::memcpy(&e, data.data() + done, sizeof(e));
                        if (e.size == 0 || e.size > MidiEvent::MaxBytes) {
                                err = Error::Invalid;
                                break;
                        }
                        if (e.timeNs != 0) {
                                const int64_t now = Clock::monotonicNs();
                                if (e.timeNs > now + MaxAheadNs) {
                                        err = Error::Invalid;
                                        break;
                                }
                                if (e.timeNs <= now) e.timeNs = 0; // passed: now
                        }
                        if (e.timeNs != 0) {
                                err = schedule(e);
                                if (err.isError()) break;
                        } else {
                                for (uint8_t i = 0; i < e.size && err.isOk(); ++i) err = put(e.data[i]);
                        }
                } else {
                        err = put(data[done]);
                }
                if (err.isOk()) done += step;
        }
        Midi::router().noteParseErrors(_port, _parser.takeErrors());
        _writeLock.unlock();
        return Result<size_t>(done, err);
}

// ---- Scheduled output (devplan/audio.md §25)

bool MidiFile::hasRoomToSchedule() const {
        return _scheduledStorage == nullptr || _scheduled.capacity() - _scheduled.size() >= RecordRoom;
}

// A record for later: its events (the writer's parser) queued with its time, for the clock.
// The writer's lock is held.
Error MidiFile::schedule(const MidiEvent &record) {
        MidiClock    &c = *midiClock;
        Mutex::Locker lock(c.lock, WaitForever);
        if (_scheduledStorage == nullptr) {
                size_t slot = MaxScheduling;
                for (size_t i = 0; i < MaxScheduling && slot == MaxScheduling; ++i)
                        if (c.files[i] == nullptr) slot = i;
                if (slot == MaxScheduling) return Error::Busy;
                _scheduledStorage =
                        static_cast<MidiEvent *>(Memory::alloc(MemSpace::Psram, ScheduledEvents * sizeof(MidiEvent)));
                if (_scheduledStorage == nullptr) return Error::NoMem;
                (void)_scheduled.setStorage(Span<MidiEvent>(_scheduledStorage, ScheduledEvents));
                c.files[slot] = this;
                if (!c.started) {
                        (void)WakeTimer::init(); // sleeps that end at their time, not on the tick
                        Thread::Config tc;
                        tc.name = "midiclock";
                        tc.stackBytes = 4096;
                        tc.stackSpace = MemSpace::Psram;
                        tc.priority = 18; // under the audio clients (19): a note is sent before its period
                        tc.kernel = true;
                        const Error err = c.thread.start(tc, clockMain, nullptr);
                        if (err.isError()) {
                                bmdsErr("midi: no clock thread: %s", err.desc());
                                return err;
                        }
                        c.started = true; // for good: never joined
                }
        }
        if (!_scheduled.isEmpty() && record.timeNs < _lastScheduledNs) return Error::Invalid; // in order
        if (!hasRoomToSchedule()) return Error::WouldBlock;
        for (uint8_t i = 0; i < record.size; ++i) {
                MidiEvent    events[MidiParser::MaxEvents];
                const size_t n = _parser.feed(record.data[i], events);
                for (size_t k = 0; k < n; ++k) {
                        events[k].timeNs = record.timeNs;
                        (void)_scheduled.push(events[k]); // room kept for a record's worth
                }
        }
        _lastScheduledNs = record.timeNs;
        c.wake.raise();
        return Error::Ok;
}

// The clock's lock is held: @p event, due, to the port as a write now would go, with its time.
void MidiFile::sendScheduled(const MidiEvent &event) {
        if (_gone.load(std::memory_order_acquire) || !hasPort()) return;
        MidiEvent e = event;
        (void)emit(e);
        return;
}

// The clock's thread: each fd's events due, sent; then asleep until the next is (the wake timer),
// or a new one comes.
void MidiFile::clockMain(void *) {
        MidiClock &c = *midiClock;
        for (;;) {
                c.wake.clear();
                int64_t next = INT64_MAX;
                {
                        Mutex::Locker lock(c.lock, WaitForever);
                        const int64_t now = Clock::monotonicNs();
                        for (MidiFile *f : c.files) {
                                if (f == nullptr) continue;
                                const bool       full = !f->hasRoomToSchedule();
                                const MidiEvent *e = f->_scheduled.peek();
                                while (e != nullptr && e->timeNs <= now) {
                                        MidiEvent due;
                                        (void)f->_scheduled.pop(due);
                                        f->sendScheduled(due);
                                        e = f->_scheduled.peek();
                                }
                                if (e != nullptr && e->timeNs < next) next = e->timeNs;
                                if (full && f->hasRoomToSchedule()) f->_pollers.notify(); // writable again
                        }
                }
                const int64_t until = next != INT64_MAX ? next : Clock::monotonicNs() + 3'600'000'000'000;
                (void)WakeTimer::sleepUntil(until, &c.wake);
        }
}

uint8_t MidiFile::readiness() {
        if (_gone.load(std::memory_order_acquire)) return Readable | HungUp | Writable | Broken;
        uint8_t ready = hasRoomToSchedule() ? Writable : 0;
        if (!hasPort() || !_in.isEmpty() || _pendingAt < _pendingSize) ready |= Readable;
        return ready;
}

BMDS_NAMESPACE_END
