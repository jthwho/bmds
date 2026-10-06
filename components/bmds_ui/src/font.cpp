/**
 * @file      font.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/font.h>

BMDS_NAMESPACE_BEGIN

bool Font::find(uint32_t codepoint, Glyph &out) const {
        for (int wide = 0; wide < 2; ++wide) {
                const Span<const Range> runs = wide != 0 ? _wide : _narrow;
                // The last run starting at or before the codepoint.
                size_t lo = 0;
                size_t hi = runs.size();
                while (lo < hi) {
                        const size_t mid = (lo + hi) / 2;
                        if (runs[mid].first <= codepoint) {
                                lo = mid + 1;
                        } else {
                                hi = mid;
                        }
                }
                if (lo == 0) continue;
                const Range &run = runs[lo - 1];
                if (codepoint - run.first >= run.count) continue;
                const size_t bytes = size_t(_height) * (_width / 8) * (wide != 0 ? 2 : 1);
                out.rows = (wide != 0 ? _wideBits : _narrowBits) + (run.index + (codepoint - run.first)) * bytes;
                out.width = static_cast<uint8_t>(wide != 0 ? _width * 2 : _width);
                return true;
        }
        return false;
}

bool Font::hasGlyph(uint32_t codepoint) const {
        Glyph g;
        return find(codepoint, g);
}

Glyph Font::lookup(uint32_t codepoint) const {
        Glyph g;
        if (find(codepoint, g)) return g;
        find(_replacement, g);
        return g;
}

size_t Font::glyphCount() const {
        size_t n = 0;
        for (const Range &r : _narrow) n += r.count;
        for (const Range &r : _wide) n += r.count;
        return n;
}

BMDS_NAMESPACE_END
