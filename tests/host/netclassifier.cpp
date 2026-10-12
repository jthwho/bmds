/**
 * @file      netclassifier.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The real-time classifier (devplan/network.md §30d): headers written with
 * checksums checked against values worked out apart, frames matched to rules
 * by EtherType and by UDP port and address, and the frames it must leave to
 * lwIP (fragments, bad checksums, lengths that don't agree).
 */

#include <doctest.h>

#include <cstring>

#include <bmds/netclassifier.h>

using bmds::MacAddress;
using bmds::Span;
namespace Arp = bmds::Arp;
namespace Ethernet = bmds::Ethernet;
namespace NetClassifier = bmds::NetClassifier;
using NetClassifier::Kind;
using NetClassifier::Rule;

namespace {

        constexpr uint32_t Board = 0xac14c8dd; // 172.20.200.221
        constexpr uint32_t Peer = 0xac140009;  // 172.20.0.9

        NetClassifier::UdpEndpoints endpoints() {
                NetClassifier::UdpEndpoints e;
                e.destinationMac = {{0x02, 0, 0, 0, 0, 2}};
                e.sourceMac = {{0x02, 0, 0, 0, 0, 1}};
                e.sourceIp = Board;
                e.destinationIp = Peer;
                e.sourcePort = 5004;
                e.destinationPort = 5005;
                e.dscp = 34;
                return e;
        }

        // A whole frame: the headers, then the payload, padded to the minimum.
        size_t frameOf(uint8_t *out, const NetClassifier::UdpEndpoints &e, Span<const uint8_t> payload,
                       uint16_t id = 0x1234) {
                const size_t h = NetClassifier::writeUdpHeaders(out, e, payload, id);
                if (h == 0) return 0;
                std::memcpy(out + h, payload.data(), payload.size());
                size_t n = h + payload.size();
                while (n < Ethernet::MinFrameBytes) out[n++] = 0;
                return n;
        }

        int classify(const uint8_t *frame, size_t n, bool checksummed, Span<const Rule> rules,
                     NetClassifier::Match &m) {
                return NetClassifier::classify(Span<const uint8_t>(frame, n), checksummed, rules, m);
        }

        // Fixes the IPv4 header's checksum after an edit.
        void resum(uint8_t *frame, size_t at = Ethernet::HeaderBytes) {
                uint8_t     *ip = frame + at;
                const size_t bytes = static_cast<size_t>(ip[0] & 0x0f) * 4;
                Ethernet::write16(ip + 10, 0);
                Ethernet::write16(ip + 10,
                                  NetClassifier::finish(NetClassifier::sum(Span<const uint8_t>(ip, bytes), 0)));
                return;
        }

} // namespace

TEST_CASE("NetClassifier: checksums") {
        // RFC 1071's way on a header whose checksum is known (b861).
        const uint8_t header[] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                                  0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
        CHECK(NetClassifier::finish(NetClassifier::sum(Span<const uint8_t>(header, sizeof(header)), 0)) == 0xb861);
        // An odd length pads with a zero.
        const uint8_t odd[] = {0x01, 0x02, 0x03};
        CHECK(NetClassifier::sum(Span<const uint8_t>(odd, 3), 0) == 0x0102 + 0x0300);
}

TEST_CASE("NetClassifier: UDP headers as worked out apart") {
        const uint8_t payload[] = {1, 2, 3, 4, 5};
        uint8_t       f[128];
        const size_t  n = frameOf(f, endpoints(), Span<const uint8_t>(payload, sizeof(payload)));
        REQUIRE(n == Ethernet::MinFrameBytes);
        CHECK(Ethernet::read16(f + 12) == 0x0800);
        const uint8_t *ip = f + Ethernet::HeaderBytes;
        CHECK(ip[0] == 0x45);
        CHECK(ip[1] == 34 << 2);
        CHECK(Ethernet::read16(ip + 2) == 20 + 8 + 5);
        CHECK(Ethernet::read16(ip + 4) == 0x1234);
        CHECK(Ethernet::read16(ip + 6) == 0x4000); // Don't Fragment
        CHECK(ip[8] == 64);
        CHECK(ip[9] == 17);
        CHECK(Ethernet::read16(ip + 10) == 0x0701); // by Python's sums
        const uint8_t *udp = ip + 20;
        CHECK(Ethernet::read16(udp) == 5004);
        CHECK(Ethernet::read16(udp + 2) == 5005);
        CHECK(Ethernet::read16(udp + 4) == 13);
        CHECK(Ethernet::read16(udp + 6) == 0xaea5);

        uint8_t big[1600];
        CHECK(NetClassifier::writeUdpHeaders(big, endpoints(),
                                             Span<const uint8_t>(big, NetClassifier::MaxUdpPayloadBytes + 1), 0) == 0);
        CHECK(NetClassifier::writeUdpHeaders(big, endpoints(),
                                             Span<const uint8_t>(big, NetClassifier::MaxUdpPayloadBytes),
                                             0) == NetClassifier::UdpHeadersBytes);
}

