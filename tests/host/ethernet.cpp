/**
 * @file      ethernet.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Ethernet framing and ARP (devplan/network.md §4): MAC addresses as text,
 * headers with and without a VLAN tag, ARP frames written and read back,
 * dotted-quad addresses.
 */

#include <doctest.h>

#include <cstring>
#include <string>

#include <bmds/ethernet.h>

using bmds::MacAddress;
using bmds::Span;
using bmds::StringView;
namespace Arp = bmds::Arp;
namespace Ipv4 = bmds::Ipv4;
namespace Ethernet = bmds::Ethernet;

TEST_CASE("MacAddress: text both ways") {
        MacAddress a;
        REQUIRE(MacAddress::fromString(StringView("e8:f6:0a:E2:e2:22"), a));
        CHECK(a.bytes[0] == 0xe8);
        CHECK(a.bytes[3] == 0xe2);
        CHECK(a.bytes[5] == 0x22);
        char text[MacAddress::TextBytes + 1];
        a.toString(text);
        CHECK(std::string(text) == "e8:f6:0a:e2:e2:22");
        CHECK(MacAddress::fromString(StringView("01-00-5e-00-01-81"), a));
        CHECK(a.isMulticast());
        CHECK_FALSE(a.isBroadcast());
        CHECK(MacAddress::broadcast().isBroadcast());
        CHECK(MacAddress{}.isZero());

        const MacAddress before = a;
        CHECK_FALSE(MacAddress::fromString(StringView("e8:f6:0a:e2:e2"), a));
        CHECK_FALSE(MacAddress::fromString(StringView("e8:f6:0a:e2:e2:2g"), a));
        CHECK_FALSE(MacAddress::fromString(StringView("e8.f6.0a.e2.e2.22"), a));
        CHECK(a == before);
}

TEST_CASE("Ethernet: headers, tagged and not") {
        uint8_t          frame[64] = {};
        const MacAddress to = MacAddress::broadcast();
        const MacAddress from = {{0x02, 0, 0, 0, 0, 1}};
        CHECK(Ethernet::writeHeader(frame, to, from, 0x0800) == Ethernet::HeaderBytes);
        Ethernet::Header h;
        REQUIRE(Ethernet::parseHeader(Span<const uint8_t>(frame, sizeof(frame)), h));
        CHECK(h.destination == to);
        CHECK(h.source == from);
        CHECK(h.etherType == 0x0800);
        CHECK_FALSE(h.tagged);
        CHECK(h.payloadOffset == Ethernet::HeaderBytes);

        // An 802.1Q tag: priority 6, VLAN 100, then PTP.
        Ethernet::write16(frame + 12, 0x8100);
        Ethernet::write16(frame + 14, (6u << 13) | 100);
        Ethernet::write16(frame + 16, 0x88f7);
        REQUIRE(Ethernet::parseHeader(Span<const uint8_t>(frame, sizeof(frame)), h));
        CHECK(h.tagged);
        CHECK(h.vlan == ((6u << 13) | 100));
        CHECK(h.etherType == 0x88f7);
        CHECK(h.payloadOffset == 18);

        CHECK_FALSE(Ethernet::parseHeader(Span<const uint8_t>(frame, 13), h));
        CHECK_FALSE(Ethernet::parseHeader(Span<const uint8_t>(frame, 17), h)); // a tag cut short
}

TEST_CASE("Arp: a request written and read back") {
        Arp::Packet p;
        p.op = Arp::Op::Request;
        p.senderMac = {{0xe8, 0xf6, 0x0a, 0xe2, 0xe2, 0x22}};
        p.senderIp = 0; // a probe
        p.targetIp = 0xac140001;
        uint8_t      frame[Ethernet::MinFrameBytes];
        const size_t n = Arp::writeFrame(p, Span<uint8_t>(frame, sizeof(frame)));
        REQUIRE(n == Ethernet::MinFrameBytes);

        Ethernet::Header h;
        REQUIRE(Ethernet::parseHeader(Span<const uint8_t>(frame, n), h));
        CHECK(h.destination.isBroadcast());
        CHECK(h.source == p.senderMac);
        CHECK(h.etherType == 0x0806);
        Arp::Packet back;
        REQUIRE(Arp::parse(Span<const uint8_t>(frame + h.payloadOffset, n - h.payloadOffset), back));
        CHECK(back.op == Arp::Op::Request);
        CHECK(back.senderMac == p.senderMac);
        CHECK(back.senderIp == 0);
        CHECK(back.targetIp == 0xac140001);
        for (size_t i = Ethernet::HeaderBytes + Arp::PacketBytes; i < n; ++i) CHECK(frame[i] == 0);

        // A reply goes to its target, not to everyone.
        p.op = Arp::Op::Reply;
        p.targetMac = {{0x02, 0, 0, 0, 0, 9}};
        REQUIRE(Arp::writeFrame(p, Span<uint8_t>(frame, sizeof(frame))) == n);
        REQUIRE(Ethernet::parseHeader(Span<const uint8_t>(frame, n), h));
        CHECK(h.destination == p.targetMac);

        CHECK(Arp::writeFrame(p, Span<uint8_t>(frame, Ethernet::MinFrameBytes - 1)) == 0);
}

