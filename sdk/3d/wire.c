/**
 * @file      wire.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's lines and points (devplan/3d.md §4e). A segment is clipped against the near and far
 * planes in clip space, projected, clipped to the drawing area (Liang and Barsky's), then stepped
 * along its major axis in 16.16 fixed point from the exact sub-pixel ends (so a turning object's
 * lines move smoothly), with its shade stepped beside it.
 */

#include <math.h>
#include "3d_internal.h"

/* An end of a segment on the screen: position and shade (in the ink's shades, 0..count-1). */
typedef struct {
                float x, y, s;
} end_t;

static float shade_of(const bmds_3d_ink_t *ink, float view_z) {
        if (ink->count <= 1) return 0.0f;
        float i = 1.0f - (-view_z - ink->fog_near) * ink->fog_scale;
        if (i < 0.0f) i = 0.0f;
        if (i > 1.0f) i = 1.0f;
        return i * (float)(ink->count - 1);
}

void bmds_3d_include(bmds_3d_bounds_t *b, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        if (b->x1 <= b->x0) {
                b->x0 = x0, b->y0 = y0, b->x1 = x1, b->y1 = y1;
                return;
        }
        if (x0 < b->x0) b->x0 = x0;
        if (y0 < b->y0) b->y0 = y0;
        if (x1 > b->x1) b->x1 = x1;
        if (y1 > b->y1) b->y1 = y1;
        return;
}

/* Cuts the parameter range [t0, t1] where f(t) = fa + (fb - fa) t >= 0. @return 0 if none is left. */
static int cut(float fa, float fb, float *t0, float *t1) {
        if (fa < 0.0f && fb < 0.0f) return 0;
        if (fa >= 0.0f && fb >= 0.0f) return 1;
        const float t = fa / (fa - fb);
        if (fa < 0.0f) {
                if (t > *t0) *t0 = t;
        } else if (t < *t1) {
                *t1 = t;
        }
        return *t0 <= *t1;
}

static float clamp_f(float v, float lo, float hi) {
        return v < lo ? lo : v > hi ? hi : v;
}

/* Steps from @p a to @p b along x (|dx| >= |dy|), @p a.x <= @p b.x, inside @p area. */
#define STEP_LINE(PUT)                                                                                                 \
        do {                                                                                                           \
                for (int32_t i = i0; i <= i1; ++i) {                                                                   \
                        int32_t minor = fy >> 16;                                                                      \
                        if (minor < lo) minor = lo;                                                                    \
                        if (minor > hi) minor = hi;                                                                    \
                        int32_t sh = fs >> 16;                                                                         \
                        if (sh < 0) sh = 0;                                                                            \
                        if (sh > top) sh = top;                                                                        \
                        PUT;                                                                                           \
                        fy += dy;                                                                                      \
                        fs += ds;                                                                                      \
                }                                                                                                      \
        } while (0)

