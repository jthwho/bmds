/**
 * @file      ethernet.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Ethernet frames (devplan/network.md §4): MAC addresses, the frame header,
 * and ARP, which the driver's own test (`net arp`) speaks before there is an
 * IP stack. Portable: host-tested.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A 48-bit IEEE 802 MAC address, in wire order.
 *
 * @par Thread Safety
 * A value type: distinct instances may be used concurrently.
 */
struct MacAddress {
                /** @brief Bytes in an address. */
                static constexpr size_t Bytes = 6;

                /** @brief Characters `toString()` writes, without the terminator ("aa:bb:cc:dd:ee:ff"). */
                static constexpr size_t TextBytes = 17;

                uint8_t bytes[Bytes] = {};

                /** @brief Returns the broadcast address, ff:ff:ff:ff:ff:ff. */
                static constexpr MacAddress broadcast() { return {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}}; }

                /**
                 * @brief Parses "aa:bb:cc:dd:ee:ff" (or with '-'), either case.
                 * @return True and the address in @p out, or false (and @p out unchanged).
                 */
                static bool fromString(StringView text, MacAddress &out);

                /** @brief Writes "aa:bb:cc:dd:ee:ff" and a terminator into @p out (`TextBytes` + 1). */
                void toString(char *out) const;

                /** @brief Returns true for a group (multicast or broadcast) address. */
                constexpr bool isMulticast() const { return (bytes[0] & 0x01) != 0; }

                /** @brief Returns true for ff:ff:ff:ff:ff:ff. */
                constexpr bool isBroadcast() const { return *this == broadcast(); }

                /** @brief Returns true for 00:00:00:00:00:00. */
                constexpr bool isZero() const { return *this == MacAddress{}; }

                constexpr bool operator==(const MacAddress &other) const {
                        for (size_t i = 0; i < Bytes; ++i)
                                if (bytes[i] != other.bytes[i]) return false;
                        return true;
                }
};

/**
 * @brief Ethernet framing: the header, EtherTypes, and the reads and writes
 *        of big-endian fields every protocol above needs.
 *
 * @par Thread Safety
 * Stateless.
 */
namespace Ethernet {

        /** @brief The header: destination, source, EtherType. */
        constexpr size_t HeaderBytes = 14;

        /** @brief The largest payload (the MTU) without a VLAN tag. */
        constexpr size_t MaxPayloadBytes = 1500;

        /** @brief The largest frame, header and a VLAN tag included, without the FCS. */
        constexpr size_t MaxFrameBytes = HeaderBytes + 4 + MaxPayloadBytes;

        /** @brief The smallest frame without the FCS; the MAC pads shorter ones. */
        constexpr size_t MinFrameBytes = 60;

        /** @brief EtherTypes the stack knows. */
        enum class EtherType : uint16_t {
                Ipv4 = 0x0800,
                Arp = 0x0806,
                Vlan = 0x8100,
                Ipv6 = 0x86dd,
                Ptp = 0x88f7, ///< IEEE 1588 over Ethernet.
        };

        /** @brief Reads a big-endian 16-bit field. */
        constexpr uint16_t read16(const uint8_t *p) {
                return static_cast<uint16_t>((p[0] << 8) | p[1]);
        }

