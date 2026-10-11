/**
 * @file      multicastfilter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The multicast groups an interface receives (devplan/network.md §15). Portable.
 */

#include <bmds/multicastfilter.h>

BMDS_NAMESPACE_BEGIN

size_t MulticastFilter::find(const MacAddress &group) const {
        for (size_t i = 0; i < _count; ++i)
                if (_groups[i] == group) return i;
        return MaxGroups;
}

Error MulticastFilter::add(const MacAddress &group) {
        if (!group.isMulticast()) return Error::Invalid;
        const size_t i = find(group);
        if (i != MaxGroups) {
                if (_users[i] == UINT16_MAX) return Error::Full;
                ++_users[i];
                return Error::Ok;
        }
        if (_count == MaxGroups) return Error::Full;
        _groups[_count] = group;
        _users[_count] = 1;
        ++_count;
        return Error::Ok;
}

Error MulticastFilter::remove(const MacAddress &group) {
        const size_t i = find(group);
        if (i == MaxGroups) return Error::NotExist;
        if (--_users[i] > 0) return Error::Ok;
        for (size_t j = i + 1; j < _count; ++j) {
                _groups[j - 1] = _groups[j];
                _users[j - 1] = _users[j];
        }
        --_count;
        return Error::Ok;
}

uint16_t MulticastFilter::users(const MacAddress &group) const {
        const size_t i = find(group);
        return i == MaxGroups ? 0 : _users[i];
}

BMDS_NAMESPACE_END
