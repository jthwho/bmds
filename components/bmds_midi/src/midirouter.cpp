/**
 * @file      midirouter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The MIDI ports and the connections between them (devplan/audio.md §21).
 */

#include <cstring>
#include <bmds/midirouter.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // `midiN` or `N`: the number, or NoPort.
        uint8_t numberOf(StringView name) {
                if (name.startsWith("midi")) name = name.mid(4);
                if (name.isEmpty() || name.size() > 2) return MidiRouter::NoPort;
                unsigned n = 0;
                for (char c : name) {
                        if (c < '0' || c > '9') return MidiRouter::NoPort;
                        n = n * 10 + static_cast<unsigned>(c - '0');
                }
                return n < MidiRouter::MaxPorts ? static_cast<uint8_t>(n) : MidiRouter::NoPort;
        }

} // namespace

void MidiRouter::setName(char (&out)[MaxName], StringView name) {
        name = name.trimmed();
        size_t n = name.size() < MaxName - 1 ? name.size() : MaxName - 1;
        if (n < name.size()) // cut at a UTF-8 character's end
                while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xc0) == 0x80) --n;
        std::memcpy(out, name.data(), n);
        out[n] = '\0';
        return;
}

bool MidiRouter::isPortLocked(uint8_t number, uint8_t side) const {
        return number < MaxPorts && _ports[number].used && (_ports[number].sides & side) != 0;
}

// ---- Ports

Result<uint8_t> MidiRouter::addPort(Kind kind, uint8_t sides, StringView name, MidiSink *destination) {
        if ((sides & (Source | Destination)) == 0 || (sides & ~(Source | Destination)) != 0 ||
            ((sides & Destination) != 0 && destination == nullptr))
                return makeError<uint8_t>(Error::Invalid);
        uint8_t number = NoPort;
        {
                Mutex::Locker lock(_lock, WaitForever);
                for (uint8_t n = 0; n < MaxPorts && number == NoPort; ++n) {
                        Port &p = _ports[n];
                        if (p.used) continue;
                        p = Port();
                        p.used = true;
                        p.kind = kind;
                        p.sides = sides;
                        p.sink = (sides & Destination) != 0 ? destination : nullptr;
                        setName(p.name, name);
                        autoConnectLocked();
                        number = n;
                }
        }
        if (number == NoPort) return makeError<uint8_t>(Error::Full);
        changed();
        return makeResult(number);
}

void MidiRouter::removePort(uint8_t number) {
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (number >= MaxPorts || !_ports[number].used) return;
                size_t kept = 0;
                for (size_t i = 0; i < _connectionCount; ++i) {
                        const Connection c = _connections[i];
                        if (c.source != number && c.destination != number) _connections[kept++] = c;
                }
                _connectionCount = kept;
                for (Tap &t : _taps) {
                        if (t.sink == nullptr || t.port != number) continue;
                        t.sink->portGone();
                        t = Tap();
                }
                _ports[number] = Port();
        }
        changed();
        return;
}

Error MidiRouter::rename(uint8_t number, StringView name) {
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (number >= MaxPorts || !_ports[number].used) return Error::NotExist;
                setName(_ports[number].name, name);
                autoConnectLocked();
        }
        changed();
        return Error::Ok;
}

void MidiRouter::changed() {
        _generation.fetch_add(1, std::memory_order_acq_rel);
        if (_changed != nullptr) _changed(_changedArg);
        return;
}

Result<MidiRouter::PortInfo> MidiRouter::port(uint8_t number) {
        Mutex::Locker lock(_lock, WaitForever);
        if (number >= MaxPorts || !_ports[number].used) return makeError<PortInfo>(Error::NotExist);
        const Port &p = _ports[number];
        PortInfo    info;
        info.number = number;
        info.kind = p.kind;
        info.sides = p.sides;
        std::memcpy(info.name, p.name, sizeof(info.name));
        info.counters = p.counters;
        return makeResult(info);
}

void MidiRouter::findAllLocked(StringView name, bool (&out)[MaxPorts]) const {
        name = name.trimmed();
        const uint8_t number = numberOf(name);
        for (uint8_t n = 0; n < MaxPorts; ++n)
                out[n] = _ports[n].used && (number != NoPort ? n == number : name.equalsIgnoreCase(_ports[n].name));
        return;
}

