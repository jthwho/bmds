/**
 * @file      timesync.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What sets the clock, at a glance (`devplan/controlpanel.md` §7):
 *        NTP's and PTP's state as summaries, read-only, for applications that
 *        show it (Settings' Date & Time). `ntp` and `ptp` show everything.
 *
 * @par Thread Safety
 * Any task. Each call takes its service's lock briefly. @blocking
 */
namespace TimeSync {

        /** @brief NTP's state (devplan/network.md §26). */
        struct Ntp {
                        bool    enabled = false;    ///< `net/ntp/enabled`.
                        bool    synced = false;     ///< The clock follows a server now.
                        uint8_t stratum = 0;        ///< The chosen server's.
                        uint8_t servers = 0;        ///< Servers known (the setting's and DHCP's).
                        uint8_t reachable = 0;      ///< Of them, those that answered lately.
                        int     pollLog2 = 0;       ///< Seconds between polls, as a power of two.
                        int64_t offsetNs = 0;       ///< The last offset applied.
                        int64_t delayNs = 0;        ///< Its round trip.
                        int64_t jitterNs = 0;       ///< The chosen server's.
                        int64_t sinceAnswerNs = -1; ///< Since the chosen server last answered (-1: none).
                        char    server[64] = {};    ///< The chosen server's name ("" if none).
        };

        /** @brief Returns NTP's state (all false before the network starts). */
        Ntp ntp();

        /** @brief PTP's state (devplan/network.md §31). */
        struct Ptp {
                        bool        enabled = false;    ///< `net/ptp/enabled`.
                        bool        running = false;    ///< Its port runs.
                        Error       error;              ///< Why it doesn't, when enabled.
                        char        interface[16] = {}; ///< The interface it runs on.
                        const char *state = "";         ///< The port's: "listening", "slave", "master", ...
                        int64_t     stateForNs = 0;     ///< Since the port last changed state.
                        bool        haveOffset = false; ///< A slave measures `offsetNs`.
                        int64_t     offsetNs = 0;       ///< Our time less the master's.
                        int64_t     pathDelayNs = 0;    ///< The mean path delay.
                        char        master[24] = {};    ///< The parent's clock identity ("" as master).
                        char        grandmaster[24] = {};
                        bool        systemFollows = false; ///< The system clock follows PTP's.
                        bool        ptpFollows = false;    ///< PTP's clock follows the system clock (a master).
                        int16_t     utcOffset = 0;         ///< TAI less UTC, in seconds.
        };

        /** @brief Returns PTP's state (all false before it is first enabled). */
        Ptp ptp();

} // namespace TimeSync

BMDS_NAMESPACE_END
