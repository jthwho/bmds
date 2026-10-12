/**
 * @file      netclassifier.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the real-time classifier (bmds/netclassifier.h,
 * devplan/network.md §30d), which reads every frame `eth0` receives while a
 * flow is open: the input's first byte says whether the MAC checked the
 * checksums and how many rules follow (8 bytes each), the rest is the frame.
 * A match must lie inside the frame; a UDP match, written again with the
 * classifier's own headers, must match the same rule; ARP's sender read too.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <bmds/netclassifier.h>

using bmds::MacAddress;
using bmds::Span;
namespace Ethernet = bmds::Ethernet;
namespace NetClassifier = bmds::NetClassifier;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        if (size < 1) return 0;
        const bool   checksummed = (data[0] & 0x80) != 0;
        const size_t count = data[0] & 0x07;
        ++data;
        --size;
        NetClassifier::Rule rules[8];
        size_t              n = 0;
        for (; n < count && size >= 12; ++n, data += 12, size -= 12) {
                rules[n].kind = static_cast<NetClassifier::Kind>(data[0] % 3);
                rules[n].etherType = Ethernet::read16(data + 1);
                rules[n].port = Ethernet::read16(data + 3);
                rules[n].address = data[5] & 1 ? 0 : Ethernet::read32(data + 4);
                rules[n].sourceAddress = data[5] & 2 ? Ethernet::read32(data + 8) : 0;
                rules[n].sourcePort = data[5] & 4 ? Ethernet::read16(data + 10) : 0;
        }
        // The frame in a buffer of its own size, so a read past it faults.
        uint8_t *frame = static_cast<uint8_t *>(std::malloc(size == 0 ? 1 : size));
        std::memcpy(frame, data, size);
        const auto           rules_ = Span<const NetClassifier::Rule>(rules, n);
        NetClassifier::Match m;
        const int            i = NetClassifier::classify(Span<const uint8_t>(frame, size), checksummed, rules_, m);
        if (i >= 0) {
                if (static_cast<size_t>(i) >= n) std::abort();
                if (m.offset < Ethernet::HeaderBytes || size_t(m.offset) + m.length > size) std::abort();
                if (rules[i].kind == NetClassifier::Kind::Udp && m.length <= NetClassifier::MaxUdpPayloadBytes) {
                        // Sent again as the classifier writes it: the same rule, the same payload.
                        uint8_t                     again[Ethernet::MaxFrameBytes];
                        NetClassifier::UdpEndpoints e;
                        e.sourceIp = m.sourceIp;
                        e.destinationIp = m.destinationIp;
                        e.sourcePort = m.sourcePort;
                        e.destinationPort = rules[i].port;
                        const auto   payload = Span<const uint8_t>(frame + m.offset, m.length);
                        const size_t h = NetClassifier::writeUdpHeaders(again, e, payload, 1);
                        if (h != NetClassifier::UdpHeadersBytes) std::abort();
                        // The same headers from the payload in pieces, split where its bytes say.
                        const size_t a = m.length == 0 ? 0 : frame[m.offset] % (m.length + 1u);
                        const size_t b = a + (m.length - a == 0 ? 0 : frame[m.offset + m.length - 1] % (m.length - a + 1u));
                        const Span<const uint8_t> pieces[] = {payload.mid(0, a), payload.mid(a, b - a),
                                                              payload.mid(b, m.length - b)};
                        uint8_t                   split[NetClassifier::UdpHeadersBytes];
                        if (NetClassifier::writeUdpHeaders(split, e, Span<const Span<const uint8_t>>(pieces, 3), 1) !=
                                    h ||
                            std::memcmp(split, again, h) != 0)
                                std::abort();
                        std::memcpy(again + h, payload.data(), payload.size());
                        NetClassifier::Match m2;
                        const int            j =
                                NetClassifier::classify(Span<const uint8_t>(again, h + m.length), false, rules_, m2);
                        if (j < 0 || rules[j].kind != NetClassifier::Kind::Udp || m2.length != m.length ||
                            std::memcmp(again + m2.offset, payload.data(), m.length) != 0)
                                std::abort();
                }
        }
        uint32_t   ip;
        MacAddress mac;
        if (NetClassifier::arpSender(Span<const uint8_t>(frame, size), ip, mac) && (ip == 0 || mac.isMulticast()))
                std::abort();
        std::free(frame);
        return 0;
}
