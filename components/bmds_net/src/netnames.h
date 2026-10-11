/**
 * @file      netnames.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_net: names (devplan/network.md §8, §18): the DNS servers
 * lwIP asks (net/dns/servers, else DHCP's), the local names (localhost, the
 * host name, net/hosts), and the files of /etc built from them.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

namespace NetNames {

        /**
         * @brief Declares `net/dns/servers`, `net/dns/search` and `net/hosts`, watches
         *        them, and adds `/etc`'s files. After the interfaces; without the core lock.
         * @return `Ok`, or the error declaring.
         */
        Error init();

        /** @brief Network task, core lock held: the settings, when they changed; DHCP's servers. */
        void poll();

        /** @brief An interface's DHCP servers changed (core lock held): `poll()` applies them. */
        void serversChanged();

} // namespace NetNames

BMDS_NAMESPACE_END