/* Draws the projected segment @p a - @p b (both inside @p area) on @p c. @return the pixels. */
static uint32_t raster(bmds_canvas_t *c, const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, end_t a, end_t b,
                       bmds_3d_bounds_t *bounds) {
        const int     x_major = fabsf(b.x - a.x) >= fabsf(b.y - a.y);
        const int32_t top = (int32_t)ink->count - 1;
        /* Along the major axis (u), the minor (v): x and y, or y and x. */
        float ua = x_major ? a.x : a.y, ub = x_major ? b.x : b.y, va = x_major ? a.y : a.x, vb = x_major ? b.y : b.x;
        float sa = a.s, sb = b.s;
        if (ua > ub) {
                float t = ua;
                ua = ub, ub = t;
                t = va, va = vb, vb = t;
                t = sa, sa = sb, sb = t;
        }
        const int32_t umin = x_major ? area->ix0 : area->iy0, umax = (x_major ? area->ix1 : area->iy1) - 1;
        const int32_t vmin = x_major ? area->iy0 : area->ix0, vmax = (x_major ? area->iy1 : area->ix1) - 1;
        int32_t       i0 = (int32_t)floorf(ua), i1 = (int32_t)floorf(ub);
        if (i0 < umin) i0 = umin;
        if (i1 > umax) i1 = umax;
        if (i1 < i0) return 0;
        int32_t lo = (int32_t)floorf(va < vb ? va : vb), hi = (int32_t)floorf(va < vb ? vb : va);
        if (lo < vmin) lo = vmin;
        if (hi > vmax) hi = vmax;
        if (hi < lo) return 0;
        const float     du = ub - ua;
        const float     slope = du > 0.0f ? (vb - va) / du : 0.0f, sslope = du > 0.0f ? (sb - sa) / du : 0.0f;
        const float     first = (float)i0 + 0.5f - ua; /* the first pixel's centre past the start */
        int32_t         fy = (int32_t)((va + first * slope) * 65536.0f), dy = (int32_t)(slope * 65536.0f);
        int32_t         fs = (int32_t)((sa + first * sslope + 0.5f) * 65536.0f), ds = (int32_t)(sslope * 65536.0f);
        const uint32_t *shades = ink->shades;
        const size_t    stride = c->stride;
        if (x_major)
                bmds_3d_include(bounds, i0, lo, i1 + 1, hi + 1);
        else
                bmds_3d_include(bounds, lo, i0, hi + 1, i1 + 1);

        if (c->format == BMDS_PIXEL_L8) {
                if (x_major)
                        STEP_LINE(c->pixels[(size_t)minor * stride + (size_t)i] = (uint8_t)shades[sh]);
                else
                        STEP_LINE(c->pixels[(size_t)i * stride + (size_t)minor] = (uint8_t)shades[sh]);
        } else if (c->format == BMDS_PIXEL_RGB565) {
                if (x_major)
                        STEP_LINE(*(uint16_t *)(void *)(c->pixels + (size_t)minor * stride + 2 * (size_t)i) =
                                          (uint16_t)shades[sh]);
                else
                        STEP_LINE(*(uint16_t *)(void *)(c->pixels + (size_t)i * stride + 2 * (size_t)minor) =
                                          (uint16_t)shades[sh]);
        } else {
                uint8_t *p;
                if (x_major)
                        STEP_LINE((p = c->pixels + (size_t)minor * stride + 3 * (size_t)i, p[0] = (uint8_t)shades[sh],
                                   p[1] = (uint8_t)(shades[sh] >> 8), p[2] = (uint8_t)(shades[sh] >> 16)));
                else
                        STEP_LINE((p = c->pixels + (size_t)i * stride + 3 * (size_t)minor, p[0] = (uint8_t)shades[sh],
                                   p[1] = (uint8_t)(shades[sh] >> 8), p[2] = (uint8_t)(shades[sh] >> 16)));
        }
        return (uint32_t)(i1 - i0 + 1);
}

static void viewport(const bmds_3d_t *ctx, float *x, float *y, float *w, float *h) {
        const bmds_canvas_t *c = ctx->target;
        *x = (float)(ctx->viewport_whole ? 0 : ctx->vx);
        *y = (float)(ctx->viewport_whole ? 0 : ctx->vy);
        *w = (float)(ctx->viewport_whole ? c->width : ctx->vw);
        *h = (float)(ctx->viewport_whole ? c->height : ctx->vh);
        return;
}

