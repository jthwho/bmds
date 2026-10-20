/**
 * @file      midi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's MIDI: its router, the /dev/midi files, the settings under midi/
 * (devplan/audio.md §21).
 */

#include <atomic>
#include <new>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midifile.h>
#include <bmds/psramobject.h>
#include <bmds/stats.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // devplan/audio.md §21: the keys are an interface once shipped.
        BMDS_SYSCONF(midiConnect, "midi/connect", Config::Type::String, "system>wavetable",
                     "MIDI connections made whenever both ports exist: SOURCE>DEST, comma-separated", 0,
                     static_cast<int32_t>(MidiRouter::MaxAutoConnect - 1), nullptr, nullptr)

        // A port's counters (devplan/sysmon.md §6): names, units and labels are an interface once
        // shipped.
        enum StatIndex : size_t {
                StatKind,
                StatEventsIn,
                StatEventsOut,
                StatDropped,
                StatParseErrors,
                StatDeviceErrors,
                StatCount
        };

        constexpr Stat::Desc PortStats[] = {
                {"kind",
                 Stat::Unit::Count,
                 Stat::Kind::State,
                 0,
                 {},
                 "usb|serial|virtual|net|usbdev|system",
                 "What the port is"},
                {"events_in",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 Stat::KeepHistory,
                 {},
                 "",
                 "Events from its source"},
                {"events_out", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Events its destination took"},
                {"dropped",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {Stat::Rule::Zero, Stat::Health::Warning},
                 "",
                 "Events its destination had no room for"},
                {"parse_errors",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {Stat::Rule::Zero, Stat::Health::Warning},
                 "",
                 "Bytes its producers threw away, SysExes cut"},
                {"device_errors",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {Stat::Rule::Zero, Stat::Health::Critical},
                 "",
                 "Transfer, framing and overrun errors"},
        };
        static_assert(sizeof(PortStats) / sizeof(PortStats[0]) == StatCount);

        void readPort(void *arg, Span<int64_t> v);

        // Each port number's counters, registered while the port exists.
        struct PortCounters {
                        PortCounters() : group(PortStats, StatCount, readPort, this) {}
                        uint8_t      number = 0;
                        Stats::Group group;
        };

        struct System {
                        MidiRouter        router;
                        SysConf::Watcher  watcher;
                        std::atomic<bool> connectChanged{true}; // read midi/connect at the next use
                        Mutex             countersLock;         // the counters' registration
                        PortCounters     *counters = nullptr;   // MaxPorts of them, in PSRAM
                        uint8_t           systemPort = MidiRouter::NoPort; // `system` (devplan/audio.md §25)
                        Mutex             watchLock;                       // the watchers, and calls to them
                        struct Watcher {
                                        Midi::Watch fn = nullptr;
                                        void       *arg = nullptr;
                        } watchers[Midi::MaxWatches];
        };

        constinit PsramObject<System>  midi;
        constinit std::atomic<bool>    ready{false};

        void readPort(void *arg, Span<int64_t> v) {
                const PortCounters *c = static_cast<PortCounters *>(arg);
                auto [info, err] = midi->router.port(c->number);
                if (err.isError()) return; // gone: unavailable
                v[StatKind] = int64_t(info.kind);
                v[StatEventsIn] = int64_t(info.counters.eventsIn);
                v[StatEventsOut] = int64_t(info.counters.eventsOut);
                v[StatDropped] = info.counters.dropped;
                v[StatParseErrors] = info.counters.parseErrors;
                v[StatDeviceErrors] = info.counters.deviceErrors;
                return;
        }

        void portCounters(System &s);

        // The graph changed (on the task that changed it, without the router's lock): each port's
        // counters registered while it exists; the watchers told.
        void graphChanged(void *) {
                System &s = *midi;
                portCounters(s);
                Mutex::Locker lock(s.watchLock, WaitForever);
                for (const System::Watcher &w : s.watchers)
                        if (w.fn != nullptr) w.fn(w.arg);
                return;
        }

        void portCounters(System &s) {
                Mutex::Locker locker(s.countersLock, WaitForever);
                if (s.counters == nullptr) return;
                for (uint8_t n = 0; n < MidiRouter::MaxPorts; ++n) {
                        Stats::Group &g = s.counters[n].group;
                        const bool    exists = s.router.port(n).isOk();
                        if (exists && !g.isAdded()) {
                                g.setName("midi.port", n);
                                (void)Stats::add(g);
                        } else if (!exists && g.isAdded()) {
                                Stats::remove(g);
                        }
                }
                return;
        }

        // On the task that changed it, which may hold the registry: the next user reads it.
        void connectChanged(void *) {
                midi->connectChanged.store(true, std::memory_order_release);
                return;
        }

} // namespace