TEST_CASE("NetClassifier: UDP matched by port and address") {
        const uint8_t payload[300] = {0xaa};
        uint8_t       f[512];
        const size_t  n = frameOf(f, endpoints(), Span<const uint8_t>(payload, sizeof(payload)));
        REQUIRE(n == NetClassifier::UdpHeadersBytes + 300);

        NetClassifier::Match m;
        const Rule           none[] = {{Kind::Udp, 0, 5004, 0}, {Kind::Udp, 0, 5005, 0x0a000001}};
        CHECK(classify(f, n, false, Span<const Rule>(none, 2), m) == -1);

        const Rule rules[] = {{Kind::EtherType, 0x88f7, 0, 0}, {Kind::Udp, 0, 5005, Peer}, {Kind::Udp, 0, 5005, 0}};
        REQUIRE(classify(f, n, false, Span<const Rule>(rules, 3), m) == 1); // the first that matches
        CHECK(m.offset == NetClassifier::UdpHeadersBytes);
        CHECK(m.length == 300);
        CHECK(m.sourceIp == Board);
        CHECK(m.sourcePort == 5004);
        CHECK(m.destinationIp == Peer);
        CHECK(m.source == endpoints().sourceMac);
        CHECK(f[m.offset] == 0xaa);

        // Any address.
        CHECK(classify(f, n, false, Span<const Rule>(rules + 2, 1), m) == 0);
        // No rules.
        CHECK(classify(f, n, false, Span<const Rule>(), m) == -1);
        // A short payload in a padded frame: its own length, not the frame's.
        const uint8_t tiny[] = {7};
        const size_t  t = frameOf(f, endpoints(), Span<const uint8_t>(tiny, 1));
        REQUIRE(t == Ethernet::MinFrameBytes);
        REQUIRE(classify(f, t, false, Span<const Rule>(rules + 2, 1), m) == 0);
        CHECK(m.length == 1);
        CHECK(f[m.offset] == 7);
}

TEST_CASE("NetClassifier: UDP headers for a payload in pieces") {
        uint8_t payload[301];
        for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i * 37 + 11);
        uint8_t whole[NetClassifier::UdpHeadersBytes];
        REQUIRE(NetClassifier::writeUdpHeaders(whole, endpoints(), Span<const uint8_t>(payload, sizeof(payload)), 9) ==
                NetClassifier::UdpHeadersBytes);
        for (const size_t a : {0, 1, 2, 7, 150, 301}) {
                for (const size_t b : {a, a + 1 < 301 ? a + 1 : a, 300 > a ? 300 : a}) {
                        const Span<const uint8_t> pieces[] = {Span<const uint8_t>(payload, a),
                                                              Span<const uint8_t>(payload + a, b - a),
                                                              Span<const uint8_t>(payload + b, sizeof(payload) - b)};
                        uint8_t                   split[NetClassifier::UdpHeadersBytes];
                        REQUIRE(NetClassifier::writeUdpHeaders(split, endpoints(),
                                                               Span<const Span<const uint8_t>>(pieces, 3), 9) ==
                                NetClassifier::UdpHeadersBytes);
                        CHECK(std::memcmp(split, whole, sizeof(whole)) == 0);
                }
        }
}

TEST_CASE("NetClassifier: a connected flow's sender") {
        const uint8_t payload[40] = {};
        uint8_t       f[128];
        const size_t  n = frameOf(f, endpoints(), Span<const uint8_t>(payload, sizeof(payload)));
        NetClassifier::Match m;
        const Rule           others[] = {{Kind::Udp, 0, 5005, 0, Board + 1, 0}, {Kind::Udp, 0, 5005, 0, Board, 5003}};
        CHECK(classify(f, n, false, Span<const Rule>(others, 2), m) == -1);
        const Rule ours[] = {{Kind::Udp, 0, 5005, 0, Board, 5004}, {Kind::Udp, 0, 5005, 0, Board, 0}};
        CHECK(classify(f, n, false, Span<const Rule>(ours, 2), m) == 0);
        CHECK(classify(f, n, false, Span<const Rule>(ours + 1, 1), m) == 0);
}

