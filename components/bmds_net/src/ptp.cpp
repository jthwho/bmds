/**
 * @file      ptp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * PTP's messages, the dataset comparison algorithm and the servo (devplan/network.md §31).
 * The port's state machine is in ptpport.cpp.
 */

#include <cmath>
#include <cstring>
#include <bmds/ptp.h>

BMDS_NAMESPACE_BEGIN

namespace Ptp {

        namespace {

                constexpr size_t  HeaderBytes = 34;
                constexpr size_t  TimedBytes = 44; // Sync, Delay_Req, Follow_Up
                constexpr size_t  DelayRespBytes = 54;
                constexpr size_t  AnnounceBytes = 64;
                constexpr int64_t NsPerSecond = 1'000'000'000;
                constexpr int64_t MaxSeconds = INT64_MAX / NsPerSecond - 1; // 2262

                uint16_t get16(const uint8_t *p) {
                        return static_cast<uint16_t>((p[0] << 8) | p[1]);
                }

                uint32_t get32(const uint8_t *p) {
                        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
                }

                uint64_t get48(const uint8_t *p) {
                        return (uint64_t(get16(p)) << 32) | get32(p + 2);
                }

                void put16(uint8_t *p, uint16_t v) {
                        p[0] = static_cast<uint8_t>(v >> 8);
                        p[1] = static_cast<uint8_t>(v);
                        return;
                }

                void put32(uint8_t *p, uint32_t v) {
                        put16(p, static_cast<uint16_t>(v >> 16));
                        put16(p + 2, static_cast<uint16_t>(v));
                        return;
                }

                void put48(uint8_t *p, uint64_t v) {
                        put16(p, static_cast<uint16_t>(v >> 32));
                        put32(p + 2, static_cast<uint32_t>(v));
                        return;
                }

                void putPort(uint8_t *p, const PortIdentity &id) {
                        std::memcpy(p, id.clock.bytes, 8);
                        put16(p + 8, id.port);
                        return;
                }

                PortIdentity getPort(const uint8_t *p) {
                        PortIdentity id;
                        std::memcpy(id.clock.bytes, p, 8);
                        id.port = get16(p + 8);
                        return id;
                }

                // A time stamp: 48 bits of seconds, 32 of nanoseconds. Times before the epoch go as 0.
                void putTime(uint8_t *p, int64_t ns) {
                        if (ns < 0) ns = 0;
                        put48(p, static_cast<uint64_t>(ns / NsPerSecond));
                        put32(p + 6, static_cast<uint32_t>(ns % NsPerSecond));
                        return;
                }

                bool getTime(const uint8_t *p, int64_t &ns) {
                        const uint64_t s = get48(p);
                        const uint32_t n = get32(p + 6);
                        if (n >= NsPerSecond || s > static_cast<uint64_t>(MaxSeconds)) return false;
                        ns = static_cast<int64_t>(s) * NsPerSecond + n;
                        return true;
                }

                size_t bodyBytes(MessageType t) {
                        switch (t) {
                                case MessageType::Sync:
                                case MessageType::DelayReq:
                                case MessageType::FollowUp: return TimedBytes;
                                case MessageType::DelayResp: return DelayRespBytes;
                                case MessageType::Announce: return AnnounceBytes;
                                default: return 0;
                        }
                }

                uint8_t controlOf(MessageType t) {
                        switch (t) {
                                case MessageType::Sync: return 0;
                                case MessageType::DelayReq: return 1;
                                case MessageType::FollowUp: return 2;
                                case MessageType::DelayResp: return 3;
                                case MessageType::Management: return 4;
                                default: return 5;
                        }
                }

                template <typename T> int order(T a, T b) {
                        return a < b ? -1 : a > b ? 1 : 0;
                }

        } // namespace

        int64_t intervalNs(int8_t log) {
                if (log < -7) log = -7;
                if (log > 7) log = 7;
                return log >= 0 ? NsPerSecond << log : NsPerSecond >> -log;
        }

        ClockIdentity ClockIdentity::fromMac(const MacAddress &mac) {
                ClockIdentity id;
                id.bytes[0] = mac.bytes[0];
                id.bytes[1] = mac.bytes[1];
                id.bytes[2] = mac.bytes[2];
                id.bytes[3] = 0xff;
                id.bytes[4] = 0xfe;
                id.bytes[5] = mac.bytes[3];
                id.bytes[6] = mac.bytes[4];
                id.bytes[7] = mac.bytes[5];
                return id;
        }

        bool ClockIdentity::isZero() const {
                for (uint8_t b : bytes)
                        if (b != 0) return false;
                return true;
        }

