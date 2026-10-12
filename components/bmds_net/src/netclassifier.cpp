/**
 * @file      netclassifier.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The real-time classifier (devplan/network.md §30d). Portable: host-tested
 * and fuzzed.
 */

#include <cstring>
#include <bmds/netclassifier.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   Ipv4Bytes = 20; // without options
        constexpr size_t   UdpBytes = 8;
        constexpr uint8_t  ProtocolUdp = 17;
        constexpr uint16_t MoreFragments = 0x2000;
        constexpr uint16_t FragmentOffset = 0x1fff;
        constexpr uint16_t DontFragment = 0x4000;

        using Ethernet::read16;
        using Ethernet::read32;
        using Ethernet::write16;
        using Ethernet::write32;

} // namespace

uint32_t NetClassifier::sum(Span<const uint8_t> bytes, uint32_t initial) {
        const uint8_t *p = bytes.data();
        size_t         n = bytes.size();
        uint64_t       s = initial;
        for (; n >= 2; p += 2, n -= 2) s += static_cast<uint32_t>((p[0] << 8) | p[1]);
        if (n != 0) s += static_cast<uint32_t>(p[0] << 8);
        while ((s >> 32) != 0) s = (s & 0xffffffffu) + (s >> 32);
        return static_cast<uint32_t>(s);
}

uint16_t NetClassifier::finish(uint32_t s) {
        while ((s >> 16) != 0) s = (s & 0xffffu) + (s >> 16);
        return static_cast<uint16_t>(~s);
}

int NetClassifier::classify(Span<const uint8_t> frame, bool checksummed, Span<const Rule> rules, Match &out) {
        Ethernet::Header h;
        if (!Ethernet::parseHeader(frame, h)) return -1;
        out = Match{};
        out.source = h.source;
        const size_t at = h.payloadOffset;
        if (h.etherType != static_cast<uint16_t>(Ethernet::EtherType::Ipv4)) {
                for (size_t i = 0; i < rules.size(); ++i) {
                        if (rules[i].kind != Kind::EtherType || rules[i].etherType != h.etherType) continue;
                        out.offset = static_cast<uint16_t>(at);
                        out.length = static_cast<uint16_t>(frame.size() - at);
                        return static_cast<int>(i);
                }
                return -1;
        }
        // IPv4: a whole datagram, its lengths inside the frame (which may carry padding after it).
        const uint8_t *ip = frame.data() + at;
        const size_t   room = frame.size() - at;
        if (room < Ipv4Bytes || (ip[0] >> 4) != 4) return -1;
        const size_t headerBytes = static_cast<size_t>(ip[0] & 0x0f) * 4;
        const size_t total = read16(ip + 2);
        if (headerBytes < Ipv4Bytes || total < headerBytes + UdpBytes || total > room) return -1;
        if (ip[9] != ProtocolUdp || (read16(ip + 6) & (MoreFragments | FragmentOffset)) != 0) return -1;
        const uint8_t *udp = ip + headerBytes;
        const size_t   udpBytes = read16(udp + 4);
        if (udpBytes < UdpBytes || udpBytes > total - headerBytes) return -1;
        const uint16_t port = read16(udp + 2);
        const uint32_t destination = read32(ip + 16);
        const uint32_t source = read32(ip + 12);
        const uint16_t sourcePort = read16(udp);
        int            found = -1;
        for (size_t i = 0; i < rules.size() && found < 0; ++i) {
                const Rule &r = rules[i];
                if (r.kind == Kind::Udp && r.port == port && (r.address == 0 || r.address == destination) &&
                    (r.sourceAddress == 0 || r.sourceAddress == source) &&
                    (r.sourcePort == 0 || r.sourcePort == sourcePort))
                        found = static_cast<int>(i);
        }
        if (found < 0) return -1;
        if (!checksummed) {
                // The MAC passed this one by (or doesn't check): the checks lwIP would make.
                if (finish(sum(Span<const uint8_t>(ip, headerBytes), 0)) != 0) return -1;
                if (read16(udp + 6) != 0) {
                        uint32_t s = sum(Span<const uint8_t>(ip + 12, 8), 0); // the pseudo-header's addresses
                        s += ProtocolUdp + static_cast<uint32_t>(udpBytes);
                        if (finish(sum(Span<const uint8_t>(udp, udpBytes), s)) != 0) return -1;
                }
        }
        out.offset = static_cast<uint16_t>(at + headerBytes + UdpBytes);
        out.length = static_cast<uint16_t>(udpBytes - UdpBytes);
        out.sourceIp = source;
        out.sourcePort = sourcePort;
        out.destinationIp = destination;
        return found;
}

