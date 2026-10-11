/**
 * @file      ntp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * NTP's arithmetic (ntp.h, devplan/network.md §22): portable, host-tested and
 * fuzzed.
 */

#include <cstring>
#include <bmds/ntp.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr int64_t NsPerSecond = Timestamp::NsPerSecond;

        // The packet's fields (RFC 5905 §7.3).
        constexpr size_t RootDelayAt = 4;
        constexpr size_t RootDispersionAt = 8;
        constexpr size_t ReferenceIdAt = 12;
        constexpr size_t OriginAt = 24;
        constexpr size_t ReceiveAt = 32;
        constexpr size_t TransmitAt = 40;

        constexpr uint8_t ModeClient = 3;
        constexpr uint8_t ModeServer = 4;
        constexpr uint8_t Version = 4;
        constexpr uint8_t LeapUnsynchronised = 3;
        constexpr uint8_t MaxStratum = 15;

        // Offsets beyond this count as this in the jitter (a stepped sample's 70 years would overflow).
        constexpr int64_t JitterClampNs = NsPerSecond;

        uint32_t be32(const uint8_t *p) {
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        uint64_t be64(const uint8_t *p) {
                return (uint64_t(be32(p)) << 32) | be32(p + 4);
        }

        void putBe64(uint8_t *p, uint64_t v) {
                for (int i = 7; i >= 0; --i) {
                        p[i] = static_cast<uint8_t>(v);
                        v >>= 8;
                }
                return;
        }

        uint64_t magnitude(int64_t v) {
                return v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        }

        int64_t clamp(int64_t v, int64_t bound) {
                return v > bound ? bound : v < -bound ? -bound : v;
        }

        uint64_t squareRoot(uint64_t v) {
                uint64_t r = 0;
                for (uint64_t bit = uint64_t(1) << 62; bit != 0; bit >>= 2) {
                        if (v >= r + bit) {
                                v -= r + bit;
                                r = (r >> 1) + bit;
                        } else {
                                r >>= 1;
                        }
                }
                return r;
        }

} // namespace

uint64_t Ntp::toNtp(Timestamp t) {
        const uint32_t seconds = static_cast<uint32_t>(t.seconds() + UnixEpochSeconds);
        const uint64_t fraction = (uint64_t(t.nanoseconds()) << 32) / NsPerSecond;
        return (uint64_t(seconds) << 32) | fraction;
}