Result<uint8_t> MidiRouter::find(StringView name) {
        Mutex::Locker lock(_lock, WaitForever);
        bool          found[MaxPorts];
        findAllLocked(name, found);
        for (uint8_t n = 0; n < MaxPorts; ++n)
                if (found[n]) return makeResult(n);
        return makeError<uint8_t>(Error::NotExist);
}

size_t MidiRouter::ports(Span<PortInfo> out) {
        size_t count = 0;
        for (uint8_t n = 0; n < MaxPorts; ++n) {
                auto [info, err] = port(n);
                if (err.isError()) continue;
                if (count < out.size()) out[count] = info;
                ++count;
        }
        return count;
}

// ---- Connections

Error MidiRouter::connectLocked(uint8_t source, uint8_t destination) {
        if (source >= MaxPorts || destination >= MaxPorts || !_ports[source].used || !_ports[destination].used)
                return Error::NotExist;
        if (!isPortLocked(source, Source) || !isPortLocked(destination, Destination)) return Error::Invalid;
        for (size_t i = 0; i < _connectionCount; ++i)
                if (_connections[i].source == source && _connections[i].destination == destination)
                        return Error::Exists;
        if (_connectionCount >= MaxConnections) return Error::Full;
        _connections[_connectionCount++] = Connection{source, destination};
        return Error::Ok;
}

Error MidiRouter::connect(uint8_t source, uint8_t destination) {
        Error err;
        {
                Mutex::Locker lock(_lock, WaitForever);
                err = connectLocked(source, destination);
        }
        if (err.isOk()) changed();
        return err;
}

Error MidiRouter::disconnect(uint8_t source, uint8_t destination) {
        {
                Mutex::Locker lock(_lock, WaitForever);
                size_t        i = 0;
                while (i < _connectionCount &&
                       (_connections[i].source != source || _connections[i].destination != destination))
                        ++i;
                if (i == _connectionCount) return Error::NotExist;
                for (size_t j = i + 1; j < _connectionCount; ++j) _connections[j - 1] = _connections[j];
                --_connectionCount;
        }
        changed();
        return Error::Ok;
}

size_t MidiRouter::connections(Span<Connection> out) {
        Mutex::Locker lock(_lock, WaitForever);
        for (size_t i = 0; i < _connectionCount && i < out.size(); ++i) out[i] = _connections[i];
        return _connectionCount;
}

Error MidiRouter::setAutoConnect(StringView list) {
        if (list.size() >= MaxAutoConnect) return Error::Invalid;
        size_t before, after;
        {
                Mutex::Locker lock(_lock, WaitForever);
                std::memcpy(_autoConnect, list.data(), list.size());
                _autoConnect[list.size()] = '\0';
                before = _connectionCount;
                autoConnectLocked();
                after = _connectionCount;
        }
        if (after != before) changed();
        return Error::Ok;
}

// Every pair of the list whose ports exist, connected (if not already, and while there's room).
void MidiRouter::autoConnectLocked() {
        StringView rest(_autoConnect);
        while (!rest.isEmpty()) {
                const size_t     comma = rest.indexOf(',');
                const StringView item = rest.left(comma);
                rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                const size_t arrow = item.indexOf('>');
                if (arrow == StringView::NPos) continue;
                bool sources[MaxPorts];
                bool destinations[MaxPorts];
                findAllLocked(item.left(arrow), sources);
                findAllLocked(item.mid(arrow + 1), destinations);
                for (uint8_t s = 0; s < MaxPorts; ++s) {
                        if (!sources[s]) continue;
                        for (uint8_t d = 0; d < MaxPorts; ++d)
                                if (destinations[d]) (void)connectLocked(s, d);
                }
        }
        return;
}

// ---- Taps

