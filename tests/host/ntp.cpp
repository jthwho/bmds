/**
 * @file      ntp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * NTP's arithmetic (bmds/ntp.h, devplan/network.md §22): time stamps and eras,
 * the request, the parser against hand-built replies, a sample's offset and
 * delay, the clock filter, and the discipline's steps, slews, frequency and poll.
 */

#include <doctest.h>

#include <cstdint>
#include <cstring>

#include <bmds/ntp.h>

using bmds::Span;
using bmds::Timestamp;
namespace Ntp = bmds::Ntp;

namespace {

        constexpr int64_t Ms = 1'000'000;
        constexpr int64_t S = Timestamp::NsPerSecond;

        void put32(uint8_t *p, uint32_t v) {
                for (int i = 3; i >= 0; --i) {
                        p[i] = static_cast<uint8_t>(v);
                        v >>= 8;
                }
        }

        void put64(uint8_t *p, uint64_t v) {
                put32(p, static_cast<uint32_t>(v >> 32));
                put32(p + 4, static_cast<uint32_t>(v));
        }

        constexpr uint64_t Cookie = 0x0123'4567'89ab'cdefull;

        // A server's good reply to Cookie: stratum 2, received at 1000 s, sent 1 ms later.
        struct Packet {
                        uint8_t bytes[64] = {};