int bmds_3d_segment(bmds_3d_t *ctx, const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, const bmds_3d_vertex_t *a,
                    const bmds_3d_vertex_t *b, bmds_3d_bounds_t *bounds) {
        end_t e[2];
        if (((a->oc | b->oc) & BMDS_3D_OUT_DEPTH) == 0 && a->cw > 0.0f && b->cw > 0.0f) {
                /* Both ends between near and far: projected already (bmds_3d_screen()). */
                e[0].x = a->sx, e[0].y = a->sy, e[0].s = shade_of(ink, a->vz);
                e[1].x = b->sx, e[1].y = b->sy, e[1].s = shade_of(ink, b->vz);
        } else {
                /* Near (z >= -w) and far (z <= w) in clip space. */
                float t0 = 0.0f, t1 = 1.0f;
                if (!cut(a->cw + a->cz, b->cw + b->cz, &t0, &t1)) return 0;
                if (!cut(a->cw - a->cz, b->cw - b->cz, &t0, &t1)) return 0;
                const bmds_3d_viewport_t vp = bmds_3d_viewport(ctx);
                for (int k = 0; k < 2; ++k) {
                        const float t = k == 0 ? t0 : t1;
                        const float cx = a->cx + (b->cx - a->cx) * t, cy = a->cy + (b->cy - a->cy) * t;
                        const float cw = a->cw + (b->cw - a->cw) * t, vz = a->vz + (b->vz - a->vz) * t;
                        if (!(cw > 0.0f)) return 0;
                        const float inv = 1.0f / cw;
                        e[k].x = vp.x + (cx * inv + 1.0f) * 0.5f * vp.w;
                        e[k].y = vp.y + (1.0f - cy * inv) * 0.5f * vp.h;
                        e[k].s = shade_of(ink, vz);
                }
        }
        /* The area, its far edges just inside (pixels there are its last). */
        const float x1 = area->x1 - 1e-3f, y1 = area->y1 - 1e-3f;
        if (!isfinite(e[0].x) || !isfinite(e[0].y) || !isfinite(e[1].x) || !isfinite(e[1].y)) return 0;
        float       s0 = 0.0f, s1 = 1.0f;
        const float dx = e[1].x - e[0].x, dy = e[1].y - e[0].y;
        if (!cut(e[0].x - area->x0, e[1].x - area->x0, &s0, &s1) || !cut(x1 - e[0].x, x1 - e[1].x, &s0, &s1) ||
            !cut(e[0].y - area->y0, e[1].y - area->y0, &s0, &s1) || !cut(y1 - e[0].y, y1 - e[1].y, &s0, &s1))
                return 0;
        const float ds = e[1].s - e[0].s;
        end_t       q0 = {e[0].x + dx * s0, e[0].y + dy * s0, e[0].s + ds * s0};
        end_t       q1 = {e[0].x + dx * s1, e[0].y + dy * s1, e[0].s + ds * s1};
        q0.x = clamp_f(q0.x, area->x0, x1), q0.y = clamp_f(q0.y, area->y0, y1);
        q1.x = clamp_f(q1.x, area->x0, x1), q1.y = clamp_f(q1.y, area->y0, y1);
        const uint32_t n = raster(ctx->target, area, ink, q0, q1, bounds);
        ctx->stats.pixels += n;
        return n != 0;
}

int bmds_3d_point(bmds_3d_t *ctx, const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, const bmds_3d_vertex_t *v,
                  int32_t size, bmds_3d_bounds_t *bounds) {
        if (!(v->cw > 0.0f) || v->cx < -v->cw || v->cx > v->cw || v->cy < -v->cw || v->cy > v->cw || v->cz < -v->cw ||
            v->cz > v->cw)
                return 0;
        float vx, vy, vw, vh;
        viewport(ctx, &vx, &vy, &vw, &vh);
        const float inv = 1.0f / v->cw;
        const float x = vx + (v->cx * inv + 1.0f) * 0.5f * vw, y = vy + (1.0f - v->cy * inv) * 0.5f * vh;
        const float half = 0.5f * (float)(size - 1);
        int32_t     x0 = (int32_t)floorf(x - half), y0 = (int32_t)floorf(y - half);
        int32_t     x1 = x0 + size, y1 = y0 + size;
        if (x0 < area->ix0) x0 = area->ix0;
        if (y0 < area->iy0) y0 = area->iy0;
        if (x1 > area->ix1) x1 = area->ix1;
        if (y1 > area->iy1) y1 = area->iy1;
        if (x1 <= x0 || y1 <= y0) return 0;
        const uint32_t pixel = ink->shades[(uint32_t)(shade_of(ink, v->vz) + 0.5f)];
        bmds_canvas_t *c = ctx->target;
        for (int32_t row = y0; row < y1; ++row) {
                uint8_t *p = c->pixels + (size_t)row * c->stride;
                for (int32_t col = x0; col < x1; ++col) {
                        if (c->format == BMDS_PIXEL_L8) {
                                p[col] = (uint8_t)pixel;
                        } else if (c->format == BMDS_PIXEL_RGB565) {
                                ((uint16_t *)(void *)p)[col] = (uint16_t)pixel;
                        } else {
                                p[3 * col] = (uint8_t)pixel;
                                p[3 * col + 1] = (uint8_t)(pixel >> 8);
                                p[3 * col + 2] = (uint8_t)(pixel >> 16);
                        }
                }
        }
        bmds_3d_include(bounds, x0, y0, x1, y1);
        ctx->stats.pixels += (uint32_t)((x1 - x0) * (y1 - y0));
        return 1;
}
