/**
 * @file      texmap.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's texture maps over a whole canvas (devplan/3d.md §8): a canvas as an image, an
 * affine map with wrapping (rotozoomers, floors) and a lookup-table map (tunnels, lenses). Fixed
 * point, texels by mask (power-of-two images), nothing divided or tested a pixel.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "game_internal.h"

static int is_pow2(int64_t v) {
        return v > 0 && (v & (v - 1)) == 0;
}

static uint32_t log2_of(uint32_t v) {
        uint32_t n = 0;
        while (v > 1u) v >>= 1, ++n;
        return n;
}

static uint32_t bytes_per_pixel(uint32_t format) {
        return format == BMDS_PIXEL_L8 ? 1u : format == BMDS_PIXEL_RGB565 ? 2u : 3u;
}

/* (x, y, width, height) cut to @p c's clip: its edges in @p e; 0 when nothing is left. */
static int clip_to(const bmds_canvas_t *c, int32_t x, int32_t y, int32_t width, int32_t height, int32_t *e) {
        if (width <= 0 || height <= 0) return 0;
        int64_t x0 = x, y0 = y, x1 = (int64_t)x + width, y1 = (int64_t)y + height;
        if (x0 < c->clip.x) x0 = c->clip.x;
        if (y0 < c->clip.y) y0 = c->clip.y;
        if (x1 > (int64_t)c->clip.x + c->clip.width) x1 = (int64_t)c->clip.x + c->clip.width;
        if (y1 > (int64_t)c->clip.y + c->clip.height) y1 = (int64_t)c->clip.y + c->clip.height;
        if (x1 <= x0 || y1 <= y0) return 0;
        e[0] = (int32_t)x0, e[1] = (int32_t)y0, e[2] = (int32_t)x1, e[3] = (int32_t)y1;
        return 1;
}

static void damage(bmds_canvas_t *c, const int32_t *e) {
        const bmds_rect_t r = {e[0], e[1], (uint32_t)(e[2] - e[0]), (uint32_t)(e[3] - e[1])};
        bmds_canvas_damage(c, &r);
        return;
}

int bmds_canvas_image(const bmds_canvas_t *canvas, const bmds_rect_t *rect, bmds_game_image_t *image) {
        if (canvas == NULL || image == NULL) return -BMDS_EINVAL;
        int64_t x0 = 0, y0 = 0, x1 = canvas->width, y1 = canvas->height;
        if (rect != NULL) {
                if (rect->x > x0) x0 = rect->x;
                if (rect->y > y0) y0 = rect->y;
                if ((int64_t)rect->x + rect->width < x1) x1 = (int64_t)rect->x + rect->width;
                if ((int64_t)rect->y + rect->height < y1) y1 = (int64_t)rect->y + rect->height;
        }
        if (x1 <= x0 || y1 <= y0) return -BMDS_EINVAL;
        memset(image, 0, sizeof(*image));
        image->pixels = canvas->pixels + (size_t)y0 * canvas->stride + (size_t)x0 * bytes_per_pixel(canvas->format);
        image->stride = canvas->stride;
        image->width = (int32_t)(x1 - x0);
        image->height = (int32_t)(y1 - y0);
        image->format = canvas->format;
        return 0;
}

/* @p x texels in 16.16, wrapped by 65536 texels (a power-of-two texture's width divides it). */
static int32_t wrapped16(float x) {
        const float w = (x - floorf(x * (1.0f / 65536.0f)) * 65536.0f) * 65536.0f;
        return w >= 0.0f && w < 4294967040.0f ? (int32_t)(uint32_t)w : 0; /* rounded up to 2^32: 0 */
}

/* A gradient in 16.16, held in range. */
static int32_t step16(float x) {
        const float v = x * 65536.0f + (x < 0.0f ? -0.5f : 0.5f);
        return (int32_t)(v < 2.0e9f ? (v > -2.0e9f ? v : -2.0e9f) : 2.0e9f); /* NaN: the top */
}