Timestamp Ntp::fromNtp(uint64_t v) {
        const uint32_t s = static_cast<uint32_t>(v >> 32);
        int64_t        seconds = s;
        if (s < 0x8000'0000u) seconds += int64_t(1) << 32; // era 1: from 2036
        const uint64_t ns = ((v & 0xffff'ffffu) * uint64_t(NsPerSecond) + (uint64_t(1) << 31)) >> 32;
        return Timestamp{(seconds - UnixEpochSeconds) * NsPerSecond + static_cast<int64_t>(ns)};
}

int64_t Ntp::shortToNs(uint32_t v) {
        return int64_t(v >> 16) * NsPerSecond + static_cast<int64_t>((uint64_t(v & 0xffffu) * NsPerSecond) >> 16);
}

void Ntp::buildRequest(uint8_t (&packet)[PacketBytes], uint64_t cookie) {
        std::memset(packet, 0, sizeof(packet));
        packet[0] = static_cast<uint8_t>((Version << 3) | ModeClient);
        putBe64(packet + TransmitAt, cookie);
        return;
}

const char *Ntp::verdictName(Verdict v) {
        switch (v) {
                case Verdict::Ok: return "ok";
                case Verdict::Short: return "short";
                case Verdict::NotServer: return "not a server's";
                case Verdict::Version: return "version";
                case Verdict::NotOurs: return "not ours";
                case Verdict::Kiss: return "kiss";
                case Verdict::Unsynchronised: return "unsynchronised";
                case Verdict::Bogus: return "bogus";
        }
        return "?";
}

Ntp::Verdict Ntp::parse(Span<const uint8_t> packet, uint64_t cookie, Reply &reply) {
        reply = Reply{};
        if (packet.size() < PacketBytes) return Verdict::Short;
        const uint8_t *p = packet.data();
        reply.leap = p[0] >> 6;
        reply.version = (p[0] >> 3) & 7;
        if ((p[0] & 7) != ModeServer) return Verdict::NotServer;
        if (reply.version == 0 || reply.version > Version) return Verdict::Version;
        if (be64(p + OriginAt) != cookie) return Verdict::NotOurs;
        reply.stratum = p[1];
        reply.poll = static_cast<int8_t>(p[2]);
        reply.precision = static_cast<int8_t>(p[3]);
        reply.rootDelayNs = shortToNs(be32(p + RootDelayAt));
        reply.rootDispersionNs = shortToNs(be32(p + RootDispersionAt));
        reply.referenceId = be32(p + ReferenceIdAt);
        if (reply.stratum == 0) {
                for (size_t i = 0; i < 4; ++i) {
                        const uint8_t c = p[ReferenceIdAt + i];
                        reply.kiss[i] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
                }
                return Verdict::Kiss;
        }
        if (reply.leap == LeapUnsynchronised || reply.stratum > MaxStratum) return Verdict::Unsynchronised;
        const uint64_t receive = be64(p + ReceiveAt);
        const uint64_t transmit = be64(p + TransmitAt);
        if (receive == 0 || transmit == 0 || reply.rootDelayNs > MaxRootNs || reply.rootDispersionNs > MaxRootNs)
                return Verdict::Bogus;
        reply.receive = fromNtp(receive);
        reply.transmit = fromNtp(transmit);
        return Verdict::Ok;
}

Ntp::Sample Ntp::sampleOf(Timestamp t1, Timestamp t2, Timestamp t3, Timestamp t4, int64_t monoNs) {
        Sample s;
        s.offsetNs = (t2.ns - t1.ns) / 2 + (t3.ns - t4.ns) / 2;
        s.delayNs = (t4.ns - t1.ns) - (t3.ns - t2.ns);
        if (s.delayNs < 0) s.delayNs = 0;
        s.monoNs = monoNs;
        return s;
}

// --- Filter -------------------------------------------------------------------------------------

void Ntp::Filter::add(const Sample &s) {
        _samples[_next] = s;
        _next = (_next + 1) % Depth;
        if (_count < Depth) ++_count;
        return;
}

void Ntp::Filter::clear() {
        _count = 0;
        _next = 0;
        return;
}

Ntp::Sample Ntp::Filter::newest() const {
        return _count == 0 ? Sample{} : _samples[newestIndex()];
}

size_t Ntp::Filter::bestIndex(int64_t nowNs) const {
        size_t   best = newestIndex();
        uint64_t bestScore = UINT64_MAX;
        for (size_t k = 0; k < _count; ++k) { // newest first: a tie keeps the newer
                const size_t   i = (_next + Depth - 1 - k) % Depth;
                const int64_t  age = nowNs > _samples[i].monoNs ? nowNs - _samples[i].monoNs : 0;
                const uint64_t score = uint64_t(_samples[i].delayNs) + uint64_t(age / 1000) * AgingPpm / 1000;
                if (score < bestScore) {
                        bestScore = score;
                        best = i;
                }
        }
        return best;
}

Ntp::Sample Ntp::Filter::best(int64_t nowNs) const {
        return _count == 0 ? Sample{} : _samples[bestIndex(nowNs)];
}

bool Ntp::Filter::newestIsBest(int64_t nowNs) const {
        return _count != 0 && bestIndex(nowNs) == newestIndex();
}

int64_t Ntp::Filter::jitterNs(int64_t nowNs) const {
        if (_count < 2) return 0;
        const size_t  b = bestIndex(nowNs);
        const int64_t bestOffset = clamp(_samples[b].offsetNs, JitterClampNs);
        uint64_t      sum = 0;
        for (size_t k = 0; k < _count; ++k) {
                const size_t i = (_next + Depth - 1 - k) % Depth;
                if (i == b) continue;
                const uint64_t d = magnitude(clamp(_samples[i].offsetNs, JitterClampNs) - bestOffset);
                sum += d * d / (_count - 1);
        }
        return static_cast<int64_t>(squareRoot(sum));
}

// --- Discipline ---------------------------------------------------------------------------------

Ntp::Discipline::Action Ntp::Discipline::update(int64_t monoNs, int64_t offsetNs, bool clockSet, int32_t frequencyPpb,
                                                int64_t slewRemainingNs, int64_t jitterNs) {
        Action a;
        a.offsetNs = offsetNs;
        if (!clockSet || magnitude(offsetNs) > uint64_t(StepNs)) {
                a.step = true;
                _lastNs = monoNs;
                _haveLast = true;
                _poll = MinPoll;
                _stable = 0;
                return a;
        }
        if (_haveLast && monoNs - _lastNs >= FrequencyIntervalNs) {
                // The offset the pending slew doesn't explain built up since the last update.
                // Weighed by the interval: a short one's error is mostly the offsets' noise.
                const int64_t interval = monoNs - _lastNs;
                const int64_t residual = clamp(offsetNs - slewRemainingNs, 2 * StepNs);
                const int64_t errorPpb = residual * NsPerSecond / interval;
                const int64_t weighed = interval < FrequencyTimeNs ? interval : FrequencyTimeNs; // no overflow
                const int64_t change = errorPpb * (weighed / 1'000'000) / ((weighed + FrequencyTimeNs) / 1'000'000);
                a.setFrequency = true;
                a.frequencyPpb = static_cast<int32_t>(clamp(frequencyPpb + change, MaxFrequencyPpb));
        }
        _lastNs = monoNs;
        _haveLast = true;
        const int64_t bound = 4 * jitterNs > StableNs ? 4 * jitterNs : StableNs;
        if (magnitude(offsetNs) > uint64_t(bound)) {
                _poll = MinPoll;
                _stable = 0;
        } else if (++_stable >= StableUpdates) {
                _stable = 0;
                if (_poll < MaxPoll) ++_poll;
        }
        return a;
}

void Ntp::Discipline::reset() {
        *this = Discipline();
        return;
}

void Ntp::Discipline::slowDown() {
        if (_poll < MaxPoll) ++_poll;
        return;
}

BMDS_NAMESPACE_END