        int compare(const ClockIdentity &a, const ClockIdentity &b) {
                const int c = std::memcmp(a.bytes, b.bytes, sizeof(a.bytes));
                return c < 0 ? -1 : c > 0 ? 1 : 0;
        }

        bool operator==(const ClockIdentity &a, const ClockIdentity &b) {
                return compare(a, b) == 0;
        }

        int compare(const PortIdentity &a, const PortIdentity &b) {
                const int c = compare(a.clock, b.clock);
                return c != 0 ? c : order(a.port, b.port);
        }

        bool operator==(const PortIdentity &a, const PortIdentity &b) {
                return compare(a, b) == 0;
        }

        int64_t correctionNs(const Header &header) {
                return header.correction >> 16; // floor: the fraction of a nanosecond dropped
        }

        size_t build(const Message &m, Span<uint8_t> out) {
                const size_t n = bodyBytes(m.header.type);
                if (n == 0 || out.size() < n) return 0;
                uint8_t *p = out.data();
                std::memset(p, 0, n);
                const Header &h = m.header;
                p[0] = static_cast<uint8_t>((h.transportSpecific << 4) | (static_cast<uint8_t>(h.type) & 0x0f));
                p[1] = static_cast<uint8_t>(h.version & 0x0f);
                put16(p + 2, static_cast<uint16_t>(n));
                p[4] = h.domain;
                put16(p + 6, h.flags);
                put32(p + 8, static_cast<uint32_t>(static_cast<uint64_t>(h.correction) >> 32));
                put32(p + 12, static_cast<uint32_t>(h.correction));
                putPort(p + 20, h.source);
                put16(p + 30, h.sequence);
                p[32] = controlOf(h.type);
                p[33] = static_cast<uint8_t>(h.logInterval);
                putTime(p + 34, m.timeNs);
                if (h.type == MessageType::DelayResp) putPort(p + 44, m.requesting);
                if (h.type == MessageType::Announce) {
                        put16(p + 44, static_cast<uint16_t>(m.utcOffset));
                        p[47] = m.priority1;
                        p[48] = m.quality.clockClass;
                        p[49] = m.quality.accuracy;
                        put16(p + 50, m.quality.variance);
                        p[52] = m.priority2;
                        std::memcpy(p + 53, m.grandmaster.bytes, 8);
                        put16(p + 61, m.stepsRemoved);
                        p[63] = m.timeSource;
                }
                return n;
        }

        const char *verdictName(Verdict v) {
                switch (v) {
                        case Verdict::Ok: return "ok";
                        case Verdict::Short: return "short";
                        case Verdict::Version: return "version";
                        case Verdict::Unsupported: return "unsupported";
                        case Verdict::Bogus: return "bogus";
                }
                return "?";
        }

        const char *typeName(MessageType t) {
                switch (t) {
                        case MessageType::Sync: return "sync";
                        case MessageType::DelayReq: return "delay_req";
                        case MessageType::PdelayReq: return "pdelay_req";
                        case MessageType::PdelayResp: return "pdelay_resp";
                        case MessageType::FollowUp: return "follow_up";
                        case MessageType::DelayResp: return "delay_resp";
                        case MessageType::PdelayRespFollowUp: return "pdelay_resp_follow_up";
                        case MessageType::Announce: return "announce";
                        case MessageType::Signaling: return "signaling";
                        case MessageType::Management: return "management";
                }
                return "?";
        }

        Verdict parse(Span<const uint8_t> bytes, Message &m) {
                m = Message{};
                if (bytes.size() < HeaderBytes) return Verdict::Short;
                const uint8_t *p = bytes.data();
                Header        &h = m.header;
                h.type = static_cast<MessageType>(p[0] & 0x0f);
                h.transportSpecific = static_cast<uint8_t>(p[0] >> 4);
                h.version = static_cast<uint8_t>(p[1] & 0x0f); // the high nibble: 1588-2019's minor version
                h.length = get16(p + 2);
                h.domain = p[4];
                h.flags = get16(p + 6);
                h.correction = static_cast<int64_t>((uint64_t(get32(p + 8)) << 32) | get32(p + 12));
                h.source = getPort(p + 20);
                h.sequence = get16(p + 30);
                h.logInterval = static_cast<int8_t>(p[33]);
                if (h.version != 2) return Verdict::Version;
                if (h.length < HeaderBytes || h.length > bytes.size()) return Verdict::Short;
                const size_t n = bodyBytes(h.type);
                if (n == 0) return Verdict::Unsupported;
                if (h.length < n) return Verdict::Short;
                if (!getTime(p + 34, m.timeNs)) return Verdict::Bogus;
                if (h.type == MessageType::DelayResp) m.requesting = getPort(p + 44);
                if (h.type == MessageType::Announce) {
                        m.utcOffset = static_cast<int16_t>(get16(p + 44));
                        m.priority1 = p[47];
                        m.quality.clockClass = p[48];
                        m.quality.accuracy = p[49];
                        m.quality.variance = get16(p + 50);
                        m.priority2 = p[52];
                        std::memcpy(m.grandmaster.bytes, p + 53, 8);
                        m.stepsRemoved = get16(p + 61);
                        m.timeSource = p[63];
                }
                return Verdict::Ok;
        }

