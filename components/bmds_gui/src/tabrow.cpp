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

        Parts parts(uint16_t titleWidth, uint16_t badgeWidth, bool closable) {
                Parts    p;
                uint32_t x = uint32_t(Pad) + titleWidth;
                if (badgeWidth > 0) {
                        const uint32_t pill = uint32_t(badgeWidth) + 2u * BadgePad;
                        p.badge = uint16_t(x + BadgeGap);
                        p.badgeWidth = uint16_t(pill);
                        x += BadgeGap + pill;
                }
                x += closable ? CloseGap + CloseSize + CloseMargin : Pad;
                x = (x + 3u) & ~3u;
                p.width = x > 0xfffcu ? uint16_t(0xfffc) : uint16_t(x);
                if (closable) p.close = uint16_t(p.width - CloseMargin - CloseSize);
                return p;
        }

        uint32_t placeAll(Span<const uint16_t> widths, Span<uint32_t> lefts) {
                uint32_t x = 0;
                for (size_t i = 0; i < widths.size() && i < lefts.size(); ++i) {
                        lefts[i] = x;
                        x += widths[i];
                }
                return x;
        }

        Bar bar(uint32_t content, uint32_t rowWidth) {
                Bar b;
                if (content <= rowWidth) {
                        b.view = rowWidth;
                        return b;
                }
                b.buttons = true;
                const uint32_t buttons = 3u * ButtonWidth;
                b.view = rowWidth > buttons ? (rowWidth - buttons) & ~3u : 0;
                return b;
        }

        uint32_t maxScroll(uint32_t content, const Bar &bar) {
                return content > bar.view ? content - bar.view : 0;
        }

        uint32_t scrollToShow(uint32_t scroll, uint32_t left, uint16_t width, uint32_t content, const Bar &bar) {
                uint32_t s = scroll;
                if (left < scroll || width >= bar.view)
                        s = left;
                else if (left + width > scroll + bar.view)
                        s = left + width - bar.view;
                const uint32_t most = maxScroll(content, bar);
                return s < most ? s : most;
        }

        uint32_t scrollBy(uint32_t scroll, Span<const uint32_t> lefts, uint32_t content, const Bar &bar, int by) {
                const uint32_t most = maxScroll(content, bar);
                uint32_t       s = scroll < most ? scroll : most;
                for (; by > 0 && s < most; --by) {
                        uint32_t next = most;
                        for (size_t i = 0; i < lefts.size(); ++i)
                                if (lefts[i] > s) {
                                        next = lefts[i];
                                        break;
                                }
                        s = next < most ? next : most;
                }
                for (; by < 0 && s > 0; ++by) {
                        uint32_t before = 0;
                        for (size_t i = lefts.size(); i > 0; --i)
                                if (lefts[i - 1] < s) {
                                        before = lefts[i - 1];
                                        break;
                                }
                        s = before;
                }
                return s;
        }

        Hit hit(Span<const uint32_t> lefts, Span<const uint16_t> widths, Span<const uint16_t> closes, uint32_t scroll,
                const Bar &bar, int x) {
                Hit h;
                if (x < 0) return h;
                if (uint32_t(x) >= bar.view) {
                        if (!bar.buttons) return h;
                        const uint32_t b = (uint32_t(x) - bar.view) / ButtonWidth;
                        if (b < 3) h.kind = b == 0 ? Hit::Kind::Back : b == 1 ? Hit::Kind::Forward : Hit::Kind::List;
                        return h;
                }
                // The last tab starting at or before the point (the lefts rise).
                const uint32_t c = scroll + uint32_t(x);
                const size_t   n = lefts.size() < widths.size() ? lefts.size() : widths.size();
                size_t         lo = 0, hi = n;
                while (lo < hi) {
                        const size_t mid = (lo + hi) / 2;
                        if (lefts[mid] <= c)
                                lo = mid + 1;
                        else
                                hi = mid;
                }
                if (lo == 0) return h;
                const size_t i = lo - 1;
                if (c >= lefts[i] + widths[i]) return h;
                h.tab = int(i);
                h.kind = Hit::Kind::Tab;
                const uint32_t in = c - lefts[i];
                if (i < closes.size() && closes[i] != 0 && in >= closes[i] && in < uint32_t(closes[i]) + CloseSize)
                        h.kind = Hit::Kind::Close;
                return h;
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
