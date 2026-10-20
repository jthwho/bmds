/**
 * @file      netmidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * MIDI over the network (devplan/audio.md §23): AppleMIDI's sessions on two
 * UDP ports of the network task, each up session a MIDI port. What a peer
 * sends is parsed on the network task and sent from its port; what the router
 * delivers to a port waits in its ring until the network task (or a task
 * holding its lock) packs it into RTP-MIDI packets. A thread follows the
 * settings and invites the hosts `midi/net/sessions` names.
 */

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/applemidi.h>
#include <bmds/clock.h>
#include <bmds/hostname.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midiparser.h>
#include <bmds/midiqueue.h>
#include <bmds/mutex.h>
#include <bmds/netmidi.h>
#include <bmds/netservices.h>
#include <bmds/netstack.h>
#include <bmds/netudp.h>
#include <bmds/random.h>
#include <bmds/rtpmidi.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *EnabledKey = "midi/net/enabled";
        constexpr const char *PortKey = "midi/net/port";
        constexpr const char *NameKey = "midi/net/name";
        constexpr const char *AcceptKey = "midi/net/accept";
        constexpr const char *SessionsKey = "midi/net/sessions";

        constexpr uint16_t     DefaultPort = 5004;
        constexpr size_t       RingEvents = 64; // a session's events waiting to be sent
        constexpr size_t       MaxHosts = 4;    // midi/net/sessions' entries kept
        constexpr unsigned int FirstRetryMs = 5'000;
        constexpr unsigned int MaxRetryMs = 300'000;
        constexpr unsigned int ResolveMs = 5'000;
        constexpr size_t       Sessions = AppleMidi::MaxSessions;

        void drain(size_t index);

        // A session's MIDI side. Static, not the service's: the router may kick one whose port has
        // just gone (it lets go of its lock first), and the service may be gone too.
        class Peer final : public MidiSink {
                public:
                        Delivery deliver(const MidiEvent &event) override {
                                if (out.push(event)) return Delivery::TakenKick;
                                dropped.fetch_add(1, std::memory_order_relaxed);
                                return Delivery::Dropped;
                        }

                        void kick() override {
                                if (NetStack::isLocked()) {
                                        drain(index);
                                        return;
                                }
                                NetStack::Locker lock;
                                drain(index);
                                return;
                        }

                        uint8_t               index = 0;
                        uint8_t               port = MidiRouter::NoPort; // the core lock
                        uint8_t               rtpStatus = 0;             // running status across packets
                        bool                  held = false;              // heldEvent didn't fit the last packet
                        MidiEvent             heldEvent;
                        MidiParser            parser;
                        MidiQueue             out; // the router's lock pushes, the core lock pops
                        std::atomic<uint32_t> dropped{0};
        };

        constinit Peer peers[Sessions];

        // The sessions and their ports: in PSRAM while it runs, used under the core lock.
        struct Service final : AppleMidi::Transport {
                        void send(AppleMidi::Channel channel, const AppleMidi::Endpoint &to, ByteSpan bytes) override {
                                (void)(channel == AppleMidi::Channel::Control ? control : data)
                                        .sendTo(to.address, to.port, bytes);
                                return;
                        }
                        void up(size_t s) override;
                        void down(size_t s, AppleMidi::End why) override;
                        void received(size_t s, const RtpMidi::Packet &packet, int64_t nowNs) override;

                        AppleMidi     apple;
                        NetUdp        control;
                        NetUdp        data;
                        uint16_t      port = 0;
                        RtpMidiWriter writers[Sessions];
                        MidiEvent     rings[Sessions][RingEvents];
        };

        // An entry of midi/net/sessions (the thread's).
        struct Host {
                        char     text[64] = {};
                        uint16_t port = DefaultPort;
                        uint32_t address = 0; // once looked up
                        int64_t  nextNs = 0;
                        uint32_t retryMs = FirstRetryMs;
        };

        struct State {
                        SysConf::Watcher watcher;
                        SysConf::Watcher hostnameWatcher;
                        Semaphore        wake{1, 0}; // a setting changed, a session went
                        Mutex            lock;       // the thread's start and end
                        Thread           thread;
                        bool             running = false;   // the thread (lock)
                        Service         *service = nullptr; // the core lock
                        char             name[64] = {};     // the core lock
        };

        // The hosts midi/net/sessions names: the thread's, on its stack (PSRAM).
        struct Hosts {
                        Host   hosts[MaxHosts];
                        size_t count = 0;
                        String text{MemSpace::Psram};
        };

        constinit StaticObject<State> state;

        const char *endName(AppleMidi::End why) {
                switch (why) {
                        case AppleMidi::End::Ours: return "ended";
                        case AppleMidi::End::Theirs: return "ended by the peer";
                        case AppleMidi::End::Refused: return "refused";
                        case AppleMidi::End::NoAnswer: return "unanswered";
                        case AppleMidi::End::Silent: return "silent for a minute";
                }
                return "?";
        }

        void addressText(uint32_t address, char (&out)[16]) {
                std::snprintf(out, sizeof(out), "%u.%u.%u.%u", unsigned(address & 0xff),
                              unsigned((address >> 8) & 0xff), unsigned((address >> 16) & 0xff),
                              unsigned(address >> 24));
                return;
        }

        void Service::up(size_t s) {
                Peer &p = peers[s];
                p.index = uint8_t(s);
                p.parser.reset();
                p.rtpStatus = 0;
                p.held = false;
                p.dropped.store(0);
                writers[s].reset();
                (void)p.out.setStorage(Span<MidiEvent>(rings[s], RingEvents));
                const AppleMidi::Info &info = apple.info(s);
                auto [n, err] = Midi::router().addPort(
                        MidiRouter::Kind::Net, MidiRouter::Source | MidiRouter::Destination, StringView(info.name), &p);
                char where[16];
                addressText(info.control.address, where);
                if (err.isError()) {
                        bmdsWarn("MIDI network: %s (%s): no port: %s", info.name, where, err.desc());
                        return;
                }
                p.port = n;
                bmdsInfo("MIDI network: %s (%s) up as midi%u", info.name, where, unsigned(n));
                state->wake.release(); // the thread applies midi/connect (settings aren't read here)
                return;
        }

        void Service::down(size_t s, AppleMidi::End why) {
                Peer &p = peers[s];
                if (p.port != MidiRouter::NoPort) Midi::router().removePort(p.port); // nothing more delivered
                p.port = MidiRouter::NoPort;
                (void)p.out.setStorage(Span<MidiEvent>());
                const AppleMidi::Info &info = apple.info(s);
                char                   where[16];
                addressText(info.control.address, where);
                bmdsInfo("MIDI network: %s (%s) %s", info.name[0] != '\0' ? info.name : "a session", where,
                         endName(why));
                state->wake.release(); // the thread may invite it again
                return;
        }

        struct Feed {
                        Peer   *peer;
                        int64_t nowNs;
        };

        void feed(void *arg, uint8_t byte) {
                const Feed  &f = *static_cast<Feed *>(arg);
                MidiEvent    events[MidiParser::MaxEvents];
                const size_t n = f.peer->parser.feed(byte, events);
                for (size_t i = 0; i < n; ++i) {
                        events[i].timeNs = f.nowNs;
                        (void)Midi::router().send(f.peer->port, events[i]);
                }
                return;
        }

        void Service::received(size_t s, const RtpMidi::Packet &packet, int64_t nowNs) {
                Peer &p = peers[s];
                if (p.port == MidiRouter::NoPort) return;
                Feed        f{&p, nowNs};
                const Error err = RtpMidi::commands(packet, p.rtpStatus, feed, &f);
                Midi::router().noteParseErrors(p.port, p.parser.takeErrors() + (err.isError() ? 1u : 0u));
                return;
        }

        // Core lock held: what waits for session @p index, in as many packets as it takes.
        void drain(size_t index) {
                Service *svc = state.isCreated() ? state->service : nullptr;
                Peer    &p = peers[index];
                if (svc == nullptr || !svc->apple.isUp(index) || p.port == MidiRouter::NoPort) return;
                RtpMidiWriter         &w = svc->writers[index];
                const AppleMidi::Info &info = svc->apple.info(index);
                const int64_t          now = Clock::monotonicNs();
                for (;;) {
                        MidiEvent e;
                        if (p.held) {
                                e = p.heldEvent;
                                p.held = false;
                        } else if (!p.out.pop(e)) {
                                return;
                        }
                        w.begin(svc->apple.takeSequence(index), AppleMidi::rtpTime(now), svc->apple.ssrc());
                        (void)w.add(e); // one always fits
                        while (p.out.pop(e)) {
                                if (w.add(e)) continue;
                                p.held = true;
                                p.heldEvent = e;
                                break;
                        }
                        (void)svc->data.sendTo(info.data.address, info.data.port, w.finish());
                }
        }

        void schedule();

        void onTimer(void *) {
                schedule();
                return;
        }

        // Core lock held: what is due now, and the timer for the next.
        void schedule() {
                Service *svc = state->service;
                NetStack::cancelCall(onTimer, nullptr);
                if (svc == nullptr) return;
                const int64_t now = Clock::monotonicNs();
                const int64_t next = svc->apple.tick(now);
                if (next == INT64_MAX) return;
                int64_t ms = (next - now) / 1'000'000 + 1;
                if (ms > 600'000) ms = 600'000;
                NetStack::callLater(uint32_t(ms), onTimer, nullptr);
                return;
        }

        void onControl(void *, uint32_t address, uint16_t port, ByteSpan bytes) {
                if (state->service == nullptr) return;
                state->service->apple.receive(AppleMidi::Channel::Control, AppleMidi::Endpoint{address, port}, bytes,
                                              Clock::monotonicNs());
                schedule();
                return;
        }

        void onData(void *, uint32_t address, uint16_t port, ByteSpan bytes) {
                if (state->service == nullptr) return;
                state->service->apple.receive(AppleMidi::Channel::Data, AppleMidi::Endpoint{address, port}, bytes,
                                              Clock::monotonicNs());
                schedule();
                return;
        }

        // ---- The thread: the settings, the service's start and end, the hosts to invite ------------

        struct Settings {
                        bool     enabled = false;
                        uint16_t port = DefaultPort;
                        bool     accept = true;
                        char     name[64] = {};     // as set (empty: the host's)
                        char     sendName[64] = {}; // the one used
                        String   sessions{MemSpace::Psram};
        };

        void read(Settings &s) {
                auto [enabled, eerr] = SysConf::getBool(StringView(EnabledKey));
                s.enabled = eerr.isOk() && enabled;
                auto [port, perr] = SysConf::getInt(StringView(PortKey));
                if (perr.isOk()) s.port = uint16_t(port);
                String text(MemSpace::Psram);
                s.accept = SysConf::get(StringView(AcceptKey), text).isError() || text.view() != StringView("none");
                text.clear();
                if (SysConf::get(StringView(NameKey), text).isOk())
                        std::snprintf(s.name, sizeof(s.name), "%.*s", int(text.size()), text.data());
                if (s.name[0] != '\0') {
                        std::memcpy(s.sendName, s.name, sizeof(s.sendName));
                } else {
                        char host[HostName::MaxBytes + 1];
                        (void)SysConf::hostname(host);
                        std::snprintf(s.sendName, sizeof(s.sendName), "%s", host);
                }
                (void)SysConf::get(StringView(SessionsKey), s.sessions);
                return;
        }

        // midi/net/sessions into the hosts, when it changed.
        void parseHosts(Hosts &list, const String &text) {
                if (text.view() == list.text.view()) return;
                (void)list.text.assign(text.view());
                list.count = 0;
                StringView rest = text.view();
                while (!rest.isEmpty() && list.count < MaxHosts) {
                        const size_t comma = rest.indexOf(',');
                        StringView   item = (comma == StringView::NPos ? rest : rest.left(comma)).trimmed();
                        rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                        if (item.isEmpty()) continue;
                        Host &h = list.hosts[list.count];
                        h = Host();
                        const size_t colon = item.indexOf(':');
                        if (colon != StringView::NPos) {
                                const StringView p = item.mid(colon + 1);
                                uint32_t         v = 0;
                                bool             ok = !p.isEmpty();
                                for (size_t i = 0; ok && i < p.size(); ++i) {
                                        ok = p[i] >= '0' && p[i] <= '9';
                                        v = v * 10 + uint32_t(p[i] - '0');
                                        ok = ok && v <= 65534;
                                }
                                if (!ok || v == 0) {
                                        bmdsWarn("MIDI network: %.*s: not HOST:PORT", int(item.size()), item.data());
                                        continue;
                                }
                                h.port = uint16_t(v);
                                item = item.left(colon);
                        }
                        std::snprintf(h.text, sizeof(h.text), "%.*s", int(item.size()), item.data());
                        ++list.count;
                }
                return;
        }

        Error startService(const Settings &s) {
                void *block = Memory::alloc(MemSpace::Psram, sizeof(Service));
                if (block == nullptr) return Error::NoMem;
                auto    *svc = new (block) Service();
                uint32_t random[3];
                Random::fill(random, sizeof(random));
                NetStack::Locker lock;
                Error            err = svc->control.open(s.port, onControl, nullptr);
                if (err.isOk()) err = svc->data.open(uint16_t(s.port + 1), onData, nullptr);
                if (err.isError()) {
                        svc->~Service();
                        Memory::free(block);
                        return err;
                }
                svc->port = s.port;
                svc->apple.start(*svc, random[0], StringView(s.sendName), s.accept, random[1] | 1);
                state->service = svc;
                std::memcpy(state->name, s.sendName, sizeof(state->name));
                schedule();
                return Error::Ok;
        }

        void stopService() {
                NetStack::Locker lock;
                Service         *svc = state->service;
                if (svc == nullptr) return;
                svc->apple.endAll(); // BY to each; their ports go
                NetStack::cancelCall(onTimer, nullptr);
                state->service = nullptr;
                svc->~Service(); // the ports closed
                Memory::free(svc);
                return;
        }

        bool sessionWith(const Host &h) {
                Service *svc = state->service;
                if (svc == nullptr || h.address == 0) return false;
                for (size_t i = 0; i < Sessions; ++i) {
                        const AppleMidi::Info &info = svc->apple.info(i);
                        if (info.state != AppleMidi::State::Free && info.control.address == h.address &&
                            info.control.port == h.port)
                                return true;
                }
                return false;
        }

        void drainWatchers() {
                SysConf::Id id;
                while (state->watcher.next(id)) {}
                (void)state->watcher.overflowed();
                while (state->hostnameWatcher.next(id)) {}
                (void)state->hostnameWatcher.overflowed();
                return;
        }

        void serve(void *) {
                Hosts hosts;
                for (;;) {
                        drainWatchers();
                        Midi::applySettings(); // midi/connect, for a session's new port
                        Settings s;
                        read(s);
                        if (!s.enabled) break;
                        bool running;
                        {
                                NetStack::QuietLocker lock;
                                running = state->service != nullptr && state->service->port == s.port;
                        }
                        if (!running) {
                                stopService();
                                const Error err = startService(s);
                                if (err.isError()) {
                                        bmdsErr("MIDI network: UDP %u and %u: %s", s.port, s.port + 1u,
                                                err == Error::Exists ? "in use" : err.desc());
                                        (void)state->wake.acquire(WaitForever);
                                        continue;
                                }
                                bmdsInfo("MIDI network: on UDP %u and %u as \"%s\"", s.port, s.port + 1u, s.sendName);
                        }
                        {
                                NetStack::Locker lock;
                                state->service->apple.setName(StringView(s.sendName));
                                state->service->apple.setAccept(s.accept);
                                std::memcpy(state->name, s.sendName, sizeof(state->name));
                        }
                        (void)NetServices::advertise("_apple-midi", true, s.port, StringView(s.name));
                        parseHosts(hosts, s.sessions);
                        // The hosts to invite: each not up and due, looked up and invited.
                        int64_t       next = INT64_MAX;
                        const int64_t now = Clock::monotonicNs();
                        for (size_t i = 0; i < hosts.count; ++i) {
                                Host &h = hosts.hosts[i];
                                bool  with;
                                {
                                        NetStack::QuietLocker lock;
                                        with = sessionWith(h);
                                }
                                if (with) {
                                        h.retryMs = FirstRetryMs;
                                        continue;
                                }
                                if (now < h.nextNs) {
                                        if (h.nextNs < next) next = h.nextNs;
                                        continue;
                                }
                                uint32_t    address = 0;
                                const Error err = NetStack::resolve(StringView(h.text), address, ResolveMs);
                                if (err.isOk()) {
                                        h.address = address;
                                        NetStack::Locker lock;
                                        if (state->service != nullptr) {
                                                (void)state->service->apple.invite(AppleMidi::Endpoint{address, h.port},
                                                                                   Clock::monotonicNs());
                                                schedule();
                                        }
                                } else {
                                        bmdsWarn("MIDI network: %s: %s", h.text, err.desc());
                                }
                                h.nextNs = Clock::monotonicNs() + int64_t(h.retryMs) * 1'000'000;
                                h.retryMs = h.retryMs * 2 > MaxRetryMs ? MaxRetryMs : h.retryMs * 2;
                                if (h.nextNs < next) next = h.nextNs;
                        }
                        if (next == INT64_MAX) {
                                (void)state->wake.acquire(WaitForever);
                        } else {
                                const int64_t ms = (next - Clock::monotonicNs()) / 1'000'000;
                                (void)state->wake.acquire(ms < 1 ? 1u : unsigned(ms));
                        }
                }
                stopService();
                (void)NetServices::advertise("_apple-midi", true, 0, StringView());
                bmdsInfo("MIDI network: off");
                Mutex::Locker lock(state->lock, WaitForever);
                state->running = false;
                state->thread.detach();
                return;
        }

        bool enabled() {
                auto [on, err] = SysConf::getBool(StringView(EnabledKey));
                return err.isOk() && on;
        }

        void startThread() {
                Mutex::Locker lock(state->lock, WaitForever);
                if (state->running || !enabled()) return;
                Thread::Config config;
                config.name = "netmidi";
                config.stackBytes = 6144; // name lookups, the host list
                config.stackSpace = MemSpace::Psram;
                config.kernel = true;
                const Error err = state->thread.start(config, &serve, nullptr);
                if (err.isError()) {
                        bmdsErr("MIDI network: no thread: %s", err.desc());
                        return;
                }
                state->running = true;
                return;
        }

        void settingsChanged(void *) {
                state->wake.release();
                startThread();
                return;
        }

        Error checkSessions(StringView value) {
                return value.size() <= 255 ? Error::Ok : Error::OutOfRange;
        }

} // namespace