TEST_CASE("NetClassifier: a group's datagrams") {
        NetClassifier::UdpEndpoints e = endpoints();
        const uint32_t              group = 0xef450101; // 239.69.1.1
        e.destinationIp = group;
        e.destinationMac = NetClassifier::groupMac(group);
        CHECK(e.destinationMac == MacAddress{{0x01, 0x00, 0x5e, 0x45, 0x01, 0x01}});
        CHECK(NetClassifier::isGroup(group));
        CHECK_FALSE(NetClassifier::isGroup(Peer));
        CHECK(NetClassifier::groupMac(0xe0ff0001) == MacAddress{{0x01, 0x00, 0x5e, 0x7f, 0x00, 0x01}}); // 23 bits

        const uint8_t        payload[20] = {};
        uint8_t              f[128];
        const size_t         n = frameOf(f, e, Span<const uint8_t>(payload, sizeof(payload)));
        NetClassifier::Match m;
        const Rule           other[] = {{Kind::Udp, 0, 5005, 0xef450102}};
        CHECK(classify(f, n, false, Span<const Rule>(other, 1), m) == -1);
        const Rule mine[] = {{Kind::Udp, 0, 5005, group}};
        CHECK(classify(f, n, false, Span<const Rule>(mine, 1), m) == 0);
}

TEST_CASE("NetClassifier: what is left to lwIP") {
        const uint8_t        payload[40] = {1, 2, 3};
        uint8_t              f[256];
        const Rule           rules[] = {{Kind::Udp, 0, 5005, 0}};
        const auto           all = Span<const Rule>(rules, 1);
        NetClassifier::Match m;
        const size_t         n = frameOf(f, endpoints(), Span<const uint8_t>(payload, sizeof(payload)));
        uint8_t             *ip = f + Ethernet::HeaderBytes;
        REQUIRE(classify(f, n, false, all, m) == 0);

        SUBCASE("a bad IPv4 checksum: checked unless the MAC did") {
                ip[10] ^= 1;
                CHECK(classify(f, n, false, all, m) == -1);
                CHECK(classify(f, n, true, all, m) == 0);
        }
        SUBCASE("a bad UDP checksum") {
                ip[20 + 8] ^= 0x40;
                CHECK(classify(f, n, false, all, m) == -1);
                CHECK(classify(f, n, true, all, m) == 0);
        }
        SUBCASE("no UDP checksum") {
                Ethernet::write16(ip + 26, 0);
                ip[20 + 8] ^= 0x40;
                CHECK(classify(f, n, false, all, m) == 0);
        }
        SUBCASE("fragments") {
                Ethernet::write16(ip + 6, 0x2000); // more fragments
                resum(f);
                CHECK(classify(f, n, false, all, m) == -1);
                Ethernet::write16(ip + 6, 0x0001); // an offset
                resum(f);
                CHECK(classify(f, n, false, all, m) == -1);
                Ethernet::write16(ip + 6, 0x0000); // whole, may be fragmented
                resum(f);
                CHECK(classify(f, n, false, all, m) == 0);
        }
        SUBCASE("lengths") {
                CHECK(classify(f, n - 1, false, all, m) == -1); // the datagram is cut short
                Ethernet::write16(ip + 2, static_cast<uint16_t>(n - Ethernet::HeaderBytes + 1));
                resum(f);
                CHECK(classify(f, n, false, all, m) == -1); // longer than the frame
                Ethernet::write16(ip + 2, 20 + 7);
                resum(f);
                CHECK(classify(f, n, true, all, m) == -1); // no room for UDP's header
                Ethernet::write16(ip + 2, static_cast<uint16_t>(20 + 8 + 40));
                resum(f);
                Ethernet::write16(ip + 24, 7);
                CHECK(classify(f, n, true, all, m) == -1); // UDP shorter than its header
                Ethernet::write16(ip + 24, 8 + 41);
                CHECK(classify(f, n, true, all, m) == -1);  // UDP longer than IPv4's payload
                CHECK(classify(f, 13, true, all, m) == -1); // no Ethernet header
                CHECK(classify(f, Ethernet::HeaderBytes + 19, true, all, m) == -1);
        }
        SUBCASE("not UDP, not IPv4") {
                ip[9] = 6; // TCP
                resum(f);
                CHECK(classify(f, n, true, all, m) == -1);
                ip[9] = 17;
                ip[0] = 0x65; // version 6
                CHECK(classify(f, n, true, all, m) == -1);
                ip[0] = 0x44; // a header shorter than 20
                CHECK(classify(f, n, true, all, m) == -1);
        }
}