        /** @brief Reads a big-endian 32-bit field. */
        constexpr uint32_t read32(const uint8_t *p) {
                return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 8) | p[3];
        }

        /** @brief Writes a big-endian 16-bit field. */
        constexpr void write16(uint8_t *p, uint16_t v) {
                p[0] = static_cast<uint8_t>(v >> 8);
                p[1] = static_cast<uint8_t>(v);
                return;
        }

        /** @brief Writes a big-endian 32-bit field. */
        constexpr void write32(uint8_t *p, uint32_t v) {
                p[0] = static_cast<uint8_t>(v >> 24);
                p[1] = static_cast<uint8_t>(v >> 16);
                p[2] = static_cast<uint8_t>(v >> 8);
                p[3] = static_cast<uint8_t>(v);
                return;
        }

        /** @brief A frame's addresses and type, as read by `parseHeader()`. */
        struct Header {
                        MacAddress destination;
                        MacAddress source;
                        uint16_t   etherType = 0;     ///< After a VLAN tag, the inner type.
                        uint16_t   vlan = 0;          ///< The tag's control field (priority, VLAN id), or 0.
                        uint8_t    payloadOffset = 0; ///< `HeaderBytes`, or 18 after a VLAN tag.
                        bool       tagged = false;    ///< Had a VLAN tag.
        };

        /**
         * @brief Reads @p frame's header, one VLAN tag included.
         * @return False if the frame is shorter than its header.
         */
        bool parseHeader(Span<const uint8_t> frame, Header &out);

        /** @brief Writes a header (no tag) at @p out; returns `HeaderBytes`. */
        size_t writeHeader(uint8_t *out, const MacAddress &destination, const MacAddress &source, uint16_t etherType);

} // namespace Ethernet

/**
 * @brief ARP for IPv4 over Ethernet (RFC 826): requests and replies.
 *
 * Addresses are IPv4 addresses in host order (192.168.1.2 is 0xc0a80102).
 *
 * @par Thread Safety
 * Stateless.
 */
namespace Arp {

        /** @brief An ARP packet's size (for Ethernet and IPv4). */
        constexpr size_t PacketBytes = 28;

        /** @brief Operations. */
        enum class Op : uint16_t {
                Request = 1,
                Reply = 2,
        };

        /** @brief A packet's fields. */
        struct Packet {
                        Op         op = Op::Request;
                        MacAddress senderMac;
                        uint32_t   senderIp = 0;
                        MacAddress targetMac;
                        uint32_t   targetIp = 0;
        };

        /**
         * @brief Reads an ARP payload (after the Ethernet header).
         * @return False unless it is Ethernet/IPv4 ARP, a request or a reply.
         */
        bool parse(Span<const uint8_t> payload, Packet &out);

        /**
         * @brief Writes a whole frame for @p packet into @p out: a request is broadcast, a reply goes
         *        to its target. The frame is padded to `Ethernet::MinFrameBytes`.
         * @return The frame's length, or 0 if @p out is shorter than `Ethernet::MinFrameBytes`.
         */
        size_t writeFrame(const Packet &packet, Span<uint8_t> out);

} // namespace Arp

/**
 * @brief IPv4 addresses as text: dotted quads and CIDR prefixes.
 *
 * Addresses are in host order (192.168.1.2 is 0xc0a80102).
 *
 * @par Thread Safety
 * Stateless.
 */
namespace Ipv4 {

        /** @brief Characters `format()` writes at most, without the terminator. */
        constexpr size_t TextBytes = 15;

        /**
         * @brief Parses dotted-quad text ("192.168.1.2").
         * @return True and @p out, or false.
         */
        bool parse(StringView text, uint32_t &out);

        /**
         * @brief Parses an address with its prefix length ("192.168.1.2/24"); without one, /32.
         * @return True, @p address and @p prefix (0-32), or false.
         */
        bool parseCidr(StringView text, uint32_t &address, uint8_t &prefix);

        /** @brief Returns the netmask of a prefix length (24: 255.255.255.0); 0 for 0, all ones above 31. */
        constexpr uint32_t maskOf(uint8_t prefix) {
                return prefix == 0 ? 0 : prefix >= 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1);
        }

        /** @brief Returns a netmask's prefix length, or -1 if its ones aren't contiguous. */
        int prefixOf(uint32_t mask);

        /** @brief Writes @p ip as dotted-quad text and a terminator into @p out (`TextBytes` + 1). */
        void format(uint32_t ip, char *out);

} // namespace Ipv4

BMDS_NAMESPACE_END