        Dataset datasetOf(const Message &m, const PortIdentity &receiver) {
                Dataset d;
                d.priority1 = m.priority1;
                d.quality = m.quality;
                d.priority2 = m.priority2;
                d.grandmaster = m.grandmaster;
                d.stepsRemoved = m.stepsRemoved;
                d.sender = m.header.source;
                d.receiver = receiver;
                return d;
        }

        // IEEE 1588-2008 figures 27 and 28.
        int compare(const Dataset &a, const Dataset &b) {
                if (!(a.grandmaster == b.grandmaster)) {
                        int c = order(a.priority1, b.priority1);
                        if (c == 0) c = order(a.quality.clockClass, b.quality.clockClass);
                        if (c == 0) c = order(a.quality.accuracy, b.quality.accuracy);
                        if (c == 0) c = order(a.quality.variance, b.quality.variance);
                        if (c == 0) c = order(a.priority2, b.priority2);
                        if (c == 0) c = compare(a.grandmaster, b.grandmaster);
                        return 2 * c;
                }
                // The same grandmaster: the shorter path, then the topology.
                const int sa = a.stepsRemoved, sb = b.stepsRemoved;
                if (sa > sb + 1) return 2;
                if (sa + 1 < sb) return -2;
                if (sa > sb) {
                        const int c = compare(a.receiver, a.sender);
                        return c < 0 ? 2 : c > 0 ? 1 : 0;
                }
                if (sa < sb) {
                        const int c = compare(b.receiver, b.sender);
                        return c < 0 ? -2 : c > 0 ? -1 : 0;
                }
                int c = compare(a.sender, b.sender);
                if (c == 0) c = order(a.receiver.port, b.receiver.port);
                return c;
        }

        // linuxptp's pi.c: its default gains for hardware stamps, scaled by the sync interval.
        void Servo::configure(const Config &config, int8_t logSyncInterval) {
                _config = config;
                const double s = static_cast<double>(intervalNs(logSyncInterval)) / 1e9;
                _kp = std::fmin(0.7 * std::pow(s, -0.3), 0.7 / s);
                _ki = std::fmin(0.3 * std::pow(s, 0.4), 0.3 / s);
                return;
        }

        void Servo::reset(double frequencyPpb) {
                _drift = -frequencyPpb; // linuxptp's sign inside: the correction is -drift
                _count = 0;
                _state = State::Unlocked;
                return;
        }

        double Servo::sample(int64_t offsetNs, int64_t localNs) {
                const double max = _config.maxFrequencyPpb;
                const double offset = static_cast<double>(offsetNs);
                const double magnitude = std::fabs(offset);
                double       ppb = _drift;
                switch (_count) {
                        case 0:
                                _offset0 = offsetNs;
                                _local0 = localNs;
                                _count = 1;
                                _state = State::Unlocked;
                                break;
                        case 1:
                                if (localNs <= _local0) {
                                        _count = 0; // time went backwards: start again
                                        _state = State::Unlocked;
                                        break;
                                }
                                _drift += (offset - static_cast<double>(_offset0)) * 1e9 /
                                          (static_cast<double>(localNs) - static_cast<double>(_local0));
                                _drift = std::fmax(-max, std::fmin(max, _drift));
                                if ((_firstUpdate && _config.firstStepNs != 0 &&
                                     magnitude > static_cast<double>(_config.firstStepNs)) ||
                                    (_config.stepNs != 0 && magnitude > static_cast<double>(_config.stepNs))) {
                                        _state = State::Jump;
                                } else {
                                        _state = State::Locked;
                                }
                                ppb = _drift;
                                _count = 2;
                                break;
                        default: {
                                if (_config.stepNs != 0 && magnitude > static_cast<double>(_config.stepNs)) {
                                        _count = 0;
                                        _state = State::Unlocked;
                                        break;
                                }
                                const double kiTerm = _ki * offset;
                                ppb = _kp * offset + _drift + kiTerm;
                                if (ppb < -max) {
                                        ppb = -max;
                                } else if (ppb > max) {
                                        ppb = max;
                                } else {
                                        _drift += kiTerm;
                                }
                                _state = State::Locked;
                                break;
                        }
                }
                if (_state != State::Unlocked) _firstUpdate = false;
                return -ppb;
        }

} // namespace Ptp

BMDS_NAMESPACE_END
