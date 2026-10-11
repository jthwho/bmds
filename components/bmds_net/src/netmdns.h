/**
 * @file      netmdns.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_net: multicast DNS (devplan/network.md §8, §22): lwIP's
 * responder answering for ${HOSTNAME}.local on each Ethernet interface,
 * following system/hostname and net/mdns/enabled live.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace NetMdns {

        /** @brief A name the responder answers for, without `.local`. */
        constexpr size_t MaxName = 63;

        /**
         * @brief Starts lwIP's responder, declares `net/mdns/enabled` and watches it
         *        and `system/hostname`. After the interfaces; without the core lock.
         * @return `Ok`, or the error declaring.
         */
        Error init();

        /** @brief Network task, core lock held: applies what changed (settings, a conflict). */
        void poll();

        /**
         * @brief Writes the name interface @p position answers for (without
         *        `.local`) into @p out. Core lock held.
         * @return False if mDNS is off on it.
         */
        bool name(size_t position, char (&out)[MaxName + 1]);

        /** @brief Returns true if @p name is one of ours with `.local` (core lock held). */
        bool isOwnName(StringView name);

        /**
         * @brief Advertises `_ssh._tcp` on @p port (0: not), on every interface the
         *        responder runs on (the SSH server's, devplan/network.md §24). Any task.
         */
        void setSshPort(uint16_t port);

} // namespace NetMdns

BMDS_NAMESPACE_END