Error NetMidi::init() {
        state.create();
        static constexpr Config::Param Params[] = {
                {"midi/net/enabled", Config::Type::Bool, "false",
                 "MIDI over the network (RTP-MIDI, AppleMIDI's sessions: macOS, iOS, rtpMIDI, rtpmidid)", 0, 0, nullptr,
                 nullptr},
                {"midi/net/port", Config::Type::Int, "5004", "Network MIDI's UDP port (control; data on the next)",
                 1024, 65534, nullptr, nullptr},
                {"midi/net/name", Config::Type::String, "", "The name other computers see (empty: the host's)", 0, 63,
                 nullptr, nullptr},
                {"midi/net/accept", Config::Type::Enum, "all",
                 "Sessions others start: all (anyone on the network may), or none", 0, 0, "all|none", nullptr},
                {"midi/net/sessions", Config::Type::String, "",
                 "Hosts to start sessions with, kept up: HOST or HOST:PORT, comma separated", 0, 255, nullptr,
                 checkSessions},
        };
        for (const Config::Param &p : Params) {
                const Result<SysConf::Id> id = SysConf::declareKernel(p);
                if (id.isError()) {
                        bmdsErr("midi: setting %s not declared: %s", p.key, id.error().desc());
                        return id.error();
                }
        }
        Error err = state->watcher.start(StringView("midi/net"), &settingsChanged, nullptr);
        if (err.isOk()) err = state->hostnameWatcher.start(StringView("system/hostname"), &settingsChanged, nullptr);
        if (err.isError()) return err;
        startThread();
        return Error::Ok;
}

