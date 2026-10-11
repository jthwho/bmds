/**
 * @file      ntpclient.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_net: the kernel's NTP client (devplan/network.md §10, §22),
 * on lwIP's raw UDP API and timers in the network task, disciplining the
 * system clock (Clock::set, adjust, setFrequency; source ntp).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

#include "netnames.h"

BMDS_NAMESPACE_BEGIN

namespace NtpClient {

        /** @brief Servers at once: `net/ntp/servers`' 4 and DHCP's 2. */
        constexpr size_t MaxServers = 6;

        /** @brief Servers from the setting. */
        constexpr size_t MaxSettingServers = 4;

        /** @brief One server, as `ntp` shows it. */
        struct ServerStatus {
                        char     name[NetNames::MaxHostName + 1] = {};
                        bool     fromDhcp = false;
                        bool     resolving = false; ///< Its name is being looked up.
                        bool     denied = false;    ///< It sent `DENY` or `RSTR`: not asked again.
                        bool     chosen = false;
                        uint32_t address = 0; ///< Network order; 0 until resolved.
                        uint8_t  stratum = 0;
                        uint8_t  reach = 0;          ///< The last 8 polls, newest in bit 0.
                        uint8_t  samples = 0;        ///< In its filter.
                        int64_t  sinceAnswerNs = -1; ///< Since its last answer (-1: none).
                        int64_t  offsetNs = 0;       ///< Its filter's best.
                        int64_t  delayNs = 0;        ///< See `offsetNs`.
                        int64_t  jitterNs = 0;
        };

        /** @brief The client's state, as `ntp` and the counters show it. */
        struct Status {
                        bool         enabled = false;
                        bool         synced = false; ///< The clock follows a server now.
                        int          poll = 0;       ///< The poll exponent (2^poll s).
                        uint8_t      stratum = 0;    ///< The chosen server's.
                        int64_t      offsetNs = 0;   ///< The last offset applied.
                        int64_t      delayNs = 0;    ///< Its sample's delay.
                        int64_t      jitterNs = 0;   ///< The chosen server's jitter.
                        uint32_t     requests = 0;
                        uint32_t     replies = 0;  ///< Used.
                        uint32_t     rejected = 0; ///< Not ours, not a server's, unsynchronised, kisses.
                        size_t       count = 0;
                        ServerStatus servers[MaxServers];
        };

        /**
         * @brief Declares `net/ntp/enabled` and `net/ntp/servers`, watches them and
         *        adds the `net.ntp` counters. After NetNames; without the core lock.
         * @return `Ok`, or the error declaring.
         */
        Error init();

        /** @brief Network task, core lock held: applies what changed (settings, servers, addresses). */
        void poll();

        /** @brief An interface's DHCP servers changed (core lock held). */
        void serversChanged();

        /** @brief An interface's address changed (core lock held): a burst once it has one. */
        void addressChanged();

        /** @brief Asks every server now, 4 times 2 s apart (`ntp poll`). Takes the core lock. */
        Error burst();

        /** @brief Fills @p out. Takes the core lock. */
        void status(Status &out);

} // namespace NtpClient

BMDS_NAMESPACE_END