void bmds_game_affine_rotozoom(bmds_game_affine_t *map, int32_t x, int32_t y, float cx, float cy, float u, float v,
                               float angle, float zoom) {
        if (!(zoom > 0.0f) && !(zoom < 0.0f)) zoom = 1.0f;
        const float k = 1.0f / zoom, c = cosf(angle) * k, s = sinf(angle) * k;
        /* A screen offset (dx, dy) from (cx, cy) is the texel offset (dx c + dy s, dy c - dx s). */
        const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
        map->u = wrapped16(u + dx * c + dy * s);
        map->v = wrapped16(v + dy * c - dx * s);
        map->du_dx = step16(c);
        map->dv_dx = step16(-s);
        map->du_dy = step16(s);
        map->dv_dy = step16(c);
        return;
}

/* L8, the stride a power of two (at most 65536): the row's offset straight from v, four a store. */
static void affine_l8(uint8_t *q, int32_t n, uint32_t u, uint32_t v, uint32_t du, uint32_t dv, const uint8_t *tex,
                      uint32_t um, uint32_t vm, uint32_t vs) {
#define TEXEL_L8() (t = tex[((v >> vs) & vm) | ((u >> 16) & um)], u += du, v += dv, t)
        uint32_t t;
        while (n > 0 && ((uintptr_t)q & 3u) != 0) {
                *q++ = (uint8_t)TEXEL_L8();
                --n;
        }
        for (; n >= 4; n -= 4, q += 4) {
                const uint32_t p0 = TEXEL_L8();
                const uint32_t p1 = TEXEL_L8();
                const uint32_t p2 = TEXEL_L8();
                const uint32_t p3 = TEXEL_L8();
                *(uint32_t *)(void *)q = p0 | p1 << 8 | p2 << 16 | p3 << 24;
        }
        while (n-- > 0) *q++ = (uint8_t)TEXEL_L8();
#undef TEXEL_L8
        return;
}

/* RGB565, the stride a power of two: byte offsets from u and v, two pixels a store. */
static void affine_565(uint16_t *q, int32_t n, uint32_t u, uint32_t v, uint32_t du, uint32_t dv, const uint8_t *tex,
                       uint32_t um2, uint32_t vm, uint32_t vs) {
#define TEXEL_565()                                                                                                    \
        (t = *(const uint16_t *)(const void *)(tex + (((v >> vs) & vm) | ((u >> 15) & um2))), u += du, v += dv, t)
        uint32_t t;
        if (n > 0 && ((uintptr_t)q & 2u) != 0) {
                *q++ = (uint16_t)TEXEL_565();
                --n;
        }
        for (; n >= 2; n -= 2, q += 2) {
                const uint32_t p0 = TEXEL_565();
                const uint32_t p1 = TEXEL_565();
                *(uint32_t *)(void *)q = p0 | p1 << 16;
        }
        if (n > 0) *q = (uint16_t)TEXEL_565();
#undef TEXEL_565
        return;
}

/* Any format and stride: a multiply a pixel for the row. */
static void affine_any(uint8_t *q, int32_t n, uint32_t u, uint32_t v, uint32_t du, uint32_t dv,
                       const bmds_game_image_t *image, uint32_t bpp) {
        const uint8_t *tex = image->pixels;
        const uint32_t um = (uint32_t)image->width - 1u, vm = (uint32_t)image->height - 1u, stride = image->stride;
        for (int32_t i = 0; i < n; ++i, q += bpp, u += du, v += dv) {
                const uint8_t *p = tex + (size_t)((v >> 16) & vm) * stride + (size_t)((u >> 16) & um) * bpp;
                q[0] = p[0];
                if (bpp > 1) q[1] = p[1];
                if (bpp > 2) q[2] = p[2];
        }
        return;
}