NetMidi::Status NetMidi::status() {
        Status s;
        s.enabled = enabled();
        if (!state.isCreated()) return s;
        String text(MemSpace::Psram);
        s.accepting = SysConf::get(StringView(AcceptKey), text).isError() || text.view() != StringView("none");
        NetStack::QuietLocker lock;
        if (state->service != nullptr) {
                s.running = true;
                s.port = state->service->port;
                std::memcpy(s.name, state->name, sizeof(s.name));
        }
        return s;
}

size_t NetMidi::sessions(Span<SessionInfo> out) {
        if (!state.isCreated()) return 0;
        NetStack::QuietLocker lock;
        Service              *svc = state->service;
        if (svc == nullptr) return 0;
        size_t n = 0;
        for (size_t i = 0; i < Sessions && n < out.size(); ++i) {
                const AppleMidi::Info &info = svc->apple.info(i);
                if (info.state == AppleMidi::State::Free) continue;
                SessionInfo &o = out[n++];
                o.index = uint8_t(i);
                o.state = info.state;
                o.inviter = info.inviter;
                std::memcpy(o.name, info.name, sizeof(o.name));
                o.address = info.control.address;
                o.port = info.control.port;
                o.midiPort = info.state == AppleMidi::State::Up ? peers[i].port : MidiRouter::NoPort;
                o.roundTripUs = info.roundTripUnits * 100;
                o.syncs = info.syncs;
                o.packetsIn = info.packetsIn;
                o.packetsOut = info.packetsOut;
                o.lost = info.lost;
                o.journals = info.journals;
                o.dropped = peers[i].dropped.load(std::memory_order_relaxed);
                o.upNs = info.upNs;
        }
        return n;
}

Error NetMidi::connect(StringView host, uint16_t port) {
        if (!state.isCreated()) return Error::InvalidState;
        {
                NetStack::QuietLocker lock;
                if (state->service == nullptr) return Error::InvalidState;
        }
        uint32_t address = 0;
        Error    err = NetStack::resolve(host, address, ResolveMs);
        if (err.isError()) return err;
        NetStack::Locker lock;
        if (state->service == nullptr) return Error::InvalidState;
        err = state->service->apple.invite(AppleMidi::Endpoint{address, port}, Clock::monotonicNs()).error();
        schedule();
        return err;
}

Error NetMidi::disconnect(uint8_t index) {
        if (!state.isCreated()) return Error::NotExist;
        NetStack::Locker lock;
        Service         *svc = state->service;
        if (svc == nullptr || index >= Sessions || svc->apple.info(index).state == AppleMidi::State::Free)
                return Error::NotExist;
        svc->apple.end(index);
        schedule();
        return Error::Ok;
}

BMDS_NAMESPACE_END
