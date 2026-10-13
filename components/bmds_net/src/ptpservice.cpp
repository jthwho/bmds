/**
 * @file      ptpservice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The kernel's PTP service (ptpservice.h, devplan/network.md §31c): a task on core 1 that
 * sleeps on one semaphore, released by its flows' listeners, with the port's next deadline as
 * the timeout. It owns the port, its flows and the clocks' following; none of it takes lwIP's
 * core lock.
 */

#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/ethernetdevice.h>
#include <bmds/log.h>
#include <bmds/mutex.h>
#include <bmds/netflow.h>
#include <bmds/psramobject.h>
#include <bmds/ptp.h>
#include <bmds/semaphore.h>
#include <bmds/stats.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/task.h>

#include "ntpclient.h"
#include "ptpservice.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Follow = PtpService::Follow;

        constexpr int64_t      NsPerSecond = 1'000'000'000;
        constexpr int64_t      FollowIntervalNs = NsPerSecond; // as phc2sys: once a second
        constexpr int64_t      RetryNs = 10 * NsPerSecond;     // flows that wouldn't open
        constexpr unsigned int SendTimeoutMs = 5;
        constexpr int32_t      MaxFollowPpb = 500'000; // the system clock's bound (ClockModel)
        // The clocks' following (once a second): a step beyond these, else a PI on the offset over
        // the other clock's rate. 0.2 and 0.02 settle in about 10 s; a microsecond of reading
        // noise moves the rate 0.2 ppm at most.
        constexpr int64_t FollowFirstStepNs = 20'000;
        constexpr int64_t FollowStepNs = 1'000'000; // the other's step (a new master, NTP's) followed by one
        constexpr double  FollowKp = 0.2;           // ppb per ns of offset
        constexpr double  FollowKi = 0.02;
        constexpr double  FollowMaxCorrectionPpb = 100'000;
        constexpr uint8_t TimeSourceNtp = 0x50;
        constexpr uint8_t TimeSourceOscillator = 0xa0;
        constexpr uint8_t AccuracyUnknown = 0xfe;

        enum Setting : uint8_t {
                Enabled,
                Interface,
                Domain,
                Profile,
                Transport,
                Priority1,
                Priority2,
                SlaveOnly,
                SyncInterval,
                AnnounceInterval,
                DelayInterval,
                SettingCount,
        };

        // What the settings say.
        struct Wanted {
                        bool              enabled = false;
                        char              interface[16] = {};
                        bool              l2 = false;
                        bool              defaultProfile = false;
                        Ptp::Port::Config config;
        };

        class Service final : public Ptp::Port::Io {
                public:
                        Service();

                        bool                 send(bool event, Span<const uint8_t> bytes, uint32_t *ticket) override;
                        int64_t              now() override { return _device->ptpTime().toNs(); }
                        void                 step(int64_t ns) override;
                        void                 setFrequency(double ppb) override;
                        double               frequency() override { return _ptpPpb; }
                        Ptp::Port::UtcOffset utcOffset() override;

                        void run();
                        void apply(const Wanted &w, int64_t nowNs);
                        void stop();
                        void fill(PtpService::Status &out);
                        void readCounters(Span<int64_t> v);

                        SysConf::Id       settings[SettingCount] = {};
                        SysConf::Watcher  watcher;
                        std::atomic<bool> settingsPending{true};
                        std::atomic<bool> owns{false};
                        std::atomic<bool> taskStarted{false};
                        Semaphore         wake{1, 1};
                        Mutex             lock; // all below
                        Task              task;
                        Stats::Group      group;

                private:
                        Ptp::Port *port() { return _running ? reinterpret_cast<Ptp::Port *>(_portBlock) : nullptr; }
                        Error      start(const Wanted &w, int64_t nowNs);
                        void       drain(NetFlow &flow, int64_t nowNs);
                        void       follow(int64_t nowNs);
                        Follow     choose(const NtpClient::Sync &ntp);
                        void       setQuality(const NtpClient::Sync &ntp, int64_t nowNs);
                        bool       readClocks(int64_t &ptpNs, int64_t &utcNs);
                        void       noteState(int64_t nowNs);

                        Wanted            _wanted;
                        Error             _error;
                        bool              _running = false;
                        EthernetDevice   *_device = nullptr;
                        NetFlow           _event;   // udp: 319; l2: the one flow
                        NetFlow           _general; // udp: 320
                        double            _ptpPpb = 0;
                        int64_t           _retryAt = INT64_MAX;
                        int64_t           _followAt = INT64_MAX;
                        Ptp::Port::State  _lastState = Ptp::Port::State::Disabled;
                        int64_t           _stateAt = 0;
                        Follow            _follow = Follow::None;
                        double            _followIntegral = 0; // ppb
                        bool              _followStarted = false;
                        int64_t           _followOffsetNs = 0;
                        uint32_t          _followBracketNs = 0;
                        NetFlow::Counters _closedFlows; // flows' counters from before their last close
                        alignas(Ptp::Port) unsigned char _portBlock[sizeof(Ptp::Port)];
        };

        void readCountersOf(void *arg, Span<int64_t> v) {
                static_cast<Service *>(arg)->readCounters(v);
                return;
        }

        using Unit = Stat::Unit;
        using Kind = Stat::Kind;

        // net.ptp: names are an interface once shipped. The message types' counters in
        // MessageCounted's order, received then sent.
        constexpr Ptp::MessageType MessageCounted[] = {Ptp::MessageType::Sync, Ptp::MessageType::DelayReq,
                                                       Ptp::MessageType::FollowUp, Ptp::MessageType::DelayResp,
                                                       Ptp::MessageType::Announce};
        constexpr size_t           Counted = sizeof(MessageCounted) / sizeof(MessageCounted[0]);

        constexpr Stat::Desc Counters[] = {
                {"state",
                 Unit::Count,
                 Kind::State,
                 0,
                 {},
                 "disabled|listening|master|passive|uncalibrated|slave",
                 "The port's state"},
                {"offset_ns",
                 Unit::Nanos,
                 Kind::Level,
                 Stat::KeepHistory,
                 {},
                 "",
                 "The last offset from the master (positive: this clock ahead)"},
                {"delay_ns", Unit::Nanos, Kind::Level, 0, {}, "", "The mean path delay to the master"},
                {"frequency_ppb", Unit::Ppb, Kind::Level, 0, {}, "", "ptpclock0's frequency correction"},
                {"rx_sync", Unit::Count, Kind::Total, 0, {}, "", "Sync messages received"},
                {"rx_delay_req", Unit::Count, Kind::Total, 0, {}, "", "Delay_Req messages received"},
                {"rx_follow_up", Unit::Count, Kind::Total, 0, {}, "", "Follow_Up messages received"},
                {"rx_delay_resp", Unit::Count, Kind::Total, 0, {}, "", "Delay_Resp messages received"},
                {"rx_announce", Unit::Count, Kind::Total, 0, {}, "", "Announce messages received"},
                {"tx_sync", Unit::Count, Kind::Total, 0, {}, "", "Sync messages sent"},
                {"tx_delay_req", Unit::Count, Kind::Total, 0, {}, "", "Delay_Req messages sent"},
                {"tx_follow_up", Unit::Count, Kind::Total, 0, {}, "", "Follow_Up messages sent"},
                {"tx_delay_resp", Unit::Count, Kind::Total, 0, {}, "", "Delay_Resp messages sent"},
                {"tx_announce", Unit::Count, Kind::Total, 0, {}, "", "Announce messages sent"},
                {"ignored",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Messages not for this port: another domain or version, its own, a stranger's"},
                {"send_failures", Unit::Count, Kind::Total, 0, {}, "", "Messages that couldn't be sent"},
                {"announce_timeouts", Unit::Count, Kind::Total, 0, {}, "", "Masters that fell silent"},
                {"state_changes", Unit::Count, Kind::Total, 0, {}, "", "Changes of the port's state"},
                {"master_changes", Unit::Count, Kind::Total, 0, {}, "", "Changes of the master followed"},
                {"steps", Unit::Count, Kind::Total, 0, {}, "", "Times ptpclock0 was stepped, not slewed"},
                {"follow_ups_missed", Unit::Count, Kind::Total, 0, {}, "", "Syncs whose Follow_Up never came"},
        };
        constexpr size_t CounterCount = sizeof(Counters) / sizeof(Counters[0]);

        Service::Service() : group(Counters, CounterCount, readCountersOf, this) {}

        constinit PsramObject<Service> service;
        constinit bool                 started = false;

        void taskEntry(void *);

        // On, and no task yet: the task (none until PTP is first enabled; it stays, idle, after).
        void startTask() {
                if (service->taskStarted.load()) return;
                auto [on, err] = SysConf::getBool(service->settings[Enabled]);
                if (err.isError() || !on) return;
                bool expected = false;
                if (!service->taskStarted.compare_exchange_strong(expected, true)) return;
                // Real-time messages on core 1, away from the driver's task (§30c).
                Task::Config tc;
                tc.name = "ptp";
                tc.stackBytes = 4096;
                tc.priority = CONFIG_BMDS_PTP_PRIORITY;
                tc.core = 1;
                err = service->task.start(tc, &taskEntry, nullptr);
                if (err.isError()) {
                        bmdsErr("ptp: no task: %s", err.desc());
                        service->taskStarted.store(false);
                }
                return;
        }

        void settingsChanged(void *) {
                service->settingsPending.store(true);
                service->wake.release();
                startTask();
                return;
        }

        double clampPpb(double ppb) {
                return ppb > MaxFollowPpb ? MaxFollowPpb : ppb < -MaxFollowPpb ? -MaxFollowPpb : ppb;
        }

        void flowReady(void *) {
                service->wake.release();
                return;
        }

        int64_t monoNs() {
                return Clock::monotonicNs();
        }

        // IEEE 1588-2008 table 6: the smallest accuracy code that covers @p ns.
        uint8_t accuracyFor(int64_t ns) {
                // 25 ns, 100 ns, 250 ns, 1 us ... 10 s: each code 2.5 or 4 times the last.
                int64_t limit = 25;
                for (uint8_t code = 0x20; code <= 0x30; ++code) {
                        if (ns <= limit) return code;
                        limit = (code & 1) == 0 ? limit * 4 : limit * 5 / 2;
                }
                return 0x31;
        }

        int8_t logInterval(const String &value, int8_t automatic) {
                if (value.view() == StringView("auto") || value.isEmpty()) return automatic;
                return static_cast<int8_t>(std::atoi(value.cStr()));
        }

        // The settings as they stand (blocking: sysconf may read its store).
        Wanted readSettings() {
                Wanted w;
                auto   getBool = [](SysConf::Id id) {
                        auto [v, err] = SysConf::getBool(id);
                        return err.isOk() && v;
                };
                auto getInt = [](SysConf::Id id, int32_t fallback) {
                        auto [v, err] = SysConf::getInt(id);
                        return err.isOk() ? v : fallback;
                };
                const SysConf::Id *ids = service->settings;
                w.enabled = getBool(ids[Enabled]);
                String text;
                (void)SysConf::get(ids[Interface], text);
                const size_t n = text.size() < sizeof(w.interface) ? text.size() : sizeof(w.interface) - 1;
                std::memcpy(w.interface, text.data(), n);
                w.interface[n] = '\0';
                (void)SysConf::get(ids[Transport], text);
                w.l2 = text.view() == StringView("l2");
                (void)SysConf::get(ids[Profile], text);
                w.defaultProfile = text.view() == StringView("default");
                Ptp::Port::Config &c = w.config;
                c.domain = static_cast<uint8_t>(getInt(ids[Domain], 0));
                c.priority1 = static_cast<uint8_t>(getInt(ids[Priority1], 128));
                c.priority2 = static_cast<uint8_t>(getInt(ids[Priority2], 128));
                c.slaveOnly = getBool(ids[SlaveOnly]);
                // The profiles' defaults: AES67's media profile, and IEEE 1588-2008's J.3.
                const int8_t sync = w.defaultProfile ? 0 : -3;
                (void)SysConf::get(ids[SyncInterval], text);
                c.logSyncInterval = logInterval(text, sync);
                (void)SysConf::get(ids[AnnounceInterval], text);
                c.logAnnounceInterval = logInterval(text, 1);
                (void)SysConf::get(ids[DelayInterval], text);
                c.logMinDelayReqInterval = logInterval(text, sync);
                c.announceReceiptTimeout = 3;
                return w;
        }

        // TAI less UTC for ptpclock0's time: a master's valid offset; none for a master whose time isn't
        // PTP's timescale (linuxptp's on software stamps runs on UTC); otherwise the assumed one.
        int16_t utcOffsetOf(const Ptp::Port::Status &s, bool &fromMaster, bool &arbitrary) {
                const bool following = s.state == Ptp::Port::State::Slave || s.state == Ptp::Port::State::Uncalibrated;
                arbitrary = following && (s.timeFlags & Ptp::PtpTimescale) == 0;
                fromMaster = following && (arbitrary || s.utcOffset.valid);
                if (arbitrary) return 0;
                return fromMaster ? s.utcOffset.seconds : PtpService::DefaultUtcOffset;
        }

        // --- The port's world -------------------------------------------------------------

        bool Service::send(bool event, Span<const uint8_t> bytes, uint32_t *ticket) {
                NetFlow &flow = event || _wanted.l2 ? _event : _general;
                auto [t, err] = flow.send(bytes, event ? uint32_t(NetFlow::Stamp) : 0u, SendTimeoutMs);
                if (err.isError()) return false;
                if (ticket != nullptr) *ticket = t;
                return true;
        }

        void Service::step(int64_t ns) {
                const Error err = _device->stepPtpTime(ns);
                if (err.isError()) bmdsWarn("ptp: ptpclock0 not stepped: %s", err.desc());
                return;
        }

        void Service::setFrequency(double ppb) {
                if (ppb > MaxFollowPpb) ppb = MaxFollowPpb;
                if (ppb < -MaxFollowPpb) ppb = -MaxFollowPpb;
                // Whole ppb (2.7 addend steps): the servo's integral takes up the rounding.
                if (_device->setPtpFrequency(static_cast<int32_t>(std::lround(ppb))).isOk()) _ptpPpb = ppb;
                return;
        }

        Ptp::Port::UtcOffset Service::utcOffset() {
                Ptp::Port::UtcOffset u; // until step 4's leap-second file: 37 s, not valid
                u.seconds = PtpService::DefaultUtcOffset;
                u.valid = false;
                return u;
        }

        // --- Running ----------------------------------------------------------------------

        EthernetDevice *deviceNamed(const char *name) {
                for (size_t i = 0; i < EthernetDevice::count(); ++i) {
                        auto [d, err] = EthernetDevice::device(i);
                        if (err.isOk() && std::strcmp(d->name(), name) == 0) return d;
                }
                return nullptr;
        }

        // Lock held.
        Error Service::start(const Wanted &w, int64_t nowNs) {
                EthernetDevice *d = deviceNamed(w.interface);
                if (d == nullptr) return Error::NotExist;
                NetFlow::Config c;
                c.packetUs = 1000;
                if (w.l2) {
                        c.kind = NetClassifier::Kind::EtherType;
                        c.etherType = Ptp::EtherType;
                        c.group = Ptp::GroupMac;
                        c.depth = 3;
                } else {
                        c.kind = NetClassifier::Kind::Udp;
                        c.address = Ptp::Group;
                        c.port = Ptp::EventPort;
                        c.depth = 2;
                        c.ttl = 1;
                        c.dscp = 46; // AES67's for PTP (EF)
                }
                _event.setListener(&flowReady, nullptr);
                _general.setListener(&flowReady, nullptr);
                Error err = _event.open(*d, c);
                if (err.isOk() && w.l2) err = _event.setDestination(Ptp::GroupMac);
                if (err.isOk() && !w.l2) {
                        _event.setIpFields(1, 1, c.dscp);
                        err = _event.tryConnect(Ptp::Group, Ptp::EventPort);
                        c.port = Ptp::GeneralPort;
                        if (err.isOk()) err = _general.open(*d, c);
                        if (err.isOk()) {
                                _general.setIpFields(1, 1, c.dscp);
                                err = _general.tryConnect(Ptp::Group, Ptp::GeneralPort);
                        }
                }
                if (err.isError()) {
                        _event.close();
                        _general.close();
                        return err;
                }
                _device = d;
                // ptpclock0 runs on TAI: the driver seeds it with the system clock's UTC.
                static bool onTai = false;
                if (!onTai && Clock::isSet()) {
                        (void)d->setPtpTime(
                                PtpTime::fromNs(Clock::now().ns + PtpService::DefaultUtcOffset * NsPerSecond));
                        onTai = true;
                }
                _ptpPpb = (static_cast<double>(d->ptpAddend()) / EthernetDevice::NominalPtpAddend - 1.0) * 1e9;
                auto *p = new (_portBlock) Ptp::Port(*this, Ptp::ClockIdentity::fromMac(d->address()), w.config);
                _running = true;
                p->start(nowNs);
                _lastState = p->state();
                _stateAt = nowNs;
                _followAt = nowNs + FollowIntervalNs;
                bmdsInfo("ptp: on %s (%s, %s profile, domain %u)", w.interface, w.l2 ? "l2" : "udp",
                         w.defaultProfile ? "default" : "media", w.config.domain);
                return Error::Ok;
        }

        // Lock held.
        void Service::stop() {
                if (_running) {
                        port()->stop();
                        port()->~Port();
                        _running = false;
                        bmdsInfo("ptp: stopped");
                }
                for (NetFlow *f : {&_event, &_general}) {
                        if (!f->isOpen()) continue;
                        const NetFlow::Counters c = f->counters();
                        _closedFlows.rxPackets += c.rxPackets;
                        _closedFlows.rxDropped += c.rxDropped;
                        _closedFlows.txPackets += c.txPackets;
                        _closedFlows.txErrors += c.txErrors;
                        _closedFlows.txStamps += c.txStamps;
                        _closedFlows.txStampsLost += c.txStampsLost;
                        f->close();
                }
                _device = nullptr;
                _followAt = INT64_MAX;
                _follow = Follow::None;
                owns.store(false);
                return;
        }

        // Lock held: the settings applied. Interface, transport, a change of identity: a new port.
        void Service::apply(const Wanted &w, int64_t nowNs) {
                const bool restart = !w.enabled || !_running || w.l2 != _wanted.l2 ||
                                     std::strcmp(w.interface, _wanted.interface) != 0;
                Ptp::Port::Config config = w.config;
                if (_running) {
                        // The quality and time source are the service's (NTP's), not the settings'.
                        config.quality = port()->config().quality;
                        config.timeSource = port()->config().timeSource;
                }
                _wanted = w;
                _wanted.config = config;
                if (restart) {
                        stop();
                        _retryAt = INT64_MAX;
                        _error = Error::Ok;
                        if (!w.enabled) return;
                        _error = start(_wanted, nowNs);
                        if (_error.isError()) {
                                bmdsWarn("ptp: not started on %s: %s", w.interface, _error.desc());
                                _retryAt = nowNs + RetryNs;
                        }
                        return;
                }
                port()->setConfig(config, nowNs);
                return;
        }

        // Lock held: everything the flow holds, to the port.
        void Service::drain(NetFlow &flow, int64_t nowNs) {
                if (!flow.isOpen()) return;
                for (;;) {
                        auto [packet, err] = flow.tryReceive();
                        if (err.isError()) break;
                        const Span<const uint8_t> payload = packet.payload();
                        // Event messages need their arrival; the MAC stamps every frame.
                        const bool event = payload.size() > 0 && (payload[0] & 0x0f) < 0x8;
                        if (packet.frame.stamped || !event) port()->receive(payload, packet.frame.stamp.toNs(), nowNs);
                        flow.release(packet);
                }
                for (;;) {
                        auto [stamp, err] = flow.tryReceiveTxStamp();
                        if (err.isError()) break;
                        port()->transmitted(stamp.ticket, stamp.time.toNs(), nowNs);
                }
                return;
        }

        void Service::noteState(int64_t nowNs) {
                const Ptp::Port::State s = port()->state();
                if (s == _lastState) return;
                bmdsInfo("ptp: %s", Ptp::Port::stateName(s));
                _lastState = s;
                _stateAt = nowNs;
                return;
        }

        // ptpclock0 and the system clock read around each other: the tightest of 4.
        bool Service::readClocks(int64_t &ptpNs, int64_t &utcNs) {
                int64_t best = INT64_MAX;
                for (int i = 0; i < 4; ++i) {
                        const int64_t a = _device->ptpTime().toNs();
                        const int64_t u = Clock::now().ns;
                        const int64_t b = _device->ptpTime().toNs();
                        if (b < a || b - a >= best) continue;
                        best = b - a;
                        ptpNs = a + best / 2;
                        utcNs = u;
                }
                if (best == INT64_MAX) return false;
                _followBracketNs = best > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(best);
                return true;
        }

        Follow Service::choose(const NtpClient::Sync &ntp) {
                const Ptp::Port::Status &s = port()->status();
                if (s.state == Ptp::Port::State::Slave && s.servo == Ptp::Servo::State::Locked)
                        return Follow::SystemFollows;
                if (ntp.synced) return s.state == Ptp::Port::State::Master ? Follow::PtpFollows : Follow::None;
                if (Clock::status().source == ClockSource::Ptp) return Follow::SystemFollows;
                return Follow::None;
        }

        // What a master announces of itself: NTP's time and how far off it may be, while synced.
        void Service::setQuality(const NtpClient::Sync &ntp, int64_t nowNs) {
                Ptp::Port::Config c = port()->config();
                uint8_t           accuracy = AccuracyUnknown, source = TimeSourceOscillator;
                if (ntp.synced) {
                        const int64_t off = ntp.offsetNs < 0 ? -ntp.offsetNs : ntp.offsetNs;
                        accuracy = accuracyFor(off + ntp.delayNs / 2);
                        source = TimeSourceNtp;
                }
                if (c.quality.accuracy == accuracy && c.timeSource == source) return;
                c.quality.accuracy = accuracy;
                c.timeSource = source;
                _wanted.config = c;
                port()->setConfig(c, nowNs);
                return;
        }

        // Once a second, lock held: the clocks following each other (devplan/network.md §31c).
        void Service::follow(int64_t nowNs) {
                _followAt = nowNs + FollowIntervalNs;
                const NtpClient::Sync ntp = NtpClient::sync();
                setQuality(ntp, nowNs);
                const Follow want = choose(ntp);
                if (want != _follow) {
                        static constexpr const char *Names[] = {"neither clock follows the other",
                                                                "the system clock follows ptpclock0",
                                                                "ptpclock0 follows the system clock"};
                        bmdsInfo("ptp: %s", Names[static_cast<int>(want)]);
                        _follow = want;
                        _followIntegral = 0;
                        _followStarted = false;
                        owns.store(want == Follow::SystemFollows);
                }
                if (want == Follow::None) return;
                int64_t ptp = 0, utc = 0;
                if (!readClocks(ptp, utc)) return;
                const Ptp::Port::Status &s = port()->status();
                bool                     fromMaster, arbitrary;
                const int64_t tai = static_cast<int64_t>(utcOffsetOf(s, fromMaster, arbitrary)) * NsPerSecond;
                const bool    system = want == Follow::SystemFollows;
                // The follower's offset from the clock it follows (positive: ahead).
                const int64_t offset = system ? utc - (ptp - tai) : ptp - tai - utc;
                _followOffsetNs = offset;
                const int64_t magnitude = offset < 0 ? -offset : offset;
                const bool stepNow = magnitude > FollowStepNs || (!_followStarted && magnitude > FollowFirstStepNs) ||
                                     (system && Clock::status().source != ClockSource::Ptp);
                _followStarted = true;
                if (stepNow) {
                        _followIntegral = 0;
                        if (system)
                                (void)Clock::set(Timestamp{Clock::now().ns - offset}, ClockSource::Ptp);
                        else
                                step(-offset);
                } else {
                        _followIntegral -= FollowKi * static_cast<double>(offset);
                        if (_followIntegral > FollowMaxCorrectionPpb) _followIntegral = FollowMaxCorrectionPpb;
                        if (_followIntegral < -FollowMaxCorrectionPpb) _followIntegral = -FollowMaxCorrectionPpb;
                }
                // Both count the same crystal: the follower takes the other's rate, and the PI only
                // the offset's correction (gentle: the clocks are read within about a microsecond).
                const double correction = stepNow ? 0 : _followIntegral - FollowKp * static_cast<double>(offset);
                if (system)
                        (void)Clock::setFrequency(static_cast<int32_t>(std::lround(clampPpb(_ptpPpb + correction))));
                else
                        setFrequency(Clock::status().frequencyPpb + correction);
                return;
        }

        void Service::run() {
                for (;;) {
                        int64_t now = monoNs();
                        int64_t due = INT64_MAX;
                        {
                                Mutex::Locker l(lock, WaitForever);
                                if (_running) {
                                        due = port()->deadline();
                                        if (_followAt < due) due = _followAt;
                                }
                                if (_retryAt < due) due = _retryAt;
                        }
                        unsigned int waitMs = WaitForever;
                        if (due != INT64_MAX) {
                                const int64_t ms = due > now ? (due - now + 999'999) / 1'000'000 : 0;
                                waitMs = ms > 60'000 ? 60'000 : static_cast<unsigned int>(ms);
                        }
                        if (waitMs != 0 || due == INT64_MAX)
                                (void)wake.acquire(due == INT64_MAX ? WaitForever : waitMs);
                        if (settingsPending.exchange(false)) {
                                SysConf::Id id;
                                while (watcher.next(id)) {}
                                (void)watcher.overflowed();
                                const Wanted  w = readSettings();
                                Mutex::Locker l(lock, WaitForever);
                                apply(w, monoNs());
                        }
                        Mutex::Locker l(lock, WaitForever);
                        now = monoNs();
                        if (!_running) {
                                if (_retryAt <= now) apply(_wanted, now);
                                continue;
                        }
                        drain(_event, now);
                        drain(_general, now);
                        if (port()->deadline() <= now) port()->advance(now);
                        noteState(now);
                        if (_followAt <= now) follow(now);
                }
        }

        // --- What it shows ----------------------------------------------------------------

        void Service::fill(PtpService::Status &out) {
                Mutex::Locker l(lock, WaitForever);
                out.enabled = _wanted.enabled;
                out.running = _running;
                out.error = _error;
                std::memcpy(out.interface, _wanted.interface, sizeof(out.interface));
                out.l2 = _wanted.l2;
                out.defaultProfile = _wanted.defaultProfile;
                out.config = _wanted.config;
                out.follow = _follow;
                out.followOffsetNs = _followOffsetNs;
                out.followBracketNs = _followBracketNs;
                out.flows = _closedFlows;
                for (NetFlow *f : {&_event, &_general}) {
                        if (!f->isOpen()) continue;
                        const NetFlow::Counters c = f->counters();
                        out.flows.rxPackets += c.rxPackets;
                        out.flows.rxDropped += c.rxDropped;
                        out.flows.txPackets += c.txPackets;
                        out.flows.txErrors += c.txErrors;
                        out.flows.txStamps += c.txStamps;
                        out.flows.txStampsLost += c.txStampsLost;
                }
                if (!_running) return;
                Ptp::Port *p = port();
                out.stateForNs = monoNs() - _stateAt;
                out.identity = p->identity();
                out.config = p->config();
                out.port = p->status();
                out.counts = p->counts();
                out.port.frequencyPpb = _ptpPpb; // ptpclock0's, whoever set it (a master's follows NTP)
                const Span<const Ptp::Port::Foreign> f = p->foreign();
                out.foreignCount = f.size();
                for (size_t i = 0; i < f.size(); ++i) out.foreign[i] = f[i];
                out.utcOffset = utcOffsetOf(out.port, out.utcFromMaster, out.arbitraryTimescale);
                return;
        }

        void Service::readCounters(Span<int64_t> v) {
                Mutex::Locker l(lock, WaitForever);
                for (int64_t &x : v) x = 0;
                if (!_running) return;
                Ptp::Port               *p = port();
                const Ptp::Port::Status &s = p->status();
                const Ptp::Port::Counts &c = p->counts();
                size_t                   i = 0;
                v[i++] = static_cast<int64_t>(s.state);
                v[i++] = s.offsetNs;
                v[i++] = s.meanPathDelayNs;
                v[i++] = std::llround(_ptpPpb);
                for (Ptp::MessageType t : MessageCounted) v[i++] = c.received[static_cast<size_t>(t)];
                for (Ptp::MessageType t : MessageCounted) v[i++] = c.sent[static_cast<size_t>(t)];
                v[i++] = c.ignored;
                v[i++] = c.sendFailures;
                v[i++] = c.announceTimeouts;
                v[i++] = c.stateChanges;
                v[i++] = c.parentChanges;
                v[i++] = c.steps;
                v[i++] = c.followUpsMissed;
                return;
        }

        void taskEntry(void *) {
                service->run();
        }

        static_assert(CounterCount == 4 + 2 * Counted + 7, "net.ptp's counters and readCounters()");

} // namespace

Error PtpService::init() {
        if (service.create() == nullptr) return Error::NoMem;
        static constexpr const char *Sync = "auto|-7|-6|-5|-4|-3|-2|-1|0|1|2|3|4";
        static constexpr struct {
                        const char  *key;
                        Config::Type type;
                        const char  *defaultValue;
                        const char  *help;
                        int32_t      min;
                        int32_t      max;
                        const char  *choices;
        } Decls[SettingCount] = {
                {"net/ptp/enabled", Config::Type::Bool, "false", "Run PTP (IEEE 1588): follow a master, or be one", 0,
                 0, nullptr},
                {"net/ptp/interface", Config::Type::String, "eth0", "The Ethernet interface PTP runs on", 0, 15,
                 nullptr},
                {"net/ptp/domain", Config::Type::Int, "0", "The PTP domain", 0, 127, nullptr},
                {"net/ptp/profile", Config::Type::Enum, "media", "The intervals 'auto' stands for: AES67's or 1588's",
                 0, 0, "media|default"},
                {"net/ptp/transport", Config::Type::Enum, "udp", "UDP/IPv4 multicast, or Ethernet (l2)", 0, 0,
                 "udp|l2"},
                {"net/ptp/priority1", Config::Type::Int, "128",
                 "The best master clock algorithm's first priority (lower wins)", 0, 255, nullptr},
                {"net/ptp/priority2", Config::Type::Int, "128", "Its second priority (lower wins)", 0, 255, nullptr},
                {"net/ptp/slave-only", Config::Type::Bool, "false", "Never be master", 0, 0, nullptr},
                {"net/ptp/sync-interval", Config::Type::Enum, "auto",
                 "log2 seconds between Syncs as master (auto: media -3, default 0)", 0, 0, Sync},
                {"net/ptp/announce-interval", Config::Type::Enum, "auto", "log2 seconds between Announces (auto: 1)", 0,
                 0, "auto|-3|-2|-1|0|1|2|3|4"},
                {"net/ptp/delay-interval", Config::Type::Enum, "auto",
                 "log2 seconds between slaves' Delay_Reqs, as master (auto: media -3, default 0)", 0, 0,
                 "auto|-7|-6|-5|-4|-3|-2|-1|0|1|2|3|4|5"},
        };
        for (size_t i = 0; i < SettingCount; ++i) {
                Config::Param p;
                p.key = Decls[i].key;
                p.type = Decls[i].type;
                p.defaultValue = Decls[i].defaultValue;
                p.help = Decls[i].help;
                p.min = Decls[i].min;
                p.max = Decls[i].max;
                p.choices = Decls[i].choices;
                auto [id, err] = SysConf::declareKernel(p);
                if (err.isError()) {
                        bmdsErr("ptp: setting %s not declared: %s", Decls[i].key, err.desc());
                        return err;
                }
                service->settings[i] = id;
        }
        service->group.setName("net.ptp");
        (void)Stats::add(service->group); // counters only: the service works without them
        started = true;
        const Error err = service->watcher.start(StringView("net/ptp"), &settingsChanged, nullptr);
        if (err.isError()) return err;
        startTask();
        return Error::Ok;
}

void PtpService::status(Status &out) {
        out = Status{};
        if (!started) return;
        service->fill(out);
        return;
}

bool PtpService::ownsSystemClock() {
        return started && service->owns.load(std::memory_order_relaxed);
}

BMDS_NAMESPACE_END
