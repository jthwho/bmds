/**
 * @file      region.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/region.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Two disjoint rectangles that make one together: a shared full edge.
        bool canMerge(const Rect &a, const Rect &b) {
                if (a.y == b.y && a.height == b.height) return a.right() == b.x || b.right() == a.x;
                if (a.x == b.x && a.width == b.width) return a.bottom() == b.y || b.bottom() == a.y;
                return false;
        }

        // The plan's working space: rectangles still to be covered by lower layers.
        constexpr size_t PlanWork = 128;

} // namespace

size_t subtract(const Rect &a, const Rect &b, Rect (&out)[4]) {
        if (a.isEmpty()) return 0;
        if (!a.intersects(b)) {
                out[0] = a;
                return 1;
        }
        const Rect i = a.intersected(b);
        size_t     n = 0;
        if (i.y > a.y) out[n++] = Rect{a.x, a.y, a.width, uint16_t(i.y - a.y)};
        if (i.bottom() < a.bottom())
                out[n++] = Rect{a.x, uint16_t(i.bottom()), a.width, uint16_t(a.bottom() - i.bottom())};
        if (i.x > a.x) out[n++] = Rect{a.x, i.y, uint16_t(i.x - a.x), i.height};
        if (i.right() < a.right()) out[n++] = Rect{uint16_t(i.right()), i.y, uint16_t(a.right() - i.right()), i.height};
        return n;
}

Rect Region::bounds() const {
        Rect b;
        for (size_t i = 0; i < _count; ++i) b = b.united(_rects[i]);
        return b;
}

uint32_t Region::area() const {
        uint32_t a = 0;
        for (size_t i = 0; i < _count; ++i) a += _rects[i].area();
        return a;
}

void Region::add(const Rect &rect) {
        if (rect.isEmpty()) return;
        // Rectangles the new one holds go; the rest of it is cut around the others.
        size_t kept = 0;
        for (size_t i = 0; i < _count; ++i) {
                if (!rect.contains(_rects[i])) _rects[kept++] = _rects[i];
        }
        _count = static_cast<uint8_t>(kept);

        Rect   pieces[MaxRects];
        size_t count = 1;
        pieces[0] = rect;
        for (size_t i = 0; i < _count && count > 0; ++i) {
                Rect   next[MaxRects];
                size_t nextCount = 0;
                for (size_t p = 0; p < count; ++p) {
                        Rect         cut[4];
                        const size_t n = subtract(pieces[p], _rects[i], cut);
                        if (nextCount + n > MaxRects) {
                                collapse(rect);
                                return;
                        }
                        for (size_t k = 0; k < n; ++k) next[nextCount++] = cut[k];
                }
                for (size_t p = 0; p < nextCount; ++p) pieces[p] = next[p];
                count = nextCount;
        }
        if (_count + count > MaxRects) {
                collapse(rect);
                return;
        }
        for (size_t p = 0; p < count; ++p) _rects[_count++] = pieces[p];
        merge();
        return;
}

void Region::add(const Region &other) {
        for (const Rect &r : other.rects()) add(r);
        return;
}

void Region::collapse(const Rect &extra) {
        _rects[0] = bounds().united(extra);
        _count = 1;
        return;
}

void Region::merge() {
        bool merged = true;
        while (merged) {
                merged = false;
                for (size_t i = 0; i < _count && !merged; ++i) {
                        for (size_t j = i + 1; j < _count; ++j) {
                                if (!canMerge(_rects[i], _rects[j])) continue;
                                _rects[i] = _rects[i].united(_rects[j]);
                                _rects[j] = _rects[--_count];
                                merged = true;
                                break;
                        }
                }
        }
        return;
}

Result<size_t> planCompose(Span<const Rect> layers, const Region &damage, Span<ComposePiece> out) {
        Rect   work[2][PlanWork];
        Rect  *open = work[0];
        Rect  *next = work[1];
        size_t openCount = 0;
        for (const Rect &r : damage.rects()) open[openCount++] = r;

        size_t pieces = 0;
        for (size_t layer = 0; layer < layers.size() && openCount > 0; ++layer) {
                const Rect &l = layers[layer];
                size_t      nextCount = 0;
                for (size_t i = 0; i < openCount; ++i) {
                        const Rect piece = open[i].intersected(l);
                        if (piece.isEmpty()) {
                                next[nextCount++] = open[i];
                                continue;
                        }
                        if (pieces == out.size()) return makeError<size_t>(Error::Full);
                        out[pieces++] = ComposePiece{static_cast<uint16_t>(layer), piece};
                        Rect         rest[4];
                        const size_t n = subtract(open[i], l, rest);
                        if (nextCount + n > PlanWork) return makeError<size_t>(Error::Full);
                        for (size_t k = 0; k < n; ++k) next[nextCount++] = rest[k];
                }
                Rect *t = open;
                open = next;
                next = t;
                openCount = nextCount;
        }
        return makeResult(pieces);
}

BMDS_NAMESPACE_END
