/**
 * @file      raster.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's triangles (devplan/3d.md §12a). Vertices in 28.4 fixed point; a row is drawn where
 * its pixel centres' y is in [top, bottom), a pixel where its centre's x is in [left, right). Each
 * edge's first column per row is ceil((x - 8) / 16) of the edge's x at the row's centre, kept
 * exact by an integer DDA (a quotient and a remainder stepped per row), so two triangles sharing an
 * edge step it identically and meet without a gap or an overlap. Shades: flat, or the shade's
 * plane evaluated at each span's ends (clamped) and stepped in 16.16 between them.
 */

#include <stdint.h>
#include <string.h>
#include "3d_internal.h"

/* An edge's first pixel column on successive rows: q, with r the remainder in [0, d). */
typedef struct {
                int32_t q, r, d, step_q, step_r;
} edge_t;

/* Floor division by @p b > 0 (no separate modulo: one library call at most). */
static int64_t floor_div(int64_t a, int64_t b) {
        const int64_t q = a / b;
        return q * b > a ? q - 1 : q;
}

static int32_t floor_div32(int32_t a, int32_t b) {
        const int32_t q = a / b;
        return q * b > a ? q - 1 : q;
}

/* The edge (x0, y0)-(x1, y1), y1 > y0, at row @p row (its centre at row × 16 + 8). Coordinates
   inside the guard band keep d, 16 × dx and most numerators in 32 bits: the hardware divides. */
static void edge_init(edge_t *e, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t row) {
        const int64_t dy = (int64_t)y1 - y0, dx = (int64_t)x1 - x0, d = 16 * dy;
        const int64_t yc = (int64_t)row * 16 + 8;
        const int64_t m = ((int64_t)x0 - 8) * dy + (yc - y0) * dx + d - 1; /* ceil as floor */
        const int32_t d32 = (int32_t)d, step = (int32_t)(16 * dx);
        int64_t       q;
        if (m >= INT32_MIN && m <= INT32_MAX)
                q = floor_div32((int32_t)m, d32);
        else
                q = floor_div(m, d);
        const int32_t sq = floor_div32(step, d32);
        e->q = (int32_t)q;
        e->r = (int32_t)(m - q * d);
        e->d = d32;
        e->step_q = sq;
        e->step_r = step - sq * d32;
        return;
}

static inline void edge_step(edge_t *e) {
        e->q += e->step_q;
        e->r += e->step_r;
        if (e->r >= e->d) {
                ++e->q;
                e->r -= e->d;
        }
        return;
}

/* The first row whose centre is at or below @p y (28.4). */
static int32_t first_row(int32_t y) {
        return (y - 8 + 15) >> 4; /* an arithmetic shift: floor */
}

/* One span: columns [x0, x1) of a row, shade from @p s (16.16) by @p ds a pixel. */
static void span(bmds_canvas_t *c, int32_t row, int32_t x0, int32_t x1, const bmds_3d_record_t *rec, int32_t s,
                 int32_t ds, uint32_t flat) {
        uint8_t      *p = c->pixels + (size_t)row * c->stride;
        const int32_t n = x1 - x0;
        if (c->format == BMDS_PIXEL_L8) {
                uint8_t *q = p + x0;
                if (!rec->smooth) {
                        memset(q, (int)flat, (size_t)n);
                        return;
                }
                const uint32_t first = rec->first;
                for (int32_t i = 0; i < n; ++i, s += ds) q[i] = (uint8_t)(first + (uint32_t)(s >> 16));
        } else if (c->format == BMDS_PIXEL_RGB565) {
                uint16_t *q = (uint16_t *)(void *)p + x0;
                if (!rec->smooth) {
                        for (int32_t i = 0; i < n; ++i) q[i] = (uint16_t)flat;
                        return;
                }
                for (int32_t i = 0; i < n; ++i, s += ds) q[i] = (uint16_t)rec->shades[s >> 16];
        } else {
                uint8_t *q = p + 3 * (size_t)x0;
                for (int32_t i = 0; i < n; ++i, q += 3) {
                        const uint32_t v = rec->smooth ? rec->shades[s >> 16] : flat;
                        q[0] = (uint8_t)v;
                        q[1] = (uint8_t)(v >> 8);
                        q[2] = (uint8_t)(v >> 16);
                        s += ds;
                }
        }
        return;
}

static uint32_t pixel_of(const bmds_canvas_t *c, const bmds_3d_record_t *rec, uint32_t shade) {
        if (shade > rec->top) shade = rec->top;
        return c->format == BMDS_PIXEL_L8 ? rec->first + shade : rec->shades[shade];
}