int bmds_canvas_texture_affine(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height,
                               const bmds_game_image_t *image, const bmds_game_affine_t *map) {
        if (canvas == NULL || image == NULL || map == NULL || image->pixels == NULL) return -BMDS_EINVAL;
        if (image->format != canvas->format || !is_pow2(image->width) || !is_pow2(image->height)) return -BMDS_EINVAL;
        int32_t e[4];
        if (!clip_to(canvas, x, y, width, height, e)) return 0;
        const uint32_t bpp = bytes_per_pixel(canvas->format);
        const uint32_t ox = (uint32_t)(e[0] - x), oy = (uint32_t)(e[1] - y);
        /* Unsigned: the coordinates wrap modulo 2^32, which the masks follow. */
        uint32_t       ur = (uint32_t)map->u + ox * (uint32_t)map->du_dx + oy * (uint32_t)map->du_dy;
        uint32_t       vr = (uint32_t)map->v + ox * (uint32_t)map->dv_dx + oy * (uint32_t)map->dv_dy;
        const uint32_t du = (uint32_t)map->du_dx, dv = (uint32_t)map->dv_dx;
        const int      fast = is_pow2(image->stride) && image->stride <= 65536u && bpp < 3;
        const uint32_t sshift = log2_of(image->stride);
        const uint32_t vm = ((uint32_t)image->height - 1u) << sshift, vs = 16u - sshift;
        const int32_t  n = e[2] - e[0];
        uint8_t       *line = canvas->pixels + (size_t)e[1] * canvas->stride + (size_t)e[0] * bpp;
        for (int32_t row = e[1]; row < e[3]; ++row, line += canvas->stride) {
                if (!fast)
                        affine_any(line, n, ur, vr, du, dv, image, bpp);
                else if (bpp == 1)
                        affine_l8(line, n, ur, vr, du, dv, image->pixels, (uint32_t)image->width - 1u, vm, vs);
                else
                        affine_565((uint16_t *)(void *)line, n, ur, vr, du, dv, image->pixels,
                                   ((uint32_t)image->width - 1u) << 1, vm, vs);
                ur += (uint32_t)map->du_dy;
                vr += (uint32_t)map->dv_dy;
        }
        damage(canvas, e);
        return 0;
}

int bmds_canvas_remap(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height,
                      const bmds_game_image_t *image, const uint16_t *table, uint32_t table_stride, uint32_t du,
                      uint32_t dv) {
        if (canvas == NULL || image == NULL || table == NULL || image->pixels == NULL) return -BMDS_EINVAL;
        if (image->format != canvas->format || !is_pow2(image->width) || !is_pow2(image->height) ||
            image->width > 256 || image->height > 256)
                return -BMDS_EINVAL;
        int32_t e[4];
        if (!clip_to(canvas, x, y, width, height, e)) return 0;
        const uint32_t bpp = bytes_per_pixel(canvas->format), stride = image->stride;
        const uint32_t um = (uint32_t)image->width - 1u, vm = (uint32_t)image->height - 1u;
        /* The motion's low bytes: u and v wrap by their masks (at most 256 texels). */
        const uint32_t  ud = du & 0xffu, vd = dv & 0xffu;
        const int32_t   n = e[2] - e[0];
        const uint8_t  *tex = image->pixels;
        uint8_t        *line = canvas->pixels + (size_t)e[1] * canvas->stride + (size_t)e[0] * bpp;
        const uint16_t *from = table + (size_t)(e[1] - y) * table_stride + (size_t)(e[0] - x);
        for (int32_t row = e[1]; row < e[3]; ++row, line += canvas->stride, from += table_stride) {
                const uint16_t *t = from;
                if (bpp == 1) {
#define REMAP_L8() tex[(((uint32_t)(*t >> 8) + vd) & vm) * stride + (((uint32_t)(*t & 0xffu) + ud) & um)]
                        uint8_t *q = line;
                        int32_t  k = n;
                        while (k > 0 && ((uintptr_t)q & 3u) != 0) {
                                *q++ = REMAP_L8();
                                ++t, --k;
                        }
                        for (; k >= 4; k -= 4, q += 4) {
                                const uint32_t p0 = REMAP_L8();
                                ++t;
                                const uint32_t p1 = REMAP_L8();
                                ++t;
                                const uint32_t p2 = REMAP_L8();
                                ++t;
                                const uint32_t p3 = REMAP_L8();
                                ++t;
                                *(uint32_t *)(void *)q = p0 | p1 << 8 | p2 << 16 | p3 << 24;
                        }
                        for (; k > 0; --k, ++t) *q++ = REMAP_L8();
#undef REMAP_L8
                        continue;
                }
                uint8_t *q = line;
                for (int32_t i = 0; i < n; ++i, ++t, q += bpp) {
                        const uint8_t *p = tex + (size_t)((((uint32_t)(*t >> 8) + vd) & vm) * stride) +
                                           (size_t)((((uint32_t)(*t & 0xffu) + ud) & um) * bpp);
                        q[0] = p[0];
                        q[1] = p[1];
                        if (bpp > 2) q[2] = p[2];
                }
        }
        damage(canvas, e);
        return 0;
}
