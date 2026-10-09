/**
 * @file      tabrow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A tab widget's row of tabs (devplan/mediaplayer.md §6d).
 */

#include <bmds/tabrow.h>

BMDS_NAMESPACE_BEGIN

namespace TabRow {

        uint32_t place(Span<const uint16_t> textWidths, uint16_t start, Span<uint16_t> lefts, Span<uint16_t> widths) {
                uint32_t x = start;
                for (size_t i = 0; i < textWidths.size() && i < lefts.size() && i < widths.size(); ++i) {
                        const uint32_t w = (uint32_t(textWidths[i]) + 2u * Pad + 3u) & ~3u;
                        lefts[i] = x > UINT16_MAX ? UINT16_MAX : uint16_t(x);
                        widths[i] = w > UINT16_MAX ? UINT16_MAX : uint16_t(w);
                        x += w;
                }
                return x;
        }

        int at(Span<const uint16_t> lefts, Span<const uint16_t> widths, int x) {
                for (size_t i = 0; i < lefts.size() && i < widths.size(); ++i)
                        if (x >= int(lefts[i]) && x < int(lefts[i]) + int(widths[i])) return int(i);
                return -1;
        }

        size_t step(size_t current, size_t count, int by) {
                if (count == 0) return 0;
                const long n = long(count);
                long       to = (long(current % count) + by) % n;
                if (to < 0) to += n;
                return size_t(to);
        }

} // namespace TabRow

BMDS_NAMESPACE_END
