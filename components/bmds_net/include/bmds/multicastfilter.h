/**
 * @file      multicastfilter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The multicast groups an interface receives, by MAC address (devplan/network.md
 * §6, §15). Portable: host-tested.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/ethernet.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The group addresses an interface must receive, each with a count of
 *        its users: IP groups share MAC addresses (32 to one for IPv4), so a
 *        MAC address stays until its last user leaves.
 *
 * The list is kept in the order the groups were first joined, as one span
 * the driver's filters take (`EthernetDevice::setMulticast()`).
 *
 * @par Thread Safety
 * Not thread-safe: the owner's lock (the network stack's core lock) covers it.
 */
class MulticastFilter {
        public:
                /** @brief Distinct group addresses kept. */
                static constexpr size_t MaxGroups = 32;

                constexpr MulticastFilter() = default;

                /**
                 * @brief Adds a user of @p group.
                 * @return `Ok`; `Invalid` for an address that isn't a group's; `Full`.
                 */
                Error add(const MacAddress &group);

                /**
                 * @brief Takes a user of @p group away; the last one takes the group out.
                 * @return `Ok`, or `NotExist`.
                 */
                Error remove(const MacAddress &group);

                /** @brief Returns the groups, in the order they were first joined. */
                Span<const MacAddress> groups() const { return Span<const MacAddress>(_groups, _count); }

                /** @brief Returns @p group's users (0 if it isn't in the list). */
                uint16_t users(const MacAddress &group) const;

        private:
                size_t find(const MacAddress &group) const;

                MacAddress _groups[MaxGroups] = {};
                uint16_t   _users[MaxGroups] = {};
                size_t     _count = 0;
};

BMDS_NAMESPACE_END
