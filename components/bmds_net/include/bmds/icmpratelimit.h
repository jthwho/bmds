/**
 * @file      icmpratelimit.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's limits on the ICMP errors a host sends (devplan/network.md §30g).
 * Portable: host-tested.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Limits the ICMP errors an interface sends as Linux does: destination unreachable,
 *        source quench, time exceeded and parameter problem (`icmp_ratemask`), but not
 *        "fragmentation needed", at most one a second to each destination with a burst of 6
 *        (`icmp_ratelimit`), and 1000 a second in all with a burst of 50
 *        (`icmp_msgs_per_sec`, `icmp_msgs_burst`).
 *
 * Without it lwIP answers every datagram to a closed port, so a flood makes the board send a
 * frame for each one it receives: bulk's transmit descriptors taken, and an amplifier for anyone
 * who forges the source.
 *
 * @par Thread Safety
 * Not thread-safe: the owner's lock (the network stack's core lock) covers it.
 */
class IcmpRateLimit {
        public:
                /** @brief Destinations remembered; past it the least recent is forgotten. */
                static constexpr size_t Peers = 16;

                /** @brief Microseconds between errors to one destination. */
                static constexpr int64_t PeerIntervalUs = 1'000'000;

                /** @brief Errors one destination may get at once. */
                static constexpr int64_t PeerBurst = 6;

                /** @brief Errors a second to all destinations. */
                static constexpr int64_t PerSecond = 1000;

                /** @brief Errors at once to all destinations. */
                static constexpr int64_t Burst = 50;

                constexpr IcmpRateLimit() = default;

                /**
                 * @brief Returns whether Ethernet frame @p frame may be sent at @p nowUs (a clock
                 *        that only goes forward): true for anything but an IPv4 ICMP error past
                 *        its limits.
                 * @nonblocking
                 */
                bool allow(Span<const uint8_t> frame, int64_t nowUs);

                /**
                 * @brief Returns whether an ICMP error of @p type and @p code may go to
                 *        @p destination at @p nowUs, taking its share if so.
                 * @nonblocking
                 */
                bool allowError(uint8_t type, uint8_t code, uint32_t destination, int64_t nowUs);

                /** @brief Returns the errors held back. */
                uint32_t limited() const { return _limited; }

        private:
                struct Peer {
                                uint32_t destination = 0;
                                int64_t  lastUs = 0;
                                int64_t  tokensUs = 0; // credit in time, as Linux's xrlim_allow()
                                bool     used = false;
                };

                Peer *peerOf(uint32_t destination, int64_t nowUs);

                Peer     _peers[Peers];
                int64_t  _credit = Burst;
                int64_t  _creditUs = 0;
                bool     _started = false;
                uint32_t _limited = 0;
};

BMDS_NAMESPACE_END
