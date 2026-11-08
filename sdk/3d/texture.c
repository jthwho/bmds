/**
 * @file      texture.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's textured triangles (devplan/3d.md §4f step 7, §13 step 4), L8: the rows and columns
 * exactly as bmds_3d_triangle()'s (the same edges, so the fill rule holds between textured and
 * untextured triangles), texture coordinates (and shades) as planes over the screen set up once a
 * triangle in floats, then stepped in 16.16 along each span. Affine by default; with
 * `BMDS_3D_PERSPECTIVE`, u / w, v / w and 1 / w are the planes and the span is exact every 16
 * pixels, affine between (one division a run). Texels fetched by mask (power-of-two textures
 * wrap), four pixels gathered into a word a store.
 */

#include <math.h>
#include <stdint.h>
#include "3d_internal.h"

/* How a span's texels are fetched. */
typedef struct {
                const uint8_t *texels;
                const uint8_t *light; /* the light table's row (flat), or the whole table (shaded) */
                uint32_t       umask, vmask, vshift;
} fetch_t;

/* The texel at (u, v), 16.16 texels: v's row offset (stride a power of two) or'd with u's column.
   On the run functions' locals (copied from the fetch_t: a byte store may alias anything, so
   fields would be loaded again after each). */
#define TEXEL() tex[((v >> vs) & vm) | ((u >> 16) & um)]

/* The next pixel of each lighting, its interpolants stepped. */
#define STRAIGHT() (tx = TEXEL(), u += du, v += dv, tx)
#define ROW() (tx = light[TEXEL()], u += du, v += dv, tx)
#define SHADED() (tx = light[((s >> 8) & 0x1f00u) | TEXEL()], u += du, v += dv, s += ds, tx)

/* Writes @p n pixels from @p q: the first ones alone up to a word's boundary, then four a store,
   then the rest. */
#define RUN(PIXEL)                                                                                                     \
        do {                                                                                                           \
                uint32_t tx;                                                                                           \
                while (n > 0 && ((uintptr_t)q & 3u) != 0) {                                                            \
                        *q++ = (uint8_t)PIXEL();                                                                       \
                        --n;                                                                                           \
                }                                                                                                      \
                for (; n >= 4; n -= 4, q += 4) {                                                                       \
                        const uint32_t p0 = PIXEL();                                                                   \
                        const uint32_t p1 = PIXEL();                                                                   \
                        const uint32_t p2 = PIXEL();                                                                   \
                        const uint32_t p3 = PIXEL();                                                                   \
                        *(uint32_t *)(void *)q = p0 | p1 << 8 | p2 << 16 | p3 << 24;                                   \
                }                                                                                                      \
                while (n-- > 0) *q++ = (uint8_t)PIXEL();                                                               \
        } while (0)

#define LOCALS()                                                                                                       \
        const uint8_t *const tex = f->texels;                                                                          \
        const uint8_t *const light = f->light;                                                                         \
        const uint32_t       um = f->umask, vm = f->vmask, vs = f->vshift;                                             \
        (void)light

/* The span's runs, one a lighting. Unsigned arithmetic: the coordinates wrap by mask, never overflow. */

static void run_straight(uint8_t *q, int32_t n, uint32_t u, uint32_t du, uint32_t v, uint32_t dv, const fetch_t *f) {
        LOCALS();
        RUN(STRAIGHT);
        return;
}

static void run_row(uint8_t *q, int32_t n, uint32_t u, uint32_t du, uint32_t v, uint32_t dv, const fetch_t *f) {
        LOCALS();
        RUN(ROW);
        return;
}

/* The shade in 16.16 (0..31.99): its row is (s >> 16) << 8, or'd with the texel. */
static void run_shaded(uint8_t *q, int32_t n, uint32_t u, uint32_t du, uint32_t v, uint32_t dv, uint32_t s, uint32_t ds,
                       const fetch_t *f) {
        LOCALS();
        RUN(SHADED);
        return;
}

/* @p x in 16.16, held in int32's range (a sliver's gradients, a texel far off: wrapped, not undefined). */
static inline uint32_t fixed16(float x) {
        const float v = x * 65536.0f;
        return (uint32_t)(int32_t)(v < 2.0e9f ? (v > -2.0e9f ? v : -2.0e9f) : 2.0e9f); /* NaN: the top */
}

/* One run of @p n pixels at @p q with the record's lighting. */
static inline void run(uint8_t *q, int32_t n, uint32_t u, uint32_t du, uint32_t v, uint32_t dv, uint32_t s, uint32_t ds,
                       int kind, const fetch_t *f) {
        if (kind == 0)
                run_straight(q, n, u, du, v, dv, f);
        else if (kind == 1)
                run_row(q, n, u, du, v, dv, f);
        else
                run_shaded(q, n, u, du, v, dv, s, ds, f);
        return;
}