TEST_CASE("Arp: what isn't Ethernet/IPv4 ARP is refused") {
        Arp::Packet p;
        p.op = Arp::Op::Reply;
        uint8_t      frame[Ethernet::MinFrameBytes];
        const size_t n = Arp::writeFrame(p, Span<uint8_t>(frame, sizeof(frame)));
        uint8_t     *payload = frame + Ethernet::HeaderBytes;
        Arp::Packet  back;
        CHECK(Arp::parse(Span<const uint8_t>(payload, n - Ethernet::HeaderBytes), back));
        CHECK_FALSE(Arp::parse(Span<const uint8_t>(payload, Arp::PacketBytes - 1), back));
        payload[7] = 3; // RARP's request
        CHECK_FALSE(Arp::parse(Span<const uint8_t>(payload, Arp::PacketBytes), back));
        payload[7] = 2;
        payload[1] = 6; // IEEE 802 hardware
        CHECK_FALSE(Arp::parse(Span<const uint8_t>(payload, Arp::PacketBytes), back));
        payload[1] = 1;
        payload[5] = 16; // IPv6-sized protocol addresses
        CHECK_FALSE(Arp::parse(Span<const uint8_t>(payload, Arp::PacketBytes), back));
}

TEST_CASE("Ipv4: dotted quads") {
        uint32_t ip = 0;
        CHECK(Ipv4::parse(StringView("172.20.0.1"), ip));
        CHECK(ip == 0xac140001);
        CHECK(Ipv4::parse(StringView("0.0.0.0"), ip));
        CHECK(ip == 0);
        CHECK(Ipv4::parse(StringView("255.255.255.255"), ip));
        CHECK(ip == 0xffffffff);
        for (const char *bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1..2.3", "1.2.3.4 ", "a.b.c.d", "1.2.3.0004"})
                CHECK_FALSE(Ipv4::parse(StringView(bad), ip));
        char text[Ipv4::TextBytes + 1];
        Ipv4::format(0xc0a80102, text);
        CHECK(std::string(text) == "192.168.1.2");
        Ipv4::format(0xffffffff, text);
        CHECK(std::string(text) == "255.255.255.255");
}

TEST_CASE("Ipv4: CIDR prefixes and masks") {
        uint32_t ip = 0;
        uint8_t  prefix = 0;
        REQUIRE(Ipv4::parseCidr(StringView("192.168.1.50/24"), ip, prefix));
        CHECK(ip == 0xc0a80132);
        CHECK(prefix == 24);
        REQUIRE(Ipv4::parseCidr(StringView("10.0.0.1"), ip, prefix));
        CHECK(prefix == 32);
        REQUIRE(Ipv4::parseCidr(StringView("0.0.0.0/0"), ip, prefix));
        CHECK(prefix == 0);
        for (const char *bad : {"", "/24", "10.0.0.1/", "10.0.0.1/33", "10.0.0.1/2a", "10.0.0.1/024", "10.0.0/8"})
                CHECK_FALSE(Ipv4::parseCidr(StringView(bad), ip, prefix));
        CHECK(Ipv4::maskOf(0) == 0);
        CHECK(Ipv4::maskOf(16) == 0xffff0000u);
        CHECK(Ipv4::maskOf(32) == 0xffffffffu);
        CHECK(Ipv4::prefixOf(0xffffff00u) == 24);
        CHECK(Ipv4::prefixOf(0) == 0);
        CHECK(Ipv4::prefixOf(0xffffffffu) == 32);
        CHECK(Ipv4::prefixOf(0xff00ff00u) == -1);
}
