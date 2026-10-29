/**
 * @file      pagewindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The PSRAM window's bookkeeping (devplan/psramwindow.md §3a, §3b).
 */

#include <bmds/pagewindow.h>

BMDS_NAMESPACE_BEGIN

void PageWindow::reset(uint32_t pages) {
        _pages = pages < MaxPages ? pages : MaxPages;
        for (uint32_t &w : _used) w = 0;
        _usedPages = 0;
        _longest = _pages;
        return;
}

void PageWindow::mark(uint32_t first, uint32_t count, bool used) {
        for (uint32_t p = first; p < first + count; ++p) {
                if (used)
                        _used[p / 32] |= 1u << (p % 32);
                else
                        _used[p / 32] &= ~(1u << (p % 32));
        }
        _usedPages = used ? _usedPages + count : _usedPages - count;
        _longest = findLongest();
        return;
}

uint32_t PageWindow::findLongest() const {
        uint32_t longest = 0;
        uint32_t run = 0;
        for (uint32_t p = 0; p < _pages; ++p) {
                // A whole free word at a time when it can.
                if (p % 32 == 0 && p + 32 <= _pages && _used[p / 32] == 0) {
                        run += 32;
                        p += 31;
                } else {
                        run = isUsed(p) ? 0 : run + 1;
                }
                if (run > longest) longest = run;
        }
        return longest;
}

uint32_t PageWindow::take(uint32_t count, bool fromTop) {
        if (count == 0 || count > _longest) return None;
        uint32_t run = 0;
        if (!fromTop) {
                for (uint32_t p = 0; p < _pages; ++p) {
                        run = isUsed(p) ? 0 : run + 1;
                        if (run == count) {
                                const uint32_t first = p + 1 - count;
                                mark(first, count, true);
                                return first;
                        }
                }
        } else {
                for (uint32_t p = _pages; p-- > 0;) {
                        run = isUsed(p) ? 0 : run + 1;
                        if (run == count) {
                                mark(p, count, true);
                                return p;
                        }
                }
        }
        return None;
}

bool PageWindow::takeAt(uint32_t first, uint32_t count) {
        if (count == 0 || first >= _pages || count > _pages - first) return false;
        for (uint32_t p = first; p < first + count; ++p)
                if (isUsed(p)) return false;
        mark(first, count, true);
        return true;
}

void PageWindow::give(uint32_t first, uint32_t count) {
        if (first >= _pages || count > _pages - first) return;
        mark(first, count, false);
        return;
}

namespace PageRuns {

        size_t gather(Source &source, uint32_t pages, uint32_t pageBytes, Span<Run> out) {
                size_t   n = 0;
                uint32_t left = pages;
                while (left > 0) {
                        if (n == out.size()) {
                                release(source, Span<const Run>(out.data(), n));
                                return 0;
                        }
                        // What the largest block holds: aligned to a page (up to a page lost in
                        // front, for the heap's header) and a line after (into one more page).
                        // One smaller if that fails (a lower bound, or an alignment that didn't
                        // fall well), then halved.
                        const size_t largest = source.largestFree() / pageBytes;
                        uint32_t     k = largest > 2 ? uint32_t(largest) - 2 : 1;
                        if (k > left) k = left;
                        uint8_t *at = source.allocRun(k);
                        if (at == nullptr && k > 1) at = source.allocRun(--k);
                        while (at == nullptr && k > 1) {
                                k /= 2;
                                at = source.allocRun(k);
                        }
                        if (at == nullptr) {
                                release(source, Span<const Run>(out.data(), n));
                                return 0;
                        }
                        out[n].at = at;
                        out[n].pages = k;
                        ++n;
                        left -= k;
                }
                return n;
        }

        void release(Source &source, Span<const Run> runs) {
                for (const Run &r : runs) source.freeRun(r.at);
                return;
        }

} // namespace PageRuns

BMDS_NAMESPACE_END