bool NetClassifier::arpSender(Span<const uint8_t> frame, uint32_t &ip, MacAddress &mac) {
        Ethernet::Header h;
        if (!Ethernet::parseHeader(frame, h) || h.etherType != static_cast<uint16_t>(Ethernet::EtherType::Arp))
                return false;
        Arp::Packet p;
        if (!Arp::parse(frame.mid(h.payloadOffset), p)) return false;
        if (p.senderIp == 0 || p.senderMac.isMulticast() || p.senderMac.isZero()) return false;
        ip = p.senderIp;
        mac = p.senderMac;
        return true;
}

size_t NetClassifier::writeUdpHeaders(uint8_t *out, const UdpEndpoints &e, Span<const uint8_t> payload, uint16_t id) {
        return writeUdpHeaders(out, e, Span<const Span<const uint8_t>>(&payload, 1), id);
}

size_t NetClassifier::writeUdpHeaders(uint8_t *out, const UdpEndpoints &e, Span<const Span<const uint8_t>> payload,
                                      uint16_t id) {
        size_t payloadBytes = 0;
        for (const Span<const uint8_t> &p : payload) payloadBytes += p.size();
        if (payloadBytes > MaxUdpPayloadBytes) return 0;
        const size_t udpBytes = UdpBytes + payloadBytes;
        uint8_t     *ip = out + Ethernet::writeHeader(out, e.destinationMac, e.sourceMac,
                                                      static_cast<uint16_t>(Ethernet::EtherType::Ipv4));
        ip[0] = 0x45;
        ip[1] = static_cast<uint8_t>(e.dscp << 2);
        write16(ip + 2, static_cast<uint16_t>(Ipv4Bytes + udpBytes));
        write16(ip + 4, id);
        write16(ip + 6, DontFragment);
        ip[8] = e.ttl;
        ip[9] = ProtocolUdp;
        write16(ip + 10, 0);
        write32(ip + 12, e.sourceIp);
        write32(ip + 16, e.destinationIp);
        write16(ip + 10, finish(sum(Span<const uint8_t>(ip, Ipv4Bytes), 0)));
        uint8_t *udp = ip + Ipv4Bytes;
        write16(udp, e.sourcePort);
        write16(udp + 2, e.destinationPort);
        write16(udp + 4, static_cast<uint16_t>(udpBytes));
        write16(udp + 6, 0);
        uint32_t s = sum(Span<const uint8_t>(ip + 12, 8), ProtocolUdp + static_cast<uint32_t>(udpBytes));
        s = sum(Span<const uint8_t>(udp, UdpBytes), s);
        // A piece starting at an odd offset sums with its bytes the other way round (RFC 1071).
        while ((s >> 16) != 0) s = (s & 0xffff) + (s >> 16);
        size_t at = 0;
        for (const Span<const uint8_t> &p : payload) {
                uint32_t part = sum(p, 0);
                while ((part >> 16) != 0) part = (part & 0xffff) + (part >> 16);
                if (at & 1) part = ((part & 0xff) << 8) | (part >> 8);
                s += part;
                at += p.size();
        }
        uint16_t c = finish(s);
        write16(udp + 6, c == 0 ? 0xffff : c); // 0 means none: all ones is the same sum
        return UdpHeadersBytes;
}

BMDS_NAMESPACE_END