Error MidiRouter::attach(uint8_t number, MidiSink &tap, bool reading) {
        Mutex::Locker lock(_lock, WaitForever);
        if (number >= MaxPorts || !_ports[number].used) return Error::NotExist;
        if (reading && !isPortLocked(number, Source)) return Error::Invalid;
        Tap *free = nullptr;
        for (Tap &t : _taps) {
                if (t.sink == &tap) return Error::Invalid;
                if (t.sink == nullptr && free == nullptr) free = &t;
        }
        if (free == nullptr) return Error::Full;
        free->sink = &tap;
        free->port = number;
        free->reading = reading;
        return Error::Ok;
}

void MidiRouter::detach(MidiSink &tap) {
        Mutex::Locker lock(_lock, WaitForever);
        for (Tap &t : _taps)
                if (t.sink == &tap) t = Tap();
        return;
}

// ---- Events

Error MidiRouter::send(uint8_t source, const MidiEvent &event) {
        MidiSink *kicks[MaxPorts];
        size_t    kickCount = 0;
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (source >= MaxPorts || !_ports[source].used) return Error::NotExist;
                Port &from = _ports[source];
                if ((from.sides & Source) == 0) return Error::Invalid;
                MidiEvent e = event;
                e.port = source;
                ++from.counters.eventsIn;
                from.counters.lastNs = e.timeNs;
                for (size_t i = 0; i < _connectionCount; ++i) {
                        if (_connections[i].source != source) continue;
                        Port &to = _ports[_connections[i].destination];
                        switch (to.sink->deliver(e)) {
                                case MidiSink::Delivery::Dropped: ++to.counters.dropped; continue;
                                case MidiSink::Delivery::TakenKick: {
                                        bool listed = false;
                                        for (size_t k = 0; k < kickCount; ++k) listed |= kicks[k] == to.sink;
                                        if (!listed && kickCount < MaxPorts) kicks[kickCount++] = to.sink;
                                        break;
                                }
                                case MidiSink::Delivery::Taken: break;
                        }
                        ++to.counters.eventsOut;
                        to.counters.lastNs = e.timeNs;
                }
                // Taps count their own drops.
                for (Tap &t : _taps)
                        if (t.sink != nullptr && t.port == source && t.reading) (void)t.sink->deliver(e);
        }
        for (size_t k = 0; k < kickCount; ++k) kicks[k]->kick();
        return Error::Ok;
}

int64_t MidiRouter::latencyNs(uint8_t number, bool asSource) {
        Mutex::Locker lock(_lock, WaitForever);
        if (number >= MaxPorts || !_ports[number].used) return 0;
        if (!asSource) return _ports[number].sink != nullptr ? _ports[number].sink->latencyNs() : 0;
        int64_t most = 0;
        for (size_t i = 0; i < _connectionCount; ++i) {
                if (_connections[i].source != number) continue;
                const MidiSink *sink = _ports[_connections[i].destination].sink;
                const int64_t   ns = sink != nullptr ? sink->latencyNs() : 0;
                if (ns > most) most = ns;
        }
        return most;
}

Error MidiRouter::deliver(uint8_t destination, const MidiEvent &event) {
        MidiSink *kick = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (destination >= MaxPorts || !_ports[destination].used) return Error::NotExist;
                Port &to = _ports[destination];
                if ((to.sides & Destination) == 0) return Error::Invalid;
                const MidiSink::Delivery d = to.sink->deliver(event);
                if (d == MidiSink::Delivery::Dropped) {
                        ++to.counters.dropped;
                        return Error::WouldBlock;
                }
                ++to.counters.eventsOut;
                to.counters.lastNs = event.timeNs;
                if (d == MidiSink::Delivery::TakenKick) kick = to.sink;
        }
        if (kick != nullptr) kick->kick();
        return Error::Ok;
}

void MidiRouter::noteParseErrors(uint8_t number, uint32_t count) {
        if (count == 0) return;
        Mutex::Locker lock(_lock, WaitForever);
        if (number < MaxPorts && _ports[number].used) _ports[number].counters.parseErrors += count;
        return;
}

void MidiRouter::noteDeviceError(uint8_t number) {
        Mutex::Locker lock(_lock, WaitForever);
        if (number < MaxPorts && _ports[number].used) ++_ports[number].counters.deviceErrors;
        return;
}

BMDS_NAMESPACE_END
