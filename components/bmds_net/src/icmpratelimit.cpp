/**
 * @file      icmpratelimit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's limits on ICMP errors (devplan/network.md §30g). Portable:
 * host-tested.
 */

#include <bmds/ethernet.h>
#include <bmds/icmpratelimit.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t  ProtocolIcmp = 1;
        constexpr uint16_t FragmentOffset = 0x1fff;
        constexpr uint8_t  DestinationUnreachable = 3;
        constexpr uint8_t  FragmentationNeeded = 4; // destination unreachable's code
        constexpr uint32_t RateMask = (1u << 3) | (1u << 4) | (1u << 11) | (1u << 12);

} // namespace

bool IcmpRateLimit::allow(Span<const uint8_t> frame, int64_t nowUs) {
        const size_t ip = Ethernet::HeaderBytes;
        if (frame.size() < ip + 20) return true;
        const uint8_t *f = frame.data();
        if (Ethernet::read16(f + 12) != static_cast<uint16_t>(Ethernet::EtherType::Ipv4)) return true;
        if ((f[ip] >> 4) != 4 || f[ip + 9] != ProtocolIcmp) return true;
        if ((Ethernet::read16(f + ip + 6) & FragmentOffset) != 0) return true;
        const size_t icmp = ip + (f[ip] & 0xfu) * 4u;
        if (frame.size() < icmp + 2) return true;
        return allowError(f[icmp], f[icmp + 1], Ethernet::read32(f + ip + 16), nowUs);
}

bool IcmpRateLimit::allowError(uint8_t type, uint8_t code, uint32_t destination, int64_t nowUs) {
        if (type >= 32 || (RateMask & (1u << type)) == 0) return true;
        if (type == DestinationUnreachable && code == FragmentationNeeded) return true;
        // All destinations: a credit refilled at PerSecond, up to Burst.
        if (!_started) {
                _started = true;
                _creditUs = nowUs;
        }
        const int64_t refill = (nowUs - _creditUs) * PerSecond / 1'000'000;
        if (refill > 0) {
                _credit = _credit + refill < Burst ? _credit + refill : Burst;
                _creditUs += refill * 1'000'000 / PerSecond;
        }
        if (_credit <= 0) {
                ++_limited;
                return false;
        }
        // Each destination: elapsed time is credit, up to PeerBurst intervals; an error costs one.
        Peer         &p = *peerOf(destination, nowUs);
        const int64_t most = PeerBurst * PeerIntervalUs;
        int64_t       tokens = p.tokensUs + (nowUs - p.lastUs);
        if (tokens > most) tokens = most;
        p.lastUs = nowUs;
        if (tokens < PeerIntervalUs) {
                p.tokensUs = tokens;
                ++_limited;
                return false;
        }
        p.tokensUs = tokens - PeerIntervalUs;
        --_credit;
        return true;
}

IcmpRateLimit::Peer *IcmpRateLimit::peerOf(uint32_t destination, int64_t nowUs) {
        Peer *oldest = &_peers[0];
        for (Peer &p : _peers) {
                if (p.used && p.destination == destination) return &p;
                if (!p.used) {
                        oldest = &p;
                        break;
                }
                if (p.lastUs < oldest->lastUs) oldest = &p;
        }
        // A destination new (or forgotten) starts with a full burst, as a new inetpeer does.
        *oldest = Peer{destination, nowUs, PeerBurst * PeerIntervalUs, true};
        return oldest;
}

BMDS_NAMESPACE_END
