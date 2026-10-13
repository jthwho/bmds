/**
 * @file      ptpservice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_net: the kernel's PTP service (devplan/network.md §31c): one ordinary clock's
 * port (bmds/ptp.h) on real-time flows, run by a task on core 1, and the system clock and
 * ptpclock0 following each other.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/netflow.h>
#include <bmds/ptp.h>

BMDS_NAMESPACE_BEGIN

namespace PtpService {

        /** @brief Which clock follows which (devplan/network.md §31c). */
        enum class Follow : uint8_t {
                None,          ///< Both run free.
                SystemFollows, ///< The system clock follows ptpclock0 (source ptp).
                PtpFollows,    ///< ptpclock0 follows the system clock (a master, NTP synced).
        };

        /** @brief The UTC offset assumed while no master gives a valid one (step 4: the leap-second file). */
        constexpr int16_t DefaultUtcOffset = 37;

        /** @brief The service's state, as `ptp` shows it. */
        struct Status {
                        bool               enabled = false;
                        bool               running = false; ///< The port runs on its flows.
                        Error              error;           ///< Why it doesn't, when enabled.
                        char               interface[16] = {};
                        bool               l2 = false; ///< Ethernet, else UDP.
                        bool               defaultProfile = false;
                        int64_t            stateForNs = 0; ///< Since the port's last change of state.
                        Ptp::PortIdentity  identity;
                        Ptp::Port::Config  config;
                        Ptp::Port::Status  port;
                        Ptp::Port::Counts  counts;
                        Ptp::Port::Foreign foreign[Ptp::Port::MaxForeign];
                        size_t             foreignCount = 0;
                        Follow             follow = Follow::None;
                        int64_t            followOffsetNs = 0;  ///< The followed clock's last offset.
                        uint32_t           followBracketNs = 0; ///< The two clocks read within this.
                        int16_t            utcOffset = DefaultUtcOffset;
                        bool               utcFromMaster = false;
                        bool               arbitraryTimescale = false; ///< The master's time isn't TAI: taken as UTC.
                        NetFlow::Counters  flows;                      ///< Its flows' together.
        };

        /**
         * @brief Declares the `net/ptp` settings, watches them and adds the `net.ptp` counters; the task
         *        starts the first time PTP is enabled. After NtpClient; without the core lock.
         */
        Error init();

        /** @brief Fills @p out. @blocking Briefly (the service's lock). */
        void status(Status &out);

        /** @brief Returns true while PTP has the system clock (NTP leaves it alone). @nonblocking */
        bool ownsSystemClock();

} // namespace PtpService

BMDS_NAMESPACE_END
