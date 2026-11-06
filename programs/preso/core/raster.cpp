/**
 * @file      raster.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Filling is a row at a time: each edge crossing the row adds the signed area it covers to the
 * row's cells (font-rs's accumulation, cut to the row), and a running sum along the row is each
 * pixel's coverage. Only a row's buffer is needed, whatever the shape's size.
 */

#include "raster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace preso {

        // ---- Affine

        Affine Affine::after(const Affine &first) const {
                Affine r;
                r.a = a * first.a + c * first.b;
                r.b = b * first.a + d * first.b;
                r.c = a * first.c + c * first.d;
                r.d = b * first.c + d * first.d;
                r.e = a * first.e + c * first.f + e;
                r.f = b * first.e + d * first.f + f;
                return r;
        }

        bool Affine::inverted(Affine *out) const {
                float det = a * d - b * c;
                if (fabsf(det) < 1e-9f) return false;
                float k = 1.0f / det;
                out->a = d * k;
                out->b = -b * k;
                out->c = -c * k;
                out->d = a * k;
                out->e = -(out->a * e + out->c * f);
                out->f = -(out->b * e + out->d * f);
                return true;
        }

        Affine Affine::rotation(float degrees) {
                float r = degrees * 3.14159265f / 180.0f;
                float s = sinf(r), c = cosf(r);
                return Affine{c, s, -s, c, 0, 0};
        }

        // ---- Path

        Path::~Path() {
                free(_points);
                free(_starts);
        }

        bool Path::grow(uint32_t points) {
                if (_count + points <= _capacity) return true;
                uint32_t capacity = _capacity != 0 ? _capacity : 64;
                while (capacity < _count + points) capacity *= 2;
                auto *p = static_cast<Point *>(realloc(_points, capacity * sizeof(Point)));
                if (p == nullptr) {
                        _failed = true;
                        return false;
                }
                _points = p;
                _capacity = capacity;
                return true;
        }

        void Path::moveTo(Point p) {
                if (_outlines == _startCapacity) {
                        uint32_t capacity = _startCapacity != 0 ? _startCapacity * 2 : 8;
                        auto    *s = static_cast<uint32_t *>(realloc(_starts, capacity * sizeof(uint32_t)));
                        if (s == nullptr) {
                                _failed = true;
                                return;
                        }
                        _starts = s;
                        _startCapacity = capacity;
                }
                if (!grow(1)) return;
                _starts[_outlines++] = _count;
                _points[_count++] = p;
                return;
        }

        void Path::lineTo(Point p) {
                if (_outlines == 0) {
                        moveTo(p);
                        return;
                }
                if (!grow(1)) return;
                _points[_count++] = p;
                return;
        }

        void Path::clear() {
                _count = 0;
                _outlines = 0;
                _failed = false;
                return;
        }

        void Path::addRect(float x, float y, float width, float height) {
                moveTo(Point{x, y});
                lineTo(Point{x + width, y});
                lineTo(Point{x + width, y + height});
                lineTo(Point{x, y + height});
                return;
        }

        uint32_t circleSides(float r) {
                // A side about 3 pixels long, at least 8 of them.
                uint32_t n = uint32_t(2.0f * 3.14159265f * fabsf(r) / 3.0f);
                return n < 8 ? 8 : n > 256 ? 256 : n;
        }

        void ellipsePoints(Point *out, uint32_t n, float cx, float cy, float rx, float ry, float start, bool reverse) {
                if (n == 0) return;
                float step = 2.0f * 3.14159265f / float(n) * (reverse ? -1.0f : 1.0f);
                float cs = cosf(step), sn = sinf(step);
                float c = cosf(start), s = sinf(start);
                for (uint32_t i = 0; i < n; i++) {
                        out[i] = Point{cx + rx * c, cy + ry * s};
                        float nc = c * cs - s * sn;
                        s = s * cs + c * sn;
                        c = nc;
                }
                return;
        }

        void Path::addEllipse(float cx, float cy, float rx, float ry) {
                uint32_t n = circleSides(rx > ry ? rx : ry);
                if (!grow(n + 1)) return;
                moveTo(Point{cx + rx, cy});
                _count--; // the first point is ellipsePoints()' too
                ellipsePoints(_points + _count, n, cx, cy, rx, ry, 0, false);
                _count += n;
                return;
        }

        void Path::addPolygon(const Point *points, uint32_t count) {
                if (count < 3 || !grow(count)) return;
                moveTo(points[0]);
                for (uint32_t i = 1; i < count; i++) lineTo(points[i]);
                return;
        }

        void Path::addSolid(const Point *points, uint32_t count) {
                if (count < 3 || !grow(count)) return;
                // A stroke's quads have a negative signed area (shoelace, y down): so this.
                float area = 0;
                for (uint32_t i = 0; i < count; i++) {
                        const Point &p = points[i], &q = points[(i + 1) % count];
                        area += p.x * q.y - q.x * p.y;
                }
                if (area <= 0) {
                        addPolygon(points, count);
                        return;
                }
                moveTo(points[count - 1]);
                for (uint32_t i = count - 1; i-- > 0;) lineTo(points[i]);
                return;
        }

        void Path::addStroke(const Point *points, uint32_t count, bool closed, float width) {
                if (count == 0 || width <= 0) return;
                float hw = width / 2;
                // Every piece turns the same way, so where they overlap they add up (and are cut at
                // full coverage) rather than cancel.
                uint32_t discSides = circleSides(hw) / 2;
                if (discSides < 6) discSides = 6;
                if (discSides > 64) discSides = 64;
                Point disc0[64];
                ellipsePoints(disc0, discSides, 0, 0, hw, hw, 0, true);
                auto disc = [&](Point c) {
                        for (uint32_t i = 0; i < discSides; i++) {
                                Point p{c.x + disc0[i].x, c.y + disc0[i].y};
                                if (i == 0) {
                                        moveTo(p);
                                } else {
                                        lineTo(p);
                                }
                        }
                };
                uint32_t segments = closed ? count : count - 1;
                for (uint32_t i = 0; i < segments; i++) {
                        Point a = points[i], b = points[(i + 1) % count];
                        float dx = b.x - a.x, dy = b.y - a.y;
                        float length = sqrtf(dx * dx + dy * dy);
                        if (length < 1e-6f) continue;
                        float nx = -dy / length * hw, ny = dx / length * hw;
                        moveTo(Point{a.x + nx, a.y + ny});
                        lineTo(Point{b.x + nx, b.y + ny});
                        lineTo(Point{b.x - nx, b.y - ny});
                        lineTo(Point{a.x - nx, a.y - ny});
                }
                // Round joins where it turns enough to show a gap, and round ends (a point alone is a
                // dot). A circle's many small turns need none.
                if (width < 2.5f && count > 1) return;
                // A turn leaves a wedge about half the width times its angle wide: under a third of a
                // pixel, the quads' overlap covers it.
                float straight = cosf(fminf(0.35f, 0.3f / hw));
                for (uint32_t i = 0; i < count; i++) {
                        bool end = !closed && (i == 0 || i == count - 1);
                        if (!end && count > 2) {
                                Point p = points[(i + count - 1) % count], c = points[i], q = points[(i + 1) % count];
                                float ax = c.x - p.x, ay = c.y - p.y, bx = q.x - c.x, by = q.y - c.y;
                                float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
                                if (la > 0 && lb > 0 && (ax * bx + ay * by) > straight * la * lb) continue;
                        }
                        disc(points[i]);
                }
                return;
        }

        void Path::transform(const Affine &t) {
                for (uint32_t i = 0; i < _count; i++) _points[i] = t.apply(_points[i]);
                return;
        }

        bool Path::bounds(float *x0, float *y0, float *x1, float *y1) const {
                if (_count == 0) return false;
                float l = _points[0].x, r = l, t = _points[0].y, b = t;
                for (uint32_t i = 1; i < _count; i++) {
                        const Point &p = _points[i];
                        l = p.x < l ? p.x : l;
                        r = p.x > r ? p.x : r;
                        t = p.y < t ? p.y : t;
                        b = p.y > b ? p.y : b;
                }
                *x0 = l;
                *y0 = t;
                *x1 = r;
                *y1 = b;
                return true;
        }

        // ---- Blending a pixel

        namespace {

                // RGB565 spread so all three channels mix in one multiply (green 21-26, red 11-15,
                // blue 0-4), at 32 levels of opacity.
                inline uint32_t spread565(uint32_t p) {
                        return (p | (p << 16)) & 0x07E0F81Fu;
                }
                inline uint16_t mix565(uint16_t under, uint32_t over, uint32_t a32) {
                        uint32_t u = spread565(under);
                        uint32_t m = ((((over - u) * a32) >> 5) + u) & 0x07E0F81Fu;
                        return uint16_t(m | (m >> 16));
                }

                inline int32_t towards(int32_t from, int32_t to, int32_t a) {
                        return from + (((to - from) * a + 128) >> 8);
                }

                inline void blendPixel(uint8_t *row, PixelFormat format, int32_t x, Color color, uint32_t coverage) {
                        if (coverage == 0) return;
                        int32_t a = int32_t(coverage == 255 ? 256 : coverage);
                        int32_t r = int32_t((color >> 16) & 0xFF), g = int32_t((color >> 8) & 0xFF),
                                b = int32_t(color & 0xFF);
                        if (format == PixelFormat::Rgb565) {
                                auto    *q = reinterpret_cast<uint16_t *>(row) + x;
                                uint32_t c = uint32_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                                *q = a >= 256 ? uint16_t(c) : mix565(*q, spread565(c), uint32_t(a + 4) >> 3);
                        } else {
                                auto   *q = reinterpret_cast<uint32_t *>(row) + x;
                                int32_t p = int32_t(*q & 0xFFFFFF);
                                int32_t pr = towards((p >> 16) & 0xFF, r, a), pg = towards((p >> 8) & 0xFF, g, a),
                                        pb = towards(p & 0xFF, b, a);
                                *q = uint32_t((pr << 16) | (pg << 8) | pb);
                        }
                        return;
                }

                // A premultiplied colour (each channel already times its opacity @p a) over a pixel,
                // @p alpha of it.
                inline void blendPremultiplied(uint8_t *row, PixelFormat format, int32_t x, uint32_t r, uint32_t g,
                                               uint32_t b, uint32_t a, uint32_t alpha) {
                        if (alpha != 255) {
                                r = (r * alpha + 128) >> 8;
                                g = (g * alpha + 128) >> 8;
                                b = (b * alpha + 128) >> 8;
                                a = (a * alpha + 128) >> 8;
                        }
                        if (a == 0) return;
                        uint32_t keep = 255 - a;
                        if (format == PixelFormat::Rgb565) {
                                auto    *q = reinterpret_cast<uint16_t *>(row) + x;
                                uint32_t p = *q;
                                uint32_t pr = (p >> 11) & 0x1F, pg = (p >> 5) & 0x3F, pb = p & 0x1F;
                                pr = (pr << 3) | (pr >> 2);
                                pg = (pg << 2) | (pg >> 4);
                                pb = (pb << 3) | (pb >> 2);
                                pr = r + ((pr * keep + 128) >> 8);
                                pg = g + ((pg * keep + 128) >> 8);
                                pb = b + ((pb * keep + 128) >> 8);
                                pr = pr > 255 ? 255 : pr;
                                pg = pg > 255 ? 255 : pg;
                                pb = pb > 255 ? 255 : pb;
                                *q = uint16_t(((pr & 0xF8) << 8) | ((pg & 0xFC) << 3) | (pb >> 3));
                        } else {
                                auto    *q = reinterpret_cast<uint32_t *>(row) + x;
                                uint32_t p = *q;
                                uint32_t pr = r + ((((p >> 16) & 0xFF) * keep + 128) >> 8);
                                uint32_t pg = g + ((((p >> 8) & 0xFF) * keep + 128) >> 8);
                                uint32_t pb = b + (((p & 0xFF) * keep + 128) >> 8);
                                pr = pr > 255 ? 255 : pr;
                                pg = pg > 255 ? 255 : pg;
                                pb = pb > 255 ? 255 : pb;
                                *q = (pr << 16) | (pg << 8) | pb;
                        }
                        return;
                }

                // ---- Accumulating a row

                // Enough for a grid's row (28 lines across a region: 56 edges; at 32, every row of
                // the showcase's camera grid was walked a pixel at a time, 85% of its frame).
                constexpr uint32_t MaxSpans = 128;

                struct Span {
                                int32_t first, last;
                };

                int compareSpans(const void *a, const void *b) {
                        int32_t fa = static_cast<const Span *>(a)->first, fb = static_cast<const Span *>(b)->first;
                        return fa < fb ? -1 : fa > fb ? 1 : 0;
                }

                struct Row {
                                float  *cells;       ///< One a pixel from lo, and two past hi.
                                int32_t lo, hi;      ///< The pixels drawn: lo to hi (not included).
                                int32_t first, last; ///< The cells touched (first > last: none).
                                // The runs of cells the edges touched; between them the coverage
                                // doesn't change (filled as a run). Past MaxSpans: every cell visited.
                                Span     span[MaxSpans];
                                uint32_t spans;
                };

                // A piece of an edge inside the row (y within it), x already within lo...hi.
                void accumulate(Row &row, float xa, float xb, float d) {
                        float *a = row.cells;
                        float  x0 = xa < xb ? xa : xb, x1 = xa < xb ? xb : xa;
                        x0 -= float(row.lo);
                        x1 -= float(row.lo);
                        // Both are 0 or more here: floor and ceiling without libm's calls (no
                        // instructions for them on the board's core).
                        int32_t x0i = int32_t(x0);
                        float   x0floor = float(x0i);
                        int32_t x1i = int32_t(x1);
                        if (float(x1i) < x1) x1i++;
                        float x1ceil = float(x1i);
                        if (x0i < row.first) row.first = x0i;
                        if (x1i + 1 > row.last) row.last = x1i + 1;
                        if (row.spans < MaxSpans) row.span[row.spans] = Span{x0i, x1i > x0i + 1 ? x1i : x0i + 1};
                        row.spans++;
                        if (x1i <= x0i + 1) {
                                float xmf = 0.5f * (x0 + x1) - x0floor;
                                a[x0i] += d - d * xmf;
                                a[x0i + 1] += d * xmf;
                                return;
                        }
                        float s = 1.0f / (x1 - x0);
                        float x0f = x0 - x0floor;
                        float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
                        float x1f = x1 - x1ceil + 1;
                        float am = 0.5f * s * x1f * x1f;
                        a[x0i] += d * a0;
                        if (x1i == x0i + 2) {
                                a[x0i + 1] += d * (1 - a0 - am);
                        } else {
                                float a1 = s * (1.5f - x0f);
                                a[x0i + 1] += d * (a1 - a0);
                                for (int32_t xi = x0i + 2; xi < x1i - 1; xi++) a[xi] += d * s;
                                float a2 = a1 + float(x1i - x0i - 3) * s;
                                a[x1i - 1] += d * (1 - a2 - am);
                        }
                        a[x1i] += d * am;
                        return;
                }

                // The part of an edge from (xa, ya) to (xb, yb) (ya < yb, both within the row) with
                // direction @p dir: cut where it crosses lo and hi, the parts outside laid along them
                // (what is left of lo covers every pixel to its right; right of hi, none shown).
                void edgeInRow(Row &row, float xa, float ya, float xb, float yb, float dir) {
                        float lo = float(row.lo), hi = float(row.hi);
                        auto  part = [&](float x0, float y0, float x1, float y1) {
                                if (y1 <= y0) return;
                                x0 = x0 < lo ? lo : x0 > hi ? hi : x0;
                                x1 = x1 < lo ? lo : x1 > hi ? hi : x1;
                                accumulate(row, x0, x1, (y1 - y0) * dir);
                        };
                        // Where it crosses lo or hi, it is split there.
                        float cuts[2];
                        int   n = 0;
                        float edges[2] = {lo, hi};
                        for (float edge : edges) {
                                if ((xa < edge && xb > edge) || (xa > edge && xb < edge)) {
                                        cuts[n++] = ya + (edge - xa) * (yb - ya) / (xb - xa);
                                }
                        }
                        if (n == 2 && cuts[0] > cuts[1]) {
                                float t = cuts[0];
                                cuts[0] = cuts[1];
                                cuts[1] = t;
                        }
                        float px = xa, py = ya;
                        for (int i = 0; i < n; i++) {
                                float x = xa + (cuts[i] - ya) * (xb - xa) / (yb - ya);
                                part(px, py, x, cuts[i]);
                                px = x;
                                py = cuts[i];
                        }
                        part(px, py, xb, yb);
                        return;
                }

        } // namespace

        namespace {

                struct Edge {
                                float top, bottom; ///< Its y extent, top < bottom.
                                float x;           ///< Its x at top.
                                float dxdy;
                                float dir; ///< 1 down, -1 up.
                };

                int compareEdges(const void *a, const void *b) {
                        float ta = static_cast<const Edge *>(a)->top, tb = static_cast<const Edge *>(b)->top;
                        return ta < tb ? -1 : ta > tb ? 1 : 0;
                }

        } // namespace

        Error fillPath(Canvas &canvas, const Path &path, Color color, uint8_t alpha) {
                float x0, y0, x1, y1;
                if (alpha == 0 || !path.bounds(&x0, &y0, &x1, &y1)) return Error::Ok;
                const Rect &clip = canvas.clip();
                int32_t     top = int32_t(floorf(y0)), bottom = int32_t(ceilf(y1));
                int32_t     left = int32_t(floorf(x0)), right = int32_t(ceilf(x1)) + 1;
                top = top < clip.y ? clip.y : top;
                bottom = bottom > clip.y + clip.height ? clip.y + clip.height : bottom;
                left = left < clip.x ? clip.x : left;
                right = right > clip.x + clip.width ? clip.x + clip.width : right;
                if (top >= bottom || left >= right) return Error::Ok;
                // The edges, by their tops; a row's are those it crosses (kept as rows go down).
                uint32_t  count = path.pointCount();
                Edge     *edges = static_cast<Edge *>(malloc(count * sizeof(Edge)));
                uint32_t *active = static_cast<uint32_t *>(malloc(count * sizeof(uint32_t)));
                Row       row{static_cast<float *>(calloc(size_t(right - left + 3), sizeof(float))),
                        left,
                        right,
                        0x7FFFFFFF,
                        -1,
                              {},
                        0};
                if (edges == nullptr || active == nullptr || row.cells == nullptr) {
                        free(edges);
                        free(active);
                        free(row.cells);
                        return Error::NoMemory;
                }
                const Point    *pts = path.points();
                const uint32_t *starts = path.outlines();
                uint32_t        outlines = path.outlineCount(), n = 0;
                for (uint32_t o = 0; o < outlines; o++) {
                        uint32_t first = starts[o];
                        uint32_t end = o + 1 < outlines ? starts[o + 1] : count;
                        if (end - first < 2) continue;
                        for (uint32_t i = first; i < end; i++) {
                                Point a = pts[i], b = pts[i + 1 < end ? i + 1 : first];
                                if (a.y == b.y) continue;
                                float dir = 1;
                                if (a.y > b.y) {
                                        Point t = a;
                                        a = b;
                                        b = t;
                                        dir = -1;
                                }
                                if (b.y <= float(top) || a.y >= float(bottom)) continue;
                                edges[n++] = Edge{a.y, b.y, a.x, (b.x - a.x) / (b.y - a.y), dir};
                        }
                }
                qsort(edges, n, sizeof(Edge), compareEdges);
                uint32_t next = 0, actives = 0;
                for (int32_t y = top; y < bottom; y++) {
                        float ry0 = float(y), ry1 = float(y + 1);
                        while (next < n && edges[next].top < ry1) active[actives++] = next++;
                        uint32_t kept = 0;
                        for (uint32_t k = 0; k < actives; k++) {
                                const Edge &e = edges[active[k]];
                                if (e.bottom <= ry0) continue; // above this row: done with
                                active[kept++] = active[k];
                                float ya = e.top > ry0 ? e.top : ry0, yb = e.bottom < ry1 ? e.bottom : ry1;
                                if (yb <= ya) continue;
                                edgeInRow(row, e.x + (ya - e.top) * e.dxdy, ya, e.x + (yb - e.top) * e.dxdy, yb, e.dir);
                        }
                        actives = kept;
                        if (row.first > row.last) continue;
                        // The cells the edges touched, each with its own coverage; between them the
                        // coverage holds, so a run is filled at once (or passed over: none). Covered
                        // wholly and opaque, a pixel is written, not read and mixed.
                        uint8_t *line = canvas.pixels() + size_t(y) * canvas.stride();
                        int32_t  cells = right - left;
                        float    sum = 0;
                        auto     pixel = [&](int32_t c, float v) {
                                if (v < 0.002f) return;
                                uint32_t a = v >= 1.0f ? alpha : uint32_t(v * float(alpha) + 0.5f);
                                blendPixel(line, canvas.format(), left + c, color, a);
                        };
                        auto run = [&](int32_t from, int32_t to) {
                                // Cells from...to (not included), all at the coverage the sum has.
                                if (to > cells) to = cells;
                                if (from >= to) return;
                                float v = sum < 0 ? -sum : sum;
                                if (v < 0.002f) return;
                                uint32_t a = v >= 1.0f ? alpha : uint32_t(v * float(alpha) + 0.5f);
                                if (a >= 255) {
                                        canvas.fill(Rect{left + from, y, to - from, 1}, color);
                                } else if (canvas.format() == PixelFormat::Rgb565) {
                                        auto    *q = reinterpret_cast<uint16_t *>(line) + left;
                                        uint32_t over = spread565(canvas.pixel(color)), a32 = (a + 4) >> 3;
                                        for (int32_t c = from; c < to; c++) q[c] = mix565(q[c], over, a32);
                                } else {
                                        canvas.blend(Rect{left + from, y, to - from, 1}, color, uint8_t(a));
                                }
                        };
                        if (row.spans > MaxSpans) {
                                for (int32_t c = row.first; c < row.last && c < cells; c++) {
                                        sum += row.cells[c];
                                        pixel(c, sum < 0 ? -sum : sum);
                                }
                        } else {
                                // By where they start (a few: insertion; many, in any order, as a
                                // grid's lines from its middle out: qsort), then each run and gap.
                                bool sorted = true;
                                for (uint32_t i = 1; i < row.spans && sorted; i++)
                                        sorted = row.span[i - 1].first <= row.span[i].first;
                                if (sorted) {
                                        // As they came (a row's edges left to right).
                                } else if (row.spans > 16) {
                                        qsort(row.span, row.spans, sizeof(Span), compareSpans);
                                } else {
                                        for (uint32_t i = 1; i < row.spans; i++) {
                                                Span     sp = row.span[i];
                                                uint32_t j = i;
                                                while (j > 0 && row.span[j - 1].first > sp.first) {
                                                        row.span[j] = row.span[j - 1];
                                                        j--;
                                                }
                                                row.span[j] = sp;
                                        }
                                }
                                int32_t at = row.first;
                                for (uint32_t i = 0; i < row.spans; i++) {
                                        int32_t f = row.span[i].first, l = row.span[i].last;
                                        if (l < at) continue; // inside one before
                                        if (f > at) {
                                                run(at, f);
                                                at = f;
                                        }
                                        for (int32_t c = at; c <= l && c < cells; c++) {
                                                sum += row.cells[c];
                                                pixel(c, sum < 0 ? -sum : sum);
                                        }
                                        at = l + 1;
                                }
                        }
                        memset(row.cells + row.first, 0, size_t(row.last - row.first + 1) * sizeof(float));
                        row.first = 0x7FFFFFFF;
                        row.last = -1;
                        row.spans = 0;
                }
                free(edges);
                free(active);
                free(row.cells);
                return Error::Ok;
        }

        Rect transformedBounds(const Affine &t, float x, float y, float width, float height) {
                Point c[4] = {t.apply(Point{x, y}), t.apply(Point{x + width, y}), t.apply(Point{x, y + height}),
                              t.apply(Point{x + width, y + height})};
                float l = c[0].x, r = l, tp = c[0].y, b = tp;
                for (const Point &p : c) {
                        l = p.x < l ? p.x : l;
                        r = p.x > r ? p.x : r;
                        tp = p.y < tp ? p.y : tp;
                        b = p.y > b ? p.y : b;
                }
                int32_t x0 = int32_t(floorf(l)) - 1, y0 = int32_t(floorf(tp)) - 1;
                return Rect{x0, y0, int32_t(ceilf(r)) + 1 - x0, int32_t(ceilf(b)) + 1 - y0};
        }

        namespace {

                // True if @p t only moves (unscaled, unturned): then pixels are copied, not sampled,
                // to the nearest whole pixel (sampled between pixels, text goes soft).
                bool wholeMove(const Affine &t, int32_t *dx, int32_t *dy) {
                        // Near enough (a scene shrunk to fit draws its text scaled by fit, then back by
                        // 1 / fit: a float's rounding from 1): under a hundredth of a pixel across 4096.
                        constexpr float Near = 2e-6f;
                        if (fabsf(t.a - 1) > Near || fabsf(t.b) > Near || fabsf(t.c) > Near || fabsf(t.d - 1) > Near)
                                return false;
                        *dx = int32_t(floorf(t.e + 0.5f));
                        *dy = int32_t(floorf(t.f + 0.5f));
                        return true;
                }

        } // namespace

        namespace {

                // Where each pixel of a row samples a picture through @p inverse: 16.16 fixed point,
                // the row's first worked out, then a step a pixel (no transform, no floor a pixel).
                // Pictures are under 32768 pixels a side: the fixed point holds them.
                class Sampler {
                        public:
                                explicit Sampler(const Affine &inverse)
                                    : _inverse(inverse), _dx(int32_t(lroundf(inverse.a * 65536))),
                                      _dy(int32_t(lroundf(inverse.b * 65536))) {}
                                void row(int32_t x, int32_t y) {
                                        Point s = _inverse.apply(Point{float(x) + 0.5f, float(y) + 0.5f});
                                        _x = int32_t(lroundf((s.x - 0.5f) * 65536));
                                        _y = int32_t(lroundf((s.y - 0.5f) * 65536));
                                        return;
                                }
                                void next() {
                                        _x += _dx;
                                        _y += _dy;
                                        return;
                                }
                                int32_t  ix() const { return _x >> 16; } // C++20: a floor
                                int32_t  iy() const { return _y >> 16; }
                                uint32_t fx() const { return uint32_t(_x >> 8) & 0xFF; }
                                uint32_t fy() const { return uint32_t(_y >> 8) & 0xFF; }

                        private:
                                Affine  _inverse;
                                int32_t _dx, _dy;
                                int32_t _x = 0, _y = 0;
                };

        } // namespace

        void drawMask(Canvas &canvas, const uint8_t *mask, int32_t width, int32_t height, uint32_t stride,
                      const Affine &transform, Color color, uint8_t alpha, const Rect *source) {
                if (alpha == 0 || width <= 0 || height <= 0) return;
                Rect part =
                        source != nullptr ? source->intersected(Rect{0, 0, width, height}) : Rect{0, 0, width, height};
                if (part.isEmpty()) return;
                int32_t dx = 0, dy = 0;
                if (wholeMove(transform, &dx, &dy)) {
                        Rect r = Rect{dx + part.x, dy + part.y, part.width, part.height}.intersected(canvas.clip());
                        for (int32_t y = r.y; y < r.y + r.height; y++) {
                                uint8_t       *line = canvas.pixels() + size_t(y) * canvas.stride();
                                const uint8_t *m = mask + size_t(y - dy) * stride;
                                for (int32_t x = r.x; x < r.x + r.width; x++) {
                                        uint32_t c = m[x - dx];
                                        if (c != 0)
                                                blendPixel(line, canvas.format(), x, color, (c * alpha + 127) / 255);
                                }
                        }
                        return;
                }
                Affine inverse;
                if (!transform.inverted(&inverse)) return;
                Rect r = transformedBounds(transform, float(part.x), float(part.y), float(part.width),
                                           float(part.height))
                                 .intersected(canvas.clip());
                int32_t x0 = part.x, y0 = part.y, x1 = part.x + part.width, y1 = part.y + part.height;
                auto    at = [&](int32_t x, int32_t y) -> uint32_t {
                        return x < x0 || y < y0 || x >= x1 || y >= y1 ? 0 : mask[size_t(y) * stride + uint32_t(x)];
                };
                Sampler sampler(inverse);
                for (int32_t y = r.y; y < r.y + r.height; y++) {
                        uint8_t *line = canvas.pixels() + size_t(y) * canvas.stride();
                        sampler.row(r.x, y);
                        for (int32_t x = r.x; x < r.x + r.width; x++, sampler.next()) {
                                int32_t ix = sampler.ix(), iy = sampler.iy();
                                if (ix < x0 - 1 || iy < y0 - 1 || ix >= x1 || iy >= y1) continue;
                                uint32_t fx = sampler.fx(), fy = sampler.fy(), p00, p10, p01, p11;
                                if (ix >= x0 && iy >= y0 && ix + 1 < x1 && iy + 1 < y1) {
                                        const uint8_t *m = mask + size_t(iy) * stride + uint32_t(ix);
                                        p00 = m[0];
                                        p10 = m[1];
                                        p01 = m[stride];
                                        p11 = m[stride + 1];
                                } else {
                                        p00 = at(ix, iy);
                                        p10 = at(ix + 1, iy);
                                        p01 = at(ix, iy + 1);
                                        p11 = at(ix + 1, iy + 1);
                                }
                                if ((p00 | p10 | p01 | p11) == 0) continue;
                                uint32_t top = p00 * (256 - fx) + p10 * fx;
                                uint32_t bottom = p01 * (256 - fx) + p11 * fx;
                                uint32_t c = (top * (256 - fy) + bottom * fy) >> 16;
                                if (c != 0) blendPixel(line, canvas.format(), x, color, (c * alpha + 127) / 255);
                        }
                }
                return;
        }

        void drawPicture(Canvas &canvas, const ScaledImage &image, const Affine &transform, uint8_t alpha,
                         const Rect *source) {
                if (alpha == 0 || image.pixels == nullptr) return;
                int32_t w = image.width, h = image.height;
                Rect    part = source != nullptr ? source->intersected(Rect{0, 0, w, h}) : Rect{0, 0, w, h};
                if (part.isEmpty()) return;
                int32_t dx = 0, dy = 0;
                bool    whole = wholeMove(transform, &dx, &dy);
                if (whole && alpha == 255) {
                        // A part: the canvas clipped to where it goes.
                        Rect clip = canvas.clip();
                        canvas.setClip(clip.intersected(Rect{dx + part.x, dy + part.y, part.width, part.height}));
                        blitImage(canvas, image, dx, dy);
                        canvas.setClip(clip);
                        return;
                }
                bool rgb565 = image.format == PixelFormat::Rgb565;
                // A pixel of the picture (its part) as premultiplied channels and its opacity (all 0
                // outside).
                auto at = [&](int32_t x, int32_t y, uint32_t *c) {
                        if (x < part.x || y < part.y || x >= part.x + part.width || y >= part.y + part.height) {
                                c[0] = c[1] = c[2] = c[3] = 0;
                                return;
                        }
                        const uint8_t *row = image.pixels + size_t(y) * image.stride;
                        if (rgb565) {
                                uint32_t p = reinterpret_cast<const uint16_t *>(row)[x];
                                uint32_t pr = (p >> 11) & 0x1F, pg = (p >> 5) & 0x3F, pb = p & 0x1F;
                                c[0] = (pr << 3) | (pr >> 2);
                                c[1] = (pg << 2) | (pg >> 4);
                                c[2] = (pb << 3) | (pb >> 2);
                        } else {
                                uint32_t p = reinterpret_cast<const uint32_t *>(row)[x];
                                c[0] = (p >> 16) & 0xFF;
                                c[1] = (p >> 8) & 0xFF;
                                c[2] = p & 0xFF;
                        }
                        c[3] = image.alpha != nullptr ? image.alpha[size_t(y) * uint32_t(w) + uint32_t(x)] : 255;
                };
                if (whole) {
                        // Moved by whole pixels, faded: each pixel as it is (a fading line of code).
                        Rect r = Rect{dx + part.x, dy + part.y, part.width, part.height}.intersected(canvas.clip());
                        for (int32_t y = r.y; y < r.y + r.height; y++) {
                                uint8_t *line = canvas.pixels() + size_t(y) * canvas.stride();
                                for (int32_t x = r.x; x < r.x + r.width; x++) {
                                        uint32_t c[4];
                                        at(x - dx, y - dy, c);
                                        if (c[3] != 0)
                                                blendPremultiplied(line, canvas.format(), x, c[0], c[1], c[2], c[3],
                                                                   alpha);
                                }
                        }
                        return;
                }
                Affine inverse;
                if (!transform.inverted(&inverse)) return;
                Rect r = transformedBounds(transform, float(part.x), float(part.y), float(part.width),
                                           float(part.height))
                                 .intersected(canvas.clip());
                Sampler sampler(inverse);
                for (int32_t y = r.y; y < r.y + r.height; y++) {
                        uint8_t *line = canvas.pixels() + size_t(y) * canvas.stride();
                        sampler.row(r.x, y);
                        for (int32_t x = r.x; x < r.x + r.width; x++, sampler.next()) {
                                int32_t ix = sampler.ix(), iy = sampler.iy();
                                if (ix < part.x - 1 || iy < part.y - 1 || ix >= part.x + part.width ||
                                    iy >= part.y + part.height)
                                        continue;
                                uint32_t fx = sampler.fx(), fy = sampler.fy();
                                uint32_t p00[4], p10[4], p01[4], p11[4], out[4];
                                at(ix, iy, p00);
                                at(ix + 1, iy, p10);
                                at(ix, iy + 1, p01);
                                at(ix + 1, iy + 1, p11);
                                for (int i = 0; i < 4; i++) {
                                        uint32_t top = p00[i] * (256 - fx) + p10[i] * fx;
                                        uint32_t bottom = p01[i] * (256 - fx) + p11[i] * fx;
                                        out[i] = (top * (256 - fy) + bottom * fy) >> 16;
                                }
                                blendPremultiplied(line, canvas.format(), x, out[0], out[1], out[2], out[3], alpha);
                        }
                }
                return;
        }

} // namespace preso
