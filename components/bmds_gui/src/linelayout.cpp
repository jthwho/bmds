/**
 * @file      linelayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/linelayout.h>

BMDS_NAMESPACE_BEGIN

void LineLayout::arrange(Span<const Item> items, uint16_t length, uint16_t spacing, uint16_t unit,
                         Span<uint16_t> position, Span<uint16_t> size) {
        const size_t n = items.size() < position.size() && items.size() < size.size()
                                 ? items.size()
                                 : (position.size() < size.size() ? position.size() : size.size());
        if (n == 0) return;
        if (unit == 0) unit = 1;
        const auto down = [unit](uint32_t v) {
                return v - v % unit;
        };
        const auto up = [unit](uint32_t v) {
                return (v + unit - 1) / unit * unit;
        };
        int64_t used = int64_t(spacing) * int64_t(n - 1);
        for (size_t i = 0; i < n; ++i) {
                uint32_t       s = up(items[i].hint);
                const uint32_t most = items[i].most < s ? down(items[i].most) : s;
                size[i] = static_cast<uint16_t>(most);
                used += size[i];
        }
        if (used < length) {
                // Room over: by stretch, in units, none past its most.
                uint32_t extra = down(static_cast<uint32_t>(length - used));
                while (extra >= unit) {
                        uint32_t stretch = 0;
                        for (size_t i = 0; i < n; ++i)
                                if (items[i].stretch != 0 && size[i] + unit <= items[i].most)
                                        stretch += items[i].stretch;
                        if (stretch == 0) break;
                        uint32_t given = 0;
                        for (size_t i = 0; i < n; ++i) {
                                if (items[i].stretch == 0 || size[i] + unit > items[i].most) continue;
                                uint32_t       share = down(extra * items[i].stretch / stretch);
                                const uint32_t room = down(items[i].most - size[i]);
                                if (share > room) share = room;
                                size[i] = static_cast<uint16_t>(size[i] + share);
                                given += share;
                        }
                        if (given == 0) {
                                // Less than a unit each: one at a time, from the first.
                                for (size_t i = 0; i < n && extra - given >= unit; ++i) {
                                        if (items[i].stretch == 0 || size[i] + unit > items[i].most) continue;
                                        size[i] = static_cast<uint16_t>(size[i] + unit);
                                        given += unit;
                                }
                        }
                        extra -= given;
                }
        } else if (used > length) {
                // Room missing: from the stretching items, then from the end.
                uint32_t missing = static_cast<uint32_t>(used - length);
                for (int pass = 0; pass < 2 && missing > 0; ++pass) {
                        for (size_t k = n; k-- > 0 && missing > 0;) {
                                if (pass == 0 && items[k].stretch == 0) continue;
                                const uint32_t want = up(missing);
                                const uint32_t take = size[k] < want ? size[k] : want;
                                size[k] = static_cast<uint16_t>(size[k] - take);
                                missing = take >= missing ? 0 : missing - take;
                        }
                }
        }
        uint32_t at = 0;
        for (size_t i = 0; i < n; ++i) {
                position[i] = static_cast<uint16_t>(at < length ? at : length);
                if (position[i] + size[i] > length) size[i] = static_cast<uint16_t>(length - position[i]);
                at += size[i] + spacing;
        }
        return;
}

BMDS_NAMESPACE_END
