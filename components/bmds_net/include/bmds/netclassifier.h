/**
 * @file      netclassifier.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The real-time classifier (devplan/network.md §5.1, §30d): which flow a
 * received frame belongs to, read straight from the frame, and the headers
 * a flow sends with. Portable: host-tested and fuzzed.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/ethernet.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Matches received frames to real-time flows, and writes the headers
 *        of the frames flows send.
 *
 * Addresses are IPv4 addresses in host order (192.168.1.2 is 0xc0a80102).
 *
 * @par Thread Safety
 * Stateless.
 */
namespace NetClassifier {

        /** @brief What a rule matches. */
        enum class Kind : uint8_t {
                None,      ///< Nothing (an empty slot).
                EtherType, ///< Frames of an EtherType (after one VLAN tag).
                Udp,       ///< UDP datagrams over IPv4 to a port (and an address).
        };

        /** @brief A flow's rule. */
        struct Rule {
                        Kind     kind = Kind::None;
                        uint16_t etherType = 0; ///< `EtherType`: the type (PTP's 0x88f7).
                        uint16_t port = 0;      ///< `Udp`: the destination port.
                        uint32_t address = 0;   ///< `Udp`: the destination address; 0 matches any.
                        uint32_t sourceAddress = 0; ///< `Udp`: the sender's address; 0 matches any.
                        uint16_t sourcePort = 0;    ///< `Udp`: the sender's port; 0 matches any.
        };

        /** @brief Where a matched frame's payload is, and who sent it. */
        struct Match {
                        uint16_t   offset = 0;     ///< The payload's start in the frame.
                        uint16_t   length = 0;     ///< The payload's bytes.
                        uint32_t   sourceIp = 0;   ///< `Udp`: the sender.
                        uint16_t   sourcePort = 0; ///< `Udp`: the sender's port.
                        uint32_t   destinationIp = 0;
                        MacAddress source; ///< The frame's source.
        };

        /** @brief Bytes of the Ethernet, IPv4 (no options) and UDP headers a flow sends. */
        constexpr size_t UdpHeadersBytes = Ethernet::HeaderBytes + 20 + 8;

        /** @brief The largest UDP payload a flow sends (no fragments). */
        constexpr size_t MaxUdpPayloadBytes = Ethernet::MaxPayloadBytes - 20 - 8;

        /** @brief Where a flow's datagrams go, and from where. */
        struct UdpEndpoints {
                        MacAddress destinationMac;
                        MacAddress sourceMac;
                        uint32_t   destinationIp = 0;
                        uint32_t   sourceIp = 0;
                        uint16_t   destinationPort = 0;
                        uint16_t   sourcePort = 0;
                        uint8_t    ttl = 64;
                        uint8_t    dscp = 0; ///< Differentiated services (0-63): AES67's media is 34.
        };

        /**
         * @brief Finds the first of @p rules @p frame matches: an EtherType rule by the type after
         *        one VLAN tag; a UDP rule by an unfragmented IPv4 datagram whose lengths agree, to
         *        its port and (unless 0) its address, from (unless 0) its source address and port. A frame not @p checksummed by the MAC has its
         *        IPv4 header's and UDP's checksums checked here (a UDP checksum of 0 is none).
         * @return The rule's index, with @p out; -1 for none (and @p out undefined).
         * @nonblocking @hotpath
         */
        int classify(Span<const uint8_t> frame, bool checksummed, Span<const Rule> rules, Match &out);

        /**
         * @brief Reads an ARP frame's sender (a request's or a reply's), with which flows keep their
         *        destinations' MACs.
         * @return True with @p ip and @p mac; false for any other frame.
         * @nonblocking @hotpath
         */
        bool arpSender(Span<const uint8_t> frame, uint32_t &ip, MacAddress &mac);

        /**
         * @brief Writes the Ethernet, IPv4 and UDP headers (`UdpHeadersBytes`) for @p payload at
         *        @p out: no fragmenting (Don't Fragment set), the IPv4 identification @p id, both
         *        checksums.
         * @return `UdpHeadersBytes`, or 0 for a payload over `MaxUdpPayloadBytes`.
         * @nonblocking
         */
        size_t writeUdpHeaders(uint8_t *out, const UdpEndpoints &endpoints, Span<const uint8_t> payload, uint16_t id);

        /** @brief As above, the payload gathered from @p payload's pieces, in order. @nonblocking */
        size_t writeUdpHeaders(uint8_t *out, const UdpEndpoints &endpoints, Span<const Span<const uint8_t>> payload,
                               uint16_t id);

        /**
         * @brief Adds @p bytes to a one's-complement sum (RFC 1071), as 16-bit big-endian words; an
         *        odd last byte is padded with a zero.
         */
        uint32_t sum(Span<const uint8_t> bytes, uint32_t initial);

        /** @brief Folds a sum to 16 bits and complements it: the checksum to write. */
        uint16_t finish(uint32_t sum);

        /** @brief Returns the MAC address of IPv4 group @p group (RFC 1112: 01:00:5e and 23 bits). */
        constexpr MacAddress groupMac(uint32_t group) {
                return {{0x01, 0x00, 0x5e, static_cast<uint8_t>((group >> 16) & 0x7f), static_cast<uint8_t>(group >> 8),
                         static_cast<uint8_t>(group)}};
        }

        /** @brief Returns true for an IPv4 group address (224.0.0.0/4). */
        constexpr bool isGroup(uint32_t address) {
                return (address >> 28) == 0xe;
        }

} // namespace NetClassifier

BMDS_NAMESPACE_END