/* 1 / n for a perspective run's length. */
static const float reciprocal[17] = {0.0f,         1.0f,         1.0f / 2.0f,  1.0f / 3.0f,  1.0f / 4.0f,  1.0f / 5.0f,
                                     1.0f / 6.0f,  1.0f / 7.0f,  1.0f / 8.0f,  1.0f / 9.0f,  1.0f / 10.0f, 1.0f / 11.0f,
                                     1.0f / 12.0f, 1.0f / 13.0f, 1.0f / 14.0f, 1.0f / 15.0f, 1.0f / 16.0f};

#define PERSPECTIVE_RUN 16

uint32_t bmds_3d_triangle_textured(bmds_canvas_t *c, const bmds_3d_area_t *area, const bmds_3d_record_t *rec,
                                   const bmds_3d_tri_t *t, const bmds_3d_tuv_t *uv, bmds_3d_bounds_t *bounds) {
        if (c->format != BMDS_PIXEL_L8) return 0;
        /* Top to bottom; the rows and edges exactly as bmds_3d_triangle()'s. */
        int i0 = 0, i1 = 1, i2 = 2, tmp;
        if (t->y[i0] > t->y[i1]) tmp = i0, i0 = i1, i1 = tmp;
        if (t->y[i1] > t->y[i2]) tmp = i1, i1 = i2, i2 = tmp;
        if (t->y[i0] > t->y[i1]) tmp = i0, i0 = i1, i1 = tmp;
        const int32_t x0 = t->x[i0], y0 = t->y[i0], x1 = t->x[i1], y1 = t->y[i1], x2 = t->x[i2], y2 = t->y[i2];
        const int64_t cross = (int64_t)(x1 - x0) * (y2 - y0) - (int64_t)(y1 - y0) * (x2 - x0);
        if (cross == 0 || y2 == y0) return 0;
        const int long_left = cross > 0;

        int32_t row = bmds_3d_first_row(y0), mid = bmds_3d_first_row(y1), end = bmds_3d_first_row(y2);
        if (row < area->iy0) row = area->iy0;
        if (end > area->iy1) end = area->iy1;
        if (row >= end) return 0;

        /* The planes, from the top vertex (pixels): u, v (u / w, v / w with perspective), 1 / w, the
           shade. u and v moved by whole turns of the texture to near 0 (the same texels), so 16.16
           holds them. */
        const int   persp = rec->perspective;
        const int   kind = rec->table == NULL ? 0 : rec->smooth ? 2 : 1;
        const int   order[3] = {i0, i1, i2};
        float       a[4][3]; /* u, v, q, s at each vertex, top first */
        const float umin = fminf(uv->u[0], fminf(uv->u[1], uv->u[2]));
        const float vmin = fminf(uv->v[0], fminf(uv->v[1], uv->v[2]));
        const float uoff = floorf(umin / rec->tw) * rec->tw, voff = floorf(vmin / rec->th) * rec->th;
        float       qmax = 1.0f;
        if (persp) {
                qmax = fmaxf(uv->q[0], fmaxf(uv->q[1], uv->q[2]));
                if (!(qmax > 0.0f)) return 0;
        }
        for (int k = 0; k < 3; ++k) {
                const int   j = order[k];
                const float q = persp ? uv->q[j] / qmax : 1.0f; /* 0..1: the nearest vertex's 1 */
                a[0][k] = (uv->u[j] - uoff) * q;
                a[1][k] = (uv->v[j] - voff) * q;
                a[2][k] = q;
                a[3][k] = (float)t->s[j] * (1.0f / 256.0f);
        }
        const float fx0 = (float)x0 * (1.0f / 16.0f), fy0 = (float)y0 * (1.0f / 16.0f);
        const float dx1 = (float)(x1 - x0) * (1.0f / 16.0f), dy1 = (float)(y1 - y0) * (1.0f / 16.0f);
        const float dx2 = (float)(x2 - x0) * (1.0f / 16.0f), dy2 = (float)(y2 - y0) * (1.0f / 16.0f);
        const float inv = 1.0f / (dx1 * dy2 - dx2 * dy1);
        float       gx[4], gy[4];
        const int   planes = persp ? 3 : 2;
        for (int p = 0; p < 4; ++p) {
                if (p >= planes && !(p == 3 && kind == 2)) {
                        gx[p] = gy[p] = 0.0f;
                        continue;
                }
                const float d1 = a[p][1] - a[p][0], d2 = a[p][2] - a[p][0];
                gx[p] = (d1 * dy2 - d2 * dy1) * inv;
                gy[p] = (d2 * dx1 - d1 * dx2) * inv;
        }

        fetch_t f;
        f.texels = rec->texels;
        f.umask = rec->umask;
        f.vmask = rec->vmask;
        f.vshift = rec->vshift;
        f.light = rec->table;
        if (kind == 1) {
                uint32_t shade = (uint32_t)((t->s[0] + 128) >> 8);
                if (shade > rec->top) shade = rec->top;
                f.light = rec->table + (shade << 8);
        }
        const uint32_t du = fixed16(gx[0]), dv = fixed16(gx[1]);
        const float    smax = (float)rec->top + 0.5f;
        const int32_t  ds_step = (int32_t)fixed16(gx[3]);

        bmds_3d_edge_t longe, shorte;
        bmds_3d_edge_init(&longe, x0, y0, x2, y2, row);
        if (row < mid)
                bmds_3d_edge_init(&shorte, x0, y0, x1, y1, row);
        else
                bmds_3d_edge_init(&shorte, x1, y1, x2, y2, row);
        uint32_t pixels = 0;
        int32_t  minx = area->ix1, maxx = area->ix0;
        uint8_t *line = c->pixels + (size_t)row * c->stride;
        for (int32_t y = row; y < end; ++y, line += c->stride) {
                if (y == mid && y != row) bmds_3d_edge_init(&shorte, x1, y1, x2, y2, y);
                const int32_t cl = bmds_3d_edge_column(&longe), cs = bmds_3d_edge_column(&shorte);
                int32_t       xl = long_left ? cl : cs, xr = long_left ? cs : cl;
                if (xl < area->ix0) xl = area->ix0;
                if (xr > area->ix1) xr = area->ix1;
                bmds_3d_edge_step(&longe);
                bmds_3d_edge_step(&shorte);
                if (xl >= xr) continue;
                const int32_t n = xr - xl;
                /* The first pixel's centre, from the top vertex. */
                const float px = (float)xl + 0.5f - fx0, py = (float)y + 0.5f - fy0;

                /* The shade: its plane at the first pixel, held in the table, stepped by its gradient;
                   re-stepped (one division) only when rounding would leave the table by the last. */
                uint32_t s = 0, ds = 0;
                if (kind == 2) {
                        float sf = a[3][0] + gx[3] * px + gy[3] * py;
                        sf = sf > 0.5f ? (sf < smax ? sf : smax) : 0.5f; /* NaN: the bottom */
                        const int32_t s0 = (int32_t)(sf * 65536.0f);
                        int32_t       d = ds_step;
                        const int64_t last = (int64_t)s0 + (int64_t)d * (n - 1);
                        if (last < 0 || last >= ((int64_t)rec->top + 1) << 16) {
                                const int32_t want = last < 0 ? 32768 : ((int32_t)rec->top << 16) + 32768;
                                d = n > 1 ? (want - s0) / (n - 1) : 0;
                        }
                        s = (uint32_t)s0;
                        ds = (uint32_t)d;
                }
                uint8_t *q = line + xl;
                if (!persp) {
                        const uint32_t u = fixed16(a[0][0] + gx[0] * px + gy[0] * py);
                        const uint32_t v = fixed16(a[1][0] + gx[1] * px + gy[1] * py);
                        run(q, n, u, du, v, dv, s, ds, kind, &f);
                } else {
                        /* Exact at each run's ends, PERSPECTIVE_RUN pixels apart; affine between. */
                        float U = a[0][0] + gx[0] * px + gy[0] * py, V = a[1][0] + gx[1] * px + gy[1] * py;
                        float Q = a[2][0] + gx[2] * px + gy[2] * py;
                        float iq = 1.0f / (Q > 1e-6f ? Q : 1e-6f);
                        float u0 = U * iq, v0 = V * iq;
                        for (int32_t left = n; left > 0;) {
                                const int32_t len = left < PERSPECTIVE_RUN ? left : PERSPECTIVE_RUN;
                                const float   fl = (float)len;
                                U += gx[0] * fl, V += gx[1] * fl, Q += gx[2] * fl;
                                iq = 1.0f / (Q > 1e-6f ? Q : 1e-6f);
                                const float u1 = U * iq, v1 = V * iq;
                                const float r = reciprocal[len];
                                run(q, len, fixed16(u0), fixed16((u1 - u0) * r), fixed16(v0), fixed16((v1 - v0) * r), s,
                                    ds, kind, &f);
                                s += ds * (uint32_t)len;
                                q += len;
                                left -= len;
                                u0 = u1, v0 = v1;
                        }
                }
                pixels += (uint32_t)n;
                if (xl < minx) minx = xl;
                if (xr > maxx) maxx = xr;
        }
        if (pixels != 0) bmds_3d_include(bounds, minx, row, maxx, end);
        return pixels;
}