Error Midi::init() {
        if (midi.create() == nullptr) return Error::NoMem;
        void *block = Memory::alloc(MemSpace::Psram, sizeof(PortCounters) * MidiRouter::MaxPorts);
        if (block != nullptr) {
                midi->counters = static_cast<PortCounters *>(block);
                for (uint8_t n = 0; n < MidiRouter::MaxPorts; ++n) new (&midi->counters[n]) PortCounters();
                for (uint8_t n = 0; n < MidiRouter::MaxPorts; ++n) midi->counters[n].number = n;
                midi->router.setChanged(graphChanged, nullptr);
        } else {
                bmdsWarn("midi: the ports' counters aren't shown (no memory)");
        }
        if (SysConf::isReady()) {
                const Error err = midi->watcher.start(bmdsConf_midiConnect.key, connectChanged, nullptr);
                if (err.isError()) bmdsWarn("midi: midi/connect won't be followed: %s", err.desc());
        }
        // `system`: what programs play (/dev/midi), a source only; the first port, midi0.
        auto [system, serr] = midi->router.addPort(MidiRouter::Kind::System, MidiRouter::Source, "system", nullptr);
        if (serr.isError()) bmdsWarn("midi: no system port: %s", serr.desc());
        midi->systemPort = serr.isOk() ? system : MidiRouter::NoPort;
        ready.store(true, std::memory_order_release);
        applySettings();
        const Error err = MidiFile::addFiles();
        if (err.isError()) bmdsWarn("midi: no /dev/midi files: %s", err.desc());
        return Error::Ok;
}

bool Midi::isReady() {
        return ready.load(std::memory_order_acquire);
}

MidiRouter &Midi::router() {
        return midi->router;
}

void Midi::applySettings() {
        System &s = *midi;
        if (!s.connectChanged.exchange(false, std::memory_order_acq_rel) || !SysConf::isReady()) return;
        String list;
        if (SysConf::get(bmdsConf_midiConnect.key, list).isOk() && s.router.setAutoConnect(list).isError())
                bmdsWarn("midi: midi/connect is too long");
        return;
}

Error Midi::addWatch(Watch fn, void *arg) {
        System       &s = *midi;
        Mutex::Locker lock(s.watchLock, WaitForever);
        for (System::Watcher &w : s.watchers) {
                if (w.fn != nullptr) continue;
                w.fn = fn;
                w.arg = arg;
                return Error::Ok;
        }
        return Error::Full;
}

void Midi::removeWatch(Watch fn, void *arg) {
        System       &s = *midi;
        Mutex::Locker lock(s.watchLock, WaitForever); // a call under way ends first
        for (System::Watcher &w : s.watchers)
                if (w.fn == fn && w.arg == arg) w = System::Watcher();
        return;
}

uint8_t Midi::systemPort() {
        return midi->systemPort;
}

const char *Midi::kindName(MidiRouter::Kind kind) {
        switch (kind) {
                case MidiRouter::Kind::Usb: return "usb";
                case MidiRouter::Kind::Serial: return "serial";
                case MidiRouter::Kind::Virtual: return "virtual";
                case MidiRouter::Kind::Net: return "net";
                case MidiRouter::Kind::UsbDevice: return "usbdev";
                case MidiRouter::Kind::System: return "system";
        }
        return "?";
}

BMDS_NAMESPACE_END
