/**
 * @file      ethernet.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Ethernet frames and ARP (devplan/network.md §4). Portable.
 */

#include <cstdio>
#include <cstring>
#include <bmds/ethernet.h>

BMDS_NAMESPACE_BEGIN

namespace {

        int hexDigit(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

        constexpr uint16_t HardwareEthernet = 1;
        constexpr uint8_t  HardwareBytes = 6;
        constexpr uint8_t  ProtocolBytes = 4;

} // namespace

bool MacAddress::fromString(StringView text, MacAddress &out) {
        if (text.size() != TextBytes) return false;
        MacAddress a;
        for (size_t i = 0; i < Bytes; ++i) {
                const size_t at = i * 3;
                const int    hi = hexDigit(text[at]);
                const int    lo = hexDigit(text[at + 1]);
                if (hi < 0 || lo < 0) return false;
                if (i + 1 < Bytes && text[at + 2] != ':' && text[at + 2] != '-') return false;
                a.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        out = a;
        return true;
}

void MacAddress::toString(char *out) const {
        static constexpr char Hex[] = "0123456789abcdef";
        for (size_t i = 0; i < Bytes; ++i) {
                out[i * 3] = Hex[bytes[i] >> 4];
                out[i * 3 + 1] = Hex[bytes[i] & 0x0f];
                out[i * 3 + 2] = i + 1 < Bytes ? ':' : '\0';
        }
        return;
}

bool Ethernet::parseHeader(Span<const uint8_t> frame, Header &out) {
        if (frame.size() < HeaderBytes) return false;
        const uint8_t *p = frame.data();
        std::memcpy(out.destination.bytes, p, MacAddress::Bytes);
        std::memcpy(out.source.bytes, p + MacAddress::Bytes, MacAddress::Bytes);
        out.etherType = read16(p + 12);
        out.vlan = 0;
        out.tagged = false;
        out.payloadOffset = HeaderBytes;
        if (out.etherType == static_cast<uint16_t>(EtherType::Vlan)) {
                if (frame.size() < HeaderBytes + 4) return false;
                out.tagged = true;
                out.vlan = read16(p + 14);
                out.etherType = read16(p + 16);
                out.payloadOffset = HeaderBytes + 4;
        }
        return true;
}

size_t Ethernet::writeHeader(uint8_t *out, const MacAddress &destination, const MacAddress &source,
                             uint16_t etherType) {
        std::memcpy(out, destination.bytes, MacAddress::Bytes);
        std::memcpy(out + MacAddress::Bytes, source.bytes, MacAddress::Bytes);
        write16(out + 12, etherType);
        return HeaderBytes;
}

bool Arp::parse(Span<const uint8_t> payload, Packet &out) {
        if (payload.size() < PacketBytes) return false;
        const uint8_t *p = payload.data();
        if (Ethernet::read16(p) != HardwareEthernet) return false;
        if (Ethernet::read16(p + 2) != static_cast<uint16_t>(Ethernet::EtherType::Ipv4)) return false;
        if (p[4] != HardwareBytes || p[5] != ProtocolBytes) return false;
        const uint16_t op = Ethernet::read16(p + 6);
        if (op != static_cast<uint16_t>(Op::Request) && op != static_cast<uint16_t>(Op::Reply)) return false;
        out.op = static_cast<Op>(op);
        std::memcpy(out.senderMac.bytes, p + 8, MacAddress::Bytes);
        out.senderIp = Ethernet::read32(p + 14);
        std::memcpy(out.targetMac.bytes, p + 18, MacAddress::Bytes);
        out.targetIp = Ethernet::read32(p + 24);
        return true;
}

size_t Arp::writeFrame(const Packet &packet, Span<uint8_t> out) {
        if (out.size() < Ethernet::MinFrameBytes) return 0;
        uint8_t         *p = out.data();
        const MacAddress to = packet.op == Op::Request ? MacAddress::broadcast() : packet.targetMac;
        size_t at = Ethernet::writeHeader(p, to, packet.senderMac, static_cast<uint16_t>(Ethernet::EtherType::Arp));
        Ethernet::write16(p + at, HardwareEthernet);
        Ethernet::write16(p + at + 2, static_cast<uint16_t>(Ethernet::EtherType::Ipv4));
        p[at + 4] = HardwareBytes;
        p[at + 5] = ProtocolBytes;
        Ethernet::write16(p + at + 6, static_cast<uint16_t>(packet.op));
        std::memcpy(p + at + 8, packet.senderMac.bytes, MacAddress::Bytes);
        Ethernet::write32(p + at + 14, packet.senderIp);
        std::memcpy(p + at + 18, packet.targetMac.bytes, MacAddress::Bytes);
        Ethernet::write32(p + at + 24, packet.targetIp);
        at += PacketBytes;
        std::memset(p + at, 0, Ethernet::MinFrameBytes - at);
        return Ethernet::MinFrameBytes;
}

bool Arp::parseIpv4(StringView text, uint32_t &out) {
        uint32_t ip = 0;
        size_t   i = 0;
        for (int part = 0; part < 4; ++part) {
                if (part > 0) {
                        if (i >= text.size() || text[i] != '.') return false;
                        ++i;
                }
                uint32_t v = 0;
                size_t   digits = 0;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                        v = v * 10 + static_cast<uint32_t>(text[i] - '0');
                        ++i;
                        if (++digits > 3) return false;
                }
                if (digits == 0 || v > 255) return false;
                ip = (ip << 8) | v;
        }
        if (i != text.size()) return false;
        out = ip;
        return true;
}

void Arp::formatIpv4(uint32_t ip, char *out) {
        std::snprintf(out, Ipv4TextBytes + 1, "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24),
                      static_cast<unsigned>((ip >> 16) & 0xff), static_cast<unsigned>((ip >> 8) & 0xff),
                      static_cast<unsigned>(ip & 0xff));
        return;
}

BMDS_NAMESPACE_END