uint32_t bmds_3d_triangle(bmds_canvas_t *c, const bmds_3d_area_t *area, const bmds_3d_record_t *rec,
                          const bmds_3d_tri_t *t, bmds_3d_bounds_t *bounds) {
        /* Top to bottom. */
        int a = 0, b = 1, k = 2, tmp;
        if (t->y[a] > t->y[b]) tmp = a, a = b, b = tmp;
        if (t->y[b] > t->y[k]) tmp = b, b = k, k = tmp;
        if (t->y[a] > t->y[b]) tmp = a, a = b, b = tmp;
        const int32_t x0 = t->x[a], y0 = t->y[a], x1 = t->x[b], y1 = t->y[b], x2 = t->x[k], y2 = t->y[k];
        const int64_t cross = ((int64_t)x1 - x0) * ((int64_t)y2 - y0) - ((int64_t)y1 - y0) * ((int64_t)x2 - x0);
        if (cross == 0 || y2 == y0) return 0;
        const int long_left = cross > 0; /* the middle vertex right of the long edge */

        int32_t row = first_row(y0), mid = first_row(y1), end = first_row(y2);
        if (row < area->iy0) row = area->iy0;
        if (end > area->iy1) end = area->iy1;
        if (row >= end) return 0;

        /* The shade: flat, or its plane over the screen (pixels, shade units). */
        uint32_t    flat = 0;
        float       ga = 0.0f, gb = 0.0f, gc = 0.0f; /* s = ga·x + gb·y + gc */
        if (!rec->smooth) {
                flat = pixel_of(c, rec, (uint32_t)((t->s[0] + 128) >> 8));
        } else {
                const float fx0 = (float)x0 / 16.0f, fy0 = (float)y0 / 16.0f;
                const float dx1 = (float)(x1 - x0) / 16.0f, dy1 = (float)(y1 - y0) / 16.0f;
                const float dx2 = (float)(x2 - x0) / 16.0f, dy2 = (float)(y2 - y0) / 16.0f;
                const float s0 = (float)t->s[a] / 256.0f, ds1 = (float)t->s[b] / 256.0f - s0;
                const float ds2 = (float)t->s[k] / 256.0f - s0;
                const float inv = 1.0f / (dx1 * dy2 - dx2 * dy1);
                ga = (ds1 * dy2 - ds2 * dy1) * inv;
                gb = (ds2 * dx1 - ds1 * dx2) * inv;
                gc = s0 - ga * fx0 - gb * fy0;
        }
        /* The plane in 16.16 at pixel centres, rounded by half, from the top vertex's pixel (xa, ya):
           s(x, y) = s0 + ia·(x − xa) + ib·(y − ya). Every value fits 32 bits (float to int64 is a
           library call through doubles here); a sliver's huge gradient is held in range, and its
           spans re-stepped below. */
        const float   gs = ga * 65536.0f, gt = gb * 65536.0f;
        const int32_t step = gs > 2.0e9f ? 2000000000 : gs < -2.0e9f ? -2000000000 : (int32_t)gs;
        const int64_t ia = step;
        const int64_t ib = gt > 2.0e9f ? 2000000000 : gt < -2.0e9f ? -2000000000 : (int32_t)gt;
        const int32_t xa = x0 >> 4, ya = y0 >> 4;
        const float   s0f = (ga * ((float)xa + 0.5f) + gb * ((float)ya + 0.5f) + gc) * 65536.0f + 32768.0f;
        const int64_t ic = s0f > 2.0e9f ? 2000000000 : s0f < -2.0e9f ? -2000000000 : (int32_t)s0f;
        const int64_t smax = ((int64_t)rec->top << 16) + 32768;

        edge_t longe, shorte;
        edge_init(&longe, x0, y0, x2, y2, row);
        const int on_upper = row < mid;
        if (on_upper)
                edge_init(&shorte, x0, y0, x1, y1, row);
        else
                edge_init(&shorte, x1, y1, x2, y2, row);
        uint32_t pixels = 0;
        int32_t  minx = area->ix1, maxx = area->ix0;
        for (int32_t y = row; y < end; ++y) {
                if (y == mid && y != row) edge_init(&shorte, x1, y1, x2, y2, y);
                int32_t xl = long_left ? longe.q : shorte.q, xr = long_left ? shorte.q : longe.q;
                if (xl < area->ix0) xl = area->ix0;
                if (xr > area->ix1) xr = area->ix1;
                if (xl < xr) {
                        int32_t s = 0, ds = 0;
                        if (rec->smooth) {
                                /* The plane at the first pixel's centre, stepped by its x gradient; the
                                   last pixel checked against the ramp (rounding at a sliver's edge). */
                                const int32_t n = xr - xl;
                                int64_t       s64 = ia * (xl - xa) + ib * (y - ya) + ic;
                                s = (int32_t)(s64 < 32768 ? 32768 : s64 > smax ? smax : s64);
                                ds = step;
                                const int64_t last = (int64_t)s + (int64_t)ds * (n - 1);
                                if (last < 0 || last >= ((int64_t)rec->top + 1) << 16) {
                                        const int32_t want = last < 0 ? 32768 : ((int32_t)rec->top << 16) + 32768;
                                        ds = n > 1 ? (want - s) / (n - 1) : 0;
                                }
                        }
                        span(c, y, xl, xr, rec, s, ds, flat);
                        pixels += (uint32_t)(xr - xl);
                        if (xl < minx) minx = xl;
                        if (xr > maxx) maxx = xr;
                }
                edge_step(&longe);
                edge_step(&shorte);
        }
        if (pixels != 0) bmds_3d_include(bounds, minx, row, maxx, end);
        return pixels;
}
