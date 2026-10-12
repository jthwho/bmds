/**
 * @file      icmpratelimit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ICMP's rate limits (devplan/network.md §30g): Linux's per-destination
 * burst and interval, the overall credit, the types limited, and the frames
 * read from the wire.
 */

#include <doctest.h>

#include <cstring>

#include <bmds/icmpratelimit.h>

using bmds::IcmpRateLimit;
using bmds::Span;

namespace {

        constexpr uint32_t Peer = 0xac140009; // 172.20.0.9

        // An Ethernet frame with an IPv4 ICMP message of @p type and @p code to @p destination.
        size_t icmpFrame(uint8_t *f, uint8_t type, uint8_t code, uint32_t destination) {
                std::memset(f, 0, 64);
                f[12] = 0x08;
                f[14] = 0x45;
                f[23] = 1;
                f[30] = static_cast<uint8_t>(destination >> 24);
                f[31] = static_cast<uint8_t>(destination >> 16);
                f[32] = static_cast<uint8_t>(destination >> 8);
                f[33] = static_cast<uint8_t>(destination);
                f[34] = type;
                f[35] = code;
                return 64;
        }

} // namespace

TEST_CASE("IcmpRateLimit: one destination gets a burst of 6, then one a second") {
        IcmpRateLimit l;
        int64_t       t = 5'000'000;
        for (int i = 0; i < 6; ++i) CHECK(l.allowError(3, 3, Peer, t));
        CHECK_FALSE(l.allowError(3, 3, Peer, t));
        CHECK_FALSE(l.allowError(11, 0, Peer, t + 999'999));
        CHECK(l.allowError(3, 3, Peer, t + 1'000'000));
        CHECK_FALSE(l.allowError(3, 3, Peer, t + 1'500'000));
        CHECK(l.allowError(3, 3, Peer, t + 2'000'000));
        CHECK(l.limited() == 3);
        // Another destination has its own burst.
        CHECK(l.allowError(3, 3, Peer + 1, t + 2'000'000));
}

TEST_CASE("IcmpRateLimit: the types Linux limits, and those it doesn't") {
        IcmpRateLimit l;
        for (int i = 0; i < 100; ++i) {
                CHECK(l.allowError(0, 0, Peer, 0));  // echo reply
                CHECK(l.allowError(3, 4, Peer, 0));  // fragmentation needed
                CHECK(l.allowError(5, 0, Peer, 0));  // redirect: not in the mask
                CHECK(l.allowError(40, 0, Peer, 0)); // past the mask
        }
        CHECK(l.limited() == 0);
        int allowed = 0;
        for (const uint8_t type : {3, 4, 11, 12})
                for (int i = 0; i < 10; ++i) allowed += l.allowError(type, 0, Peer, 0) ? 1 : 0;
        CHECK(allowed == 6);
}

TEST_CASE("IcmpRateLimit: 50 at once to everyone, then 1000 a second") {
        IcmpRateLimit l;
        int           allowed = 0;
        // Each destination new: only the overall credit holds them back.
        for (uint32_t d = 0; d < 200; ++d) allowed += l.allowError(3, 3, Peer + d, 0) ? 1 : 0;
        CHECK(allowed == IcmpRateLimit::Burst);
        CHECK_FALSE(l.allowError(3, 3, Peer + 500, 999));
        CHECK(l.allowError(3, 3, Peer + 501, 1000));
        CHECK_FALSE(l.allowError(3, 3, Peer + 502, 1000));
        allowed = 0;
        for (uint32_t d = 0; d < 200; ++d) allowed += l.allowError(3, 3, Peer + 1000 + d, 1'000'000) ? 1 : 0;
        CHECK(allowed == IcmpRateLimit::Burst);
}

TEST_CASE("IcmpRateLimit: frames read from the wire") {
        IcmpRateLimit l;
        uint8_t       f[64];
        const size_t  n = icmpFrame(f, 3, 3, Peer);
        for (int i = 0; i < 6; ++i) CHECK(l.allow(Span<const uint8_t>(f, n), 0));
        CHECK_FALSE(l.allow(Span<const uint8_t>(f, n), 0));
        // Not ICMP errors: echo replies, UDP, ARP, a later fragment, short frames.
        (void)icmpFrame(f, 0, 0, Peer);
        CHECK(l.allow(Span<const uint8_t>(f, n), 0));
        (void)icmpFrame(f, 3, 3, Peer);
        f[23] = 17;
        CHECK(l.allow(Span<const uint8_t>(f, n), 0));
        f[23] = 1;
        f[12] = 0x08;
        f[13] = 0x06;
        CHECK(l.allow(Span<const uint8_t>(f, n), 0));
        f[13] = 0;
        f[21] = 1; // fragment offset 8
        CHECK(l.allow(Span<const uint8_t>(f, n), 0));
        f[21] = 0;
        CHECK(l.allow(Span<const uint8_t>(f, 35), 0));
        CHECK_FALSE(l.allow(Span<const uint8_t>(f, n), 0));
        // IP options move the ICMP header.
        f[14] = 0x46;
        f[38] = 0;
        CHECK(l.allow(Span<const uint8_t>(f, n), 0));
        f[38] = 3;
        CHECK_FALSE(l.allow(Span<const uint8_t>(f, n), 0));
}
