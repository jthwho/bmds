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

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midifile.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The ring follows the MidiFile in the same allocation.
        constexpr size_t HeaderBytes = (sizeof(MidiFile) + 15) & ~static_cast<size_t>(15);

        static_assert((MidiFile::RingEvents & (MidiFile::RingEvents - 1)) == 0, "the ring: a power of two");

        constexpr uint8_t DefaultPort = 0xfe; // /dev/midi: midi/default's
        constexpr uint8_t NewPort = MidiRouter::NoPort;

        // midi0 to midi15's names and numbers, then /dev/midi's and /dev/midinew's.
        struct FileArg {
                        const char *name;
                        uint8_t     port;
        };

        constexpr FileArg Files[] = {
                {"midi0", 0},   {"midi1", 1},          {"midi2", 2},         {"midi3", 3},   {"midi4", 4},
                {"midi5", 5},   {"midi6", 6},          {"midi7", 7},         {"midi8", 8},   {"midi9", 9},
                {"midi10", 10}, {"midi11", 11},        {"midi12", 12},       {"midi13", 13}, {"midi14", 14},
                {"midi15", 15}, {"midi", DefaultPort}, {"midinew", NewPort},
        };
        static_assert(sizeof(Files) / sizeof(Files[0]) == MidiRouter::MaxPorts + 2, "a file for every port");

        // Under the VFS's lock: the router only, never the settings.
        bool portExists(void *arg) {
                const auto *fa = static_cast<const FileArg *>(arg);
                return Midi::router().port(fa->port).error().isOk();
        }

} // namespace

// ---- Opening

Error MidiFile::addFiles() {
        for (const FileArg &a : Files) {
                Vfs::DeviceFile file;
                file.name = a.name;
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        const auto *fa = static_cast<const FileArg *>(arg);
                        Midi::applySettings(); // called without the VFS's lock
                        uint8_t port = fa->port;
                        if (port == DefaultPort) {
                                auto [n, err] = Midi::defaultPort();
                                if (err.isError()) return makeError<FileHandle::Ptr>(Error::NotExist);
                                port = n;
                        }
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
        if (port != NewPort) {
                auto [info, err] = router.port(port);
                if (err.isError()) return makeError<FileHandle::Ptr>(Error::NotExist);
                if (((flags & Vfs::Read) != 0 && (info.sides & MidiRouter::Source) == 0) ||
                    ((flags & Vfs::Write) != 0 && (info.sides & MidiRouter::Destination) == 0))
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
        return Midi::router().connect(_port, destination);
}

Error MidiFile::disconnect(uint8_t destination) {
        if (!hasPort()) return Error::Invalid;
        return Midi::router().disconnect(_port, destination);
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
                Error err;
                if (_owner) {
                        err = Midi::router().send(_port, e);
                } else {
                        e.port = MidiRouter::NoPort; // from no port: a write to the port's file
                        err = Midi::router().deliver(_port, e);
                }
                if (err == Error::NotExist) return Error::BrokenPipe;
                // WouldBlock: a full destination dropped it, counted there; writes never wait.
        }
        return Error::Ok;
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
                        // A record: its time 0 (now: later times are reserved), 1 to 5 bytes.
                        MidiEvent e;
                        std::memcpy(&e, data.data() + done, sizeof(e));
                        if (e.timeNs != 0 || e.size == 0 || e.size > MidiEvent::MaxBytes) {
                                err = Error::Invalid;
                                break;
                        }
                        for (uint8_t i = 0; i < e.size && err.isOk(); ++i) err = put(e.data[i]);
                } else {
                        err = put(data[done]);
                }
                if (err.isOk()) done += step;
        }
        Midi::router().noteParseErrors(_port, _parser.takeErrors());
        _writeLock.unlock();
        return Result<size_t>(done, err);
}

uint8_t MidiFile::readiness() {
        if (_gone.load(std::memory_order_acquire)) return Readable | HungUp | Writable | Broken;
        uint8_t ready = Writable;
        if (!hasPort() || !_in.isEmpty() || _pendingAt < _pendingSize) ready |= Readable;
        return ready;
}

BMDS_NAMESPACE_END
