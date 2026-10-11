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
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace NetNames {

        /** @brief The longest name in `net/hosts` (and an NTP server's). */
        constexpr size_t MaxHostName = 63;

        /** @brief Returns true for a list's separator: ';', ',' or a blank. */
        inline bool isSeparator(char c) {
                return c == ';' || c == ',' || c == ' ' || c == '\t';
        }

        /** @brief Calls @p fn with each item of a list separated by ';', ',' or blanks; false stops it. */
        template <typename Fn> bool forEachItem(StringView list, Fn &&fn) {
                size_t at = 0;
                while (at < list.size()) {
                        while (at < list.size() && isSeparator(list[at])) ++at;
                        size_t end = at;
                        while (end < list.size() && !isSeparator(list[end])) ++end;
                        if (end > at && !fn(list.mid(at, end - at))) return false;
                        at = end;
                }
                return true;
        }

        /** @brief Returns true for a name of letters, digits, '-', '_' and '.', 1 to `MaxHostName` long. */
        bool isHostName(StringView name);

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