                        Packet() {
                                bytes[0] = (0 << 6) | (4 << 3) | 4;
                                bytes[1] = 2;
                                bytes[2] = 6;
                                bytes[3] = static_cast<uint8_t>(-20);
                                put32(bytes + 4, 0x0000'0800); // root delay 1/32 s
                                put32(bytes + 8, 0x0000'0400); // root dispersion 1/64 s
                                std::memcpy(bytes + 12, "GPS", 3);
                                put64(bytes + 24, Cookie);
                                put64(bytes + 32, Ntp::toNtp(Timestamp::fromSeconds(1'800'000'000)));
                                put64(bytes + 40, Ntp::toNtp(Timestamp{1'800'000'000 * S + Ms}));
                        }

                        Ntp::Verdict parse(size_t size, Ntp::Reply &r, uint64_t cookie = Cookie) const {
                                return Ntp::parse(Span<const uint8_t>(bytes, size), cookie, r);
                        }
        };

} // namespace

TEST_CASE("Ntp: time stamps and eras") {
        CHECK(Ntp::toNtp(Timestamp{}) == uint64_t(Ntp::UnixEpochSeconds) << 32);
        CHECK(Ntp::fromNtp(uint64_t(Ntp::UnixEpochSeconds) << 32).ns == 0);
        const Timestamp t{1'791'720'896 * S + 789'000'123}; // 2026-10-11
        CHECK(Ntp::fromNtp(Ntp::toNtp(t)).ns - t.ns <= 1);
        CHECK(t.ns - Ntp::fromNtp(Ntp::toNtp(t)).ns <= 1);
        // 2036-02-07 06:28:16 UTC wraps the seconds to 0: era 1, read right.
        const Timestamp wrap = Timestamp::fromSeconds(2'085'978'496);
        CHECK((Ntp::toNtp(wrap) >> 32) == 0);
        CHECK(Ntp::fromNtp(Ntp::toNtp(wrap)).ns == wrap.ns);
        CHECK(Ntp::fromNtp(Ntp::toNtp(Timestamp::fromSeconds(4'000'000'000))).ns == 4'000'000'000 * S); // 2096
        // Half a second's fraction; 16.16's.
        CHECK(Ntp::fromNtp((uint64_t(Ntp::UnixEpochSeconds) << 32) | 0x8000'0000u).ns == S / 2);
        CHECK(Ntp::shortToNs(0x0001'0000) == S);
        CHECK(Ntp::shortToNs(0x0000'8000) == S / 2);
        CHECK(Ntp::shortToNs(0xffff'ffff) > 65535 * S);
}

TEST_CASE("Ntp: the request") {
        uint8_t p[Ntp::PacketBytes];
        std::memset(p, 0xaa, sizeof(p));
        Ntp::buildRequest(p, Cookie);
        CHECK(p[0] == 0x23); // leap 0, version 4, mode 3
        for (size_t i = 1; i < 40; ++i) CHECK(p[i] == 0);
        const uint8_t cookie[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
        CHECK(std::memcmp(p + 40, cookie, 8) == 0);
}

TEST_CASE("Ntp: replies") {
        Packet     good;
        Ntp::Reply r;
        REQUIRE(good.parse(48, r) == Ntp::Verdict::Ok);
        CHECK(r.version == 4);
        CHECK(r.stratum == 2);
        CHECK(r.poll == 6);
        CHECK(r.precision == -20);
        CHECK(r.rootDelayNs == S / 32);
        CHECK(r.rootDispersionNs == S / 64);
        CHECK(r.referenceId == 0x4750'5300);
        CHECK(r.receive.ns == 1'800'000'000 * S);
        CHECK(r.transmit.ns - r.receive.ns >= Ms - 1);
        CHECK(r.transmit.ns - r.receive.ns <= Ms + 1);
        CHECK(good.parse(64, r) == Ntp::Verdict::Ok); // extensions after it
        CHECK(good.parse(47, r) == Ntp::Verdict::Short);
        CHECK(good.parse(48, r, Cookie + 1) == Ntp::Verdict::NotOurs);

        Packet p;
        p.bytes[0] = (4 << 3) | 3;
        CHECK(p.parse(48, r) == Ntp::Verdict::NotServer);
        p.bytes[0] = (0 << 3) | 4;
        CHECK(p.parse(48, r) == Ntp::Verdict::Version);
        p.bytes[0] = (5 << 3) | 4;
        CHECK(p.parse(48, r) == Ntp::Verdict::Version);
        p.bytes[0] = (3 << 3) | 4; // version 3 is fine
        CHECK(p.parse(48, r) == Ntp::Verdict::Ok);
        p.bytes[0] = (3 << 6) | (4 << 3) | 4;
        CHECK(p.parse(48, r) == Ntp::Verdict::Unsynchronised);

        p = Packet();
        p.bytes[1] = 16;
        CHECK(p.parse(48, r) == Ntp::Verdict::Unsynchronised);
        p.bytes[1] = 0;
        std::memcpy(p.bytes + 12, "RATE", 4);
        CHECK(p.parse(48, r) == Ntp::Verdict::Kiss);
        CHECK(std::strcmp(r.kiss, "RATE") == 0);
        p.bytes[12] = 0x01;
        CHECK(p.parse(48, r) == Ntp::Verdict::Kiss);
        CHECK(std::strcmp(r.kiss, "?ATE") == 0);

        p = Packet();
        put64(p.bytes + 32, 0);
        CHECK(p.parse(48, r) == Ntp::Verdict::Bogus);
        p = Packet();
        put64(p.bytes + 40, 0);
        CHECK(p.parse(48, r) == Ntp::Verdict::Bogus);
        p = Packet();
        put32(p.bytes + 4, 17u << 16);
        CHECK(p.parse(48, r) == Ntp::Verdict::Bogus);
        p = Packet();
        put32(p.bytes + 8, 17u << 16);
        CHECK(p.parse(48, r) == Ntp::Verdict::Bogus);

        CHECK(std::strcmp(Ntp::verdictName(Ntp::Verdict::Kiss), "kiss") == 0);
}

TEST_CASE("Ntp: a sample's offset and delay") {
        // The server is 0.5 s ahead; 10 ms each way; it holds the request 1 ms.
        const Timestamp   t1{100 * S};
        const Timestamp   t2{t1.ns + S / 2 + 10 * Ms};
        const Timestamp   t3{t2.ns + Ms};
        const Timestamp   t4{t1.ns + 21 * Ms};
        const Ntp::Sample s = Ntp::sampleOf(t1, t2, t3, t4, 7);
        CHECK(s.offsetNs == S / 2);
        CHECK(s.delayNs == 20 * Ms);
        CHECK(s.monoNs == 7);
        // An unset clock 70 years behind: the offset holds it.
        const Timestamp   old{-450'000'000 * S};
        const Timestamp   now{1'791'720'896 * S};
        const Ntp::Sample far = Ntp::sampleOf(old, now, now, Timestamp{old.ns + Ms}, 0);
        CHECK(far.offsetNs == now.ns - old.ns - Ms / 2);
        // A server's time running backwards: no negative delay.
        CHECK(Ntp::sampleOf(t1, t3, t2, t4, 0).delayNs >= 0);
        CHECK(Ntp::sampleOf(t1, t2, Timestamp{t3.ns + S}, t4, 0).delayNs == 0);
}

TEST_CASE("Ntp: the clock filter") {
        Ntp::Filter f;
        CHECK(f.count() == 0);
        CHECK(f.best(0).delayNs == 0);
        CHECK_FALSE(f.newestIsBest(0));
        CHECK(f.jitterNs(0) == 0);
        f.add({0, 30 * Ms, 1 * S});
        CHECK(f.newestIsBest(1 * S));
        f.add({3 * Ms, 10 * Ms, 2 * S});
        f.add({-4 * Ms, 20 * Ms, 3 * S});
        CHECK(f.count() == 3);
        CHECK(f.best(3 * S).offsetNs == 3 * Ms);
        CHECK_FALSE(f.newestIsBest(3 * S));
        CHECK(f.newest().offsetNs == -4 * Ms);
        // Offsets 0 and -4 ms from the best's 3 ms: sqrt((9 + 49) / 2) ms.
        CHECK(f.jitterNs(3 * S) / 1000 == 5385);
        f.add({1 * Ms, 5 * Ms, 4 * S});
        CHECK(f.newestIsBest(4 * S));

        // An old best gives way: 15 ppm of 1000 s is 15 ms.
        Ntp::Filter a;
        a.add({0, 10 * Ms, 0});
        a.add({Ms, 20 * Ms, 1000 * S});
        CHECK(a.best(1000 * S).offsetNs == Ms);
        CHECK(a.best(1 * S).offsetNs == 0); // 1 s on, the old one is still the better
        Ntp::Filter b;
        b.add({0, 10 * Ms, 0});
        b.add({Ms, 20 * Ms, 100 * S}); // 1.5 ms of age: the old one still wins
        CHECK(b.best(100 * S).offsetNs == 0);

        // Full: the oldest gives way.
        Ntp::Filter c;
        c.add({42, 1, 0});
        for (int i = 0; i < int(Ntp::Filter::Depth); ++i) c.add({i, 10 * Ms, 0});
        CHECK(c.count() == Ntp::Filter::Depth);
        CHECK(c.best(0).offsetNs != 42);
        c.clear();
        CHECK(c.count() == 0);

        // A stepped sample's 70 years doesn't overflow the jitter.
        Ntp::Filter d;
        d.add({INT64_MAX / 2, 10 * Ms, 0});
        d.add({INT64_MIN / 2, 1 * Ms, 0});
        d.add({0, 20 * Ms, 0});
        CHECK(d.jitterNs(0) > 0);
}

TEST_CASE("Ntp: the discipline") {
        using D = Ntp::Discipline;
        D d;
        CHECK(d.poll() == D::MinPoll);
        // Unset: a step, whatever the offset.
        D::Action a = d.update(0, 5 * Ms, false, 0, 0, 0);
        CHECK(a.step);
        CHECK(a.offsetNs == 5 * Ms);
        // A burst's next, 2 s on: a slew, no frequency yet.
        a = d.update(2 * S, 2 * Ms, true, 0, 0, 0);
        CHECK_FALSE(a.step);
        CHECK(a.offsetNs == 2 * Ms);
        CHECK_FALSE(a.setFrequency);
        // 64 s on, 1 ms built up: 15625 ppb, taken in by 64 / (64 + 1024).
        a = d.update(66 * S, 1 * Ms, true, 0, 0, 0);
        CHECK(a.setFrequency);
        CHECK(a.frequencyPpb == 15625 * 64 / 1088);
        // The slew still pending isn't the rate's.
        a = d.update(130 * S, 3 * Ms, true, 15625, 2500 * 1000, 0);
        CHECK(a.setFrequency);
        CHECK(a.frequencyPpb == 15625 + 7812 * 64 / 1088);
        // A long interval takes half.
        a = d.update(130 * S + 1024 * S, 1 * Ms, true, 0, 0, 0);
        CHECK(a.frequencyPpb == 976 / 2);
        a = d.update(130 * S + 5024 * S, 4 * Ms, true, 0, 0, 0);
        CHECK(a.frequencyPpb == 1000 / 2); // and no more, however long
        // Within ±500 ppm.
        a = d.update(6000 * S, 100 * Ms, true, 499'000, 0, 0);
        CHECK(a.frequencyPpb == D::MaxFrequencyPpb);
        a = d.update(6064 * S, -100 * Ms, true, -499'000, 0, 0);
        CHECK(a.frequencyPpb == -D::MaxFrequencyPpb);
        // Beyond 128 ms: a step, and the poll back to 64 s.
        a = d.update(6128 * S, -200 * Ms, true, 0, 0, 0);
        CHECK(a.step);
        CHECK(d.poll() == D::MinPoll);

        // Stable updates double the poll, up to 1024 s; a large offset brings it back.
        D p;
        (void)p.update(0, 0, false, 0, 0, 0);
        int64_t t = 0;
        for (unsigned int i = 0; i < 4 * D::StableUpdates * 2; ++i) (void)p.update(t += 64 * S, Ms / 2, true, 0, 0, 0);
        CHECK(p.poll() == D::MaxPoll);
        (void)p.update(t += 64 * S, 3 * Ms, true, 0, 0, 0); // beyond 1 ms (jitter 0)
        CHECK(p.poll() == D::MinPoll);
        for (unsigned int i = 0; i < D::StableUpdates; ++i) (void)p.update(t += 64 * S, 3 * Ms, true, 0, 0, Ms);
        CHECK(p.poll() == D::MinPoll + 1); // within 4 jitters of 1 ms
        p.slowDown();
        CHECK(p.poll() == D::MinPoll + 2);
        p.reset();
        CHECK(p.poll() == D::MinPoll);
        CHECK(p.update(0, Ms, true, 0, 0, 0).step == false);
}
