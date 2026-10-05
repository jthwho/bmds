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
        if (rect.isEmpty() || tryAdd(rect)) return;
        // Too many rectangles: the cheapest neighbours merged, then once more.
        squeeze();
        if (!tryAdd(rect)) collapse(rect);
        return;
}

// Adds @p rect if its pieces fit. Rectangles it holds go either way (the
// caller covers it after a failure).
bool Region::tryAdd(const Rect &rect) {
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
                        if (nextCount + n > MaxRects) return false;
                        for (size_t k = 0; k < n; ++k) next[nextCount++] = cut[k];
                }
                for (size_t p = 0; p < nextCount; ++p) pieces[p] = next[p];
                count = nextCount;
        }
        if (_count + count > MaxRects) return false;
        for (size_t p = 0; p < count; ++p) _rects[_count++] = pieces[p];
        merge();
        return true;
}

// Down to half the rectangles: each time the two whose bounding box wastes
// the least area become that box (others it overlaps cut around it), so the
// region stays close to what was added.
void Region::squeeze() {
        for (size_t guard = 0; guard < 2 * MaxRects && _count > MaxRects / 2; ++guard) {
                size_t   bi = 0, bj = 1;
                uint32_t best = UINT32_MAX;
                for (size_t i = 0; i < _count; ++i) {
                        for (size_t j = i + 1; j < _count; ++j) {
                                const uint32_t waste =
                                        _rects[i].united(_rects[j]).area() - _rects[i].area() - _rects[j].area();
                                if (waste >= best) continue;
                                best = waste;
                                bi = i;
                                bj = j;
                        }
                }
                const Rect u = _rects[bi].united(_rects[bj]);
                Rect       kept[MaxRects];
                size_t     n = 0;
                for (size_t i = 0; i < _count; ++i) {
                        if (i == bi || i == bj || u.contains(_rects[i])) continue;
                        Rect         cut[4];
                        const size_t pieces = subtract(_rects[i], u, cut);
                        if (n + pieces + 1 > MaxRects) {
                                collapse(u);
                                return;
                        }
                        for (size_t k = 0; k < pieces; ++k) kept[n++] = cut[k];
                }
                kept[n++] = u;
                for (size_t i = 0; i < n; ++i) _rects[i] = kept[i];
                _count = static_cast<uint8_t>(n);
        }
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

BMDS_NAMESPACE_END