TEST_CASE("NetClassifier: IPv4 options and a VLAN tag") {
        // Options: 4 bytes of no-ops, the payload after them.
        const uint8_t payload[16] = {9, 8, 7};
        uint8_t       plain[128];
        const size_t  n = frameOf(plain, endpoints(), Span<const uint8_t>(payload, sizeof(payload)));
        uint8_t       f[160];
        std::memcpy(f, plain, Ethernet::HeaderBytes + 20);
        std::memset(f + Ethernet::HeaderBytes + 20, 1, 4);
        std::memcpy(f + Ethernet::HeaderBytes + 24, plain + Ethernet::HeaderBytes + 20, n - Ethernet::HeaderBytes - 20);
        f[Ethernet::HeaderBytes] = 0x46;
        Ethernet::write16(f + Ethernet::HeaderBytes + 2, 24 + 8 + 16);
        resum(f);
        const Rule           rules[] = {{Kind::Udp, 0, 5005, 0}};
        NetClassifier::Match m;
        REQUIRE(classify(f, n + 4, false, Span<const Rule>(rules, 1), m) == 0);
        CHECK(m.offset == Ethernet::HeaderBytes + 24 + 8);
        CHECK(f[m.offset] == 9);

        // A tag: the type after it, the payload 4 bytes on.
        uint8_t t[160];
        std::memcpy(t, plain, 12);
        Ethernet::write16(t + 12, 0x8100);
        Ethernet::write16(t + 14, 0xa005); // priority 5, VLAN 5
        std::memcpy(t + 16, plain + 12, n - 12);
        REQUIRE(classify(t, n + 4, false, Span<const Rule>(rules, 1), m) == 0);
        CHECK(m.offset == NetClassifier::UdpHeadersBytes + 4);
        CHECK(t[m.offset] == 9);
}

TEST_CASE("NetClassifier: EtherType flows") {
        uint8_t f[64] = {};
        Ethernet::writeHeader(f, MacAddress{{0x01, 0x1b, 0x19, 0, 0, 0}}, MacAddress{{0x02, 0, 0, 0, 0, 1}}, 0x88f7);
        f[14] = 0x12; // a PTP message
        const Rule           rules[] = {{Kind::Udp, 0, 319, 0}, {Kind::EtherType, 0x88f7, 0, 0}};
        NetClassifier::Match m;
        REQUIRE(classify(f, sizeof(f), false, Span<const Rule>(rules, 2), m) == 1);
        CHECK(m.offset == Ethernet::HeaderBytes);
        CHECK(m.length == sizeof(f) - Ethernet::HeaderBytes);
        Ethernet::write16(f + 12, 0x88f8);
        CHECK(classify(f, sizeof(f), false, Span<const Rule>(rules, 2), m) == -1);

        // Tagged.
        uint8_t t[68] = {};
        Ethernet::writeHeader(t, MacAddress::broadcast(), MacAddress{{0x02, 0, 0, 0, 0, 1}}, 0x8100);
        Ethernet::write16(t + 14, 0x6000);
        Ethernet::write16(t + 16, 0x88f7);
        REQUIRE(classify(t, sizeof(t), false, Span<const Rule>(rules, 2), m) == 1);
        CHECK(m.offset == Ethernet::HeaderBytes + 4);
}

TEST_CASE("NetClassifier: ARP senders") {
        Arp::Packet p;
        p.op = Arp::Op::Reply;
        p.senderMac = {{0x00, 0x02, 0xc9, 0x54, 0x78, 0x34}};
        p.senderIp = Peer;
        p.targetMac = {{0x02, 0, 0, 0, 0, 1}};
        p.targetIp = Board;
        uint8_t      f[64];
        const size_t n = Arp::writeFrame(p, Span<uint8_t>(f, sizeof(f)));
        REQUIRE(n == Ethernet::MinFrameBytes);
        uint32_t   ip = 0;
        MacAddress mac;
        REQUIRE(NetClassifier::arpSender(Span<const uint8_t>(f, n), ip, mac));
        CHECK(ip == Peer);
        CHECK(mac == p.senderMac);

        // A probe (sender 0.0.0.0) and a group sender teach nothing; nor does IPv4.
        p.senderIp = 0;
        Arp::writeFrame(p, Span<uint8_t>(f, sizeof(f)));
        CHECK_FALSE(NetClassifier::arpSender(Span<const uint8_t>(f, n), ip, mac));
        p.senderIp = Peer;
        p.senderMac = MacAddress::broadcast();
        Arp::writeFrame(p, Span<uint8_t>(f, sizeof(f)));
        CHECK_FALSE(NetClassifier::arpSender(Span<const uint8_t>(f, n), ip, mac));
        const uint8_t payload[4] = {};
        uint8_t       u[128];
        const size_t  k = frameOf(u, endpoints(), Span<const uint8_t>(payload, 4));
        CHECK_FALSE(NetClassifier::arpSender(Span<const uint8_t>(u, k), ip, mac));
        CHECK_FALSE(NetClassifier::arpSender(Span<const uint8_t>(f, 20), ip, mac));
}
