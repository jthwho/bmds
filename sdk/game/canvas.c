/**
 * @file      canvas.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's canvas (devplan/games.md §13): clipped drawing that records its damage.
 */

#include <stdint.h>
#include <string.h>
#include "game_internal.h"

/* A rectangle as its edges, the right and bottom ones excluded. */
typedef struct {
                int32_t x0, y0, x1, y1;
} edges_t;

static uint32_t bytes_per_pixel(uint32_t format) {
        return format == BMDS_PIXEL_L8 ? 1u : format == BMDS_PIXEL_RGB565 ? 2u : 3u;
}

static edges_t edges_of(const bmds_rect_t *r) {
        edges_t e = {r->x, r->y, (int32_t)(r->x + (int64_t)r->width), (int32_t)(r->y + (int64_t)r->height)};
        return e;
}

static bmds_rect_t rect_of(edges_t e) {
        bmds_rect_t r = {e.x0, e.y0, (uint32_t)(e.x1 - e.x0), (uint32_t)(e.y1 - e.y0)};
        return r;
}

/* @p e cut to the canvas's clip; empty: x1 <= x0 or y1 <= y0. */
static edges_t clipped(const bmds_canvas_t *c, edges_t e) {
        const edges_t k = edges_of(&c->clip);
        if (e.x0 < k.x0) e.x0 = k.x0;
        if (e.y0 < k.y0) e.y0 = k.y0;
        if (e.x1 > k.x1) e.x1 = k.x1;
        if (e.y1 > k.y1) e.y1 = k.y1;
        return e;
}

static int is_empty(edges_t e) {
        return e.x1 <= e.x0 || e.y1 <= e.y0;
}

static uint8_t *at(const bmds_canvas_t *c, int32_t x, int32_t y) {
        return c->pixels + (size_t)y * c->stride + (size_t)x * bytes_per_pixel(c->format);
}

static void damage_edges(bmds_canvas_t *c, edges_t e) {
        if (is_empty(e)) return;
        const bmds_rect_t r = rect_of(e);
        bmds_game_rects_add(c->damage, &c->damage_count, BMDS_CANVAS_MAX_DAMAGE, &r);
}

/* @p count pixels of @p pixel from @p p. */
static void put_run(uint8_t *p, uint32_t format, int32_t count, uint32_t pixel) {
        if (format == BMDS_PIXEL_L8) {
                memset(p, (int)(pixel & 0xffu), (size_t)count);
        } else if (format == BMDS_PIXEL_RGB565) {
                uint16_t *q = (uint16_t *)(void *)p;
                for (int32_t i = 0; i < count; ++i) q[i] = (uint16_t)pixel;
        } else {
                for (int32_t i = 0; i < count; ++i) {
                        p[3 * i] = (uint8_t)pixel;
                        p[3 * i + 1] = (uint8_t)(pixel >> 8);
                        p[3 * i + 2] = (uint8_t)(pixel >> 16);
                }
        }
}

/* Fills @p e (already clipped, not empty), recording no damage. */
static void fill_edges(bmds_canvas_t *c, edges_t e, uint32_t pixel) {
        const size_t row = (size_t)(e.x1 - e.x0) * bytes_per_pixel(c->format);
        uint8_t     *first = at(c, e.x0, e.y0);
        put_run(first, c->format, e.x1 - e.x0, pixel);
        for (int32_t y = e.y0 + 1; y < e.y1; ++y) memcpy(at(c, e.x0, y), first, row);
}

/* Whether the pixel at @p p is @p pixel. */
static int is_pixel(const uint8_t *p, uint32_t format, uint32_t pixel) {
        if (format == BMDS_PIXEL_L8) return p[0] == (uint8_t)pixel;
        if (format == BMDS_PIXEL_RGB565) return *(const uint16_t *)(const void *)p == (uint16_t)pixel;
        return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16) == (pixel & 0xffffffu);
}

static int64_t area_of(edges_t e) {
        return is_empty(e) ? 0 : (int64_t)(e.x1 - e.x0) * (e.y1 - e.y0);
}

/* Whether @p a and @p b meet or touch and their bounds cover at most a quarter more than they do. */
static int cheap_to_merge(edges_t a, edges_t b) {
        if (a.x0 > b.x1 || b.x0 > a.x1 || a.y0 > b.y1 || b.y0 > a.y1) return 0;
        const edges_t bounds = {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
                                a.y1 > b.y1 ? a.y1 : b.y1};
        const edges_t overlap = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1,
                                 a.y1 < b.y1 ? a.y1 : b.y1};
        const int64_t covered = area_of(a) + area_of(b) - area_of(overlap);
        return area_of(bounds) * 4 <= covered * 5;
}

void bmds_game_rects_add(bmds_rect_t *list, uint32_t *count, uint32_t max, const bmds_rect_t *rect) {
        if (rect->width == 0 || rect->height == 0) return;
        /* Merged with each one it meets or touches whose bounds with it add little (merging on
           touch alone chains: one rectangle ends up around everything). */
        edges_t  u = edges_of(rect);
        uint32_t i = 0;
        while (i < *count) {
                const edges_t e = edges_of(&list[i]);
                if (cheap_to_merge(u, e)) {
                        if (e.x0 < u.x0) u.x0 = e.x0;
                        if (e.y0 < u.y0) u.y0 = e.y0;
                        if (e.x1 > u.x1) u.x1 = e.x1;
                        if (e.y1 > u.y1) u.y1 = e.y1;
                        list[i] = list[--*count];
                        i = 0;
                } else {
                        ++i;
                }
        }
        if (*count < max) {
                list[(*count)++] = rect_of(u);
                return;
        }
        /* Full: the two (the new one among them) whose bounds add the least area merged, so many
           small changes stay many small rectangles rather than one holding them all. */
        bmds_rect_t    all[BMDS_CANVAS_MAX_DAMAGE + 1];
        const uint32_t n = max < BMDS_CANVAS_MAX_DAMAGE ? max : BMDS_CANVAS_MAX_DAMAGE;
        memcpy(all, list, sizeof(bmds_rect_t) * n);
        all[n] = rect_of(u);
        uint32_t pa = 0, pb = 1;
        int64_t  best = INT64_MAX;
        for (uint32_t a = 0; a <= n; ++a) {
                for (uint32_t b = a + 1; b <= n; ++b) {
                        const edges_t x = edges_of(&all[a]), y = edges_of(&all[b]);
                        const int64_t w = (int64_t)(x.x1 > y.x1 ? x.x1 : y.x1) - (x.x0 < y.x0 ? x.x0 : y.x0);
                        const int64_t h = (int64_t)(x.y1 > y.y1 ? x.y1 : y.y1) - (x.y0 < y.y0 ? x.y0 : y.y0);
                        const int64_t added =
                                w * h - (int64_t)all[a].width * all[a].height - (int64_t)all[b].width * all[b].height;
                        if (added < best) best = added, pa = a, pb = b;
                }
        }
        const edges_t x = edges_of(&all[pa]), y = edges_of(&all[pb]);
        const edges_t m = {x.x0 < y.x0 ? x.x0 : y.x0, x.y0 < y.y0 ? x.y0 : y.y0, x.x1 > y.x1 ? x.x1 : y.x1,
                           x.y1 > y.y1 ? x.y1 : y.y1};
        *count = 0;
        for (uint32_t k = 0; k <= n; ++k)
                if (k != pa && k != pb) list[(*count)++] = all[k];
        /* In their place, even where it overlaps others: merging those too would snowball into one
           rectangle again. Overlapping damage is only done twice. */
        list[(*count)++] = rect_of(m);
}

int bmds_canvas_init(bmds_canvas_t *canvas, void *pixels, uint32_t stride, int32_t width, int32_t height,
                     uint32_t format) {
        if (pixels == NULL || width <= 0 || height <= 0 ||
            (format != BMDS_PIXEL_L8 && format != BMDS_PIXEL_RGB565 && format != BMDS_PIXEL_BGR24) ||
            stride < (uint32_t)width * bytes_per_pixel(format))
                return -1;
        memset(canvas, 0, sizeof(*canvas));
        canvas->pixels = (uint8_t *)pixels;
        canvas->stride = stride;
        canvas->width = width;
        canvas->height = height;
        canvas->format = format;
        bmds_canvas_set_clip(canvas, NULL);
        return 0;
}

uint32_t bmds_canvas_pixel(const bmds_canvas_t *canvas, uint32_t rgb) {
        if (canvas->format == BMDS_PIXEL_L8) return rgb & 0xffu;
        if (canvas->format == BMDS_PIXEL_RGB565)
                return ((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu);
        return rgb & 0xffffffu;
}

void bmds_canvas_set_clip(bmds_canvas_t *canvas, const bmds_rect_t *rect) {
        edges_t e = {0, 0, canvas->width, canvas->height};
        if (rect != NULL) {
                const edges_t r = edges_of(rect);
                if (r.x0 > e.x0) e.x0 = r.x0;
                if (r.y0 > e.y0) e.y0 = r.y0;
                if (r.x1 < e.x1) e.x1 = r.x1;
                if (r.y1 < e.y1) e.y1 = r.y1;
                if (is_empty(e)) e.x1 = e.x0, e.y1 = e.y0;
        }
        canvas->clip = rect_of(e);
}

void bmds_canvas_fill(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t pixel) {
        if (width <= 0 || height <= 0) return;
        const edges_t e =
                clipped(canvas, (edges_t){x, y, (int32_t)((int64_t)x + width), (int32_t)((int64_t)y + height)});
        if (is_empty(e)) return;
        fill_edges(canvas, e, pixel);
        damage_edges(canvas, e);
}

void bmds_canvas_clear(bmds_canvas_t *canvas, uint32_t pixel) {
        bmds_canvas_fill(canvas, canvas->clip.x, canvas->clip.y, (int32_t)canvas->clip.width,
                         (int32_t)canvas->clip.height, pixel);
}

void bmds_canvas_rect(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t pixel) {
        if (width <= 0 || height <= 0) return;
        bmds_canvas_fill(canvas, x, y, width, 1, pixel);
        bmds_canvas_fill(canvas, x, y + height - 1, width, 1, pixel);
        bmds_canvas_fill(canvas, x, y + 1, 1, height - 2, pixel);
        bmds_canvas_fill(canvas, x + width - 1, y + 1, 1, height - 2, pixel);
}

void bmds_canvas_plot(bmds_canvas_t *canvas, int32_t x, int32_t y, uint32_t pixel) {
        bmds_canvas_fill(canvas, x, y, 1, 1, pixel);
}

void bmds_canvas_line(bmds_canvas_t *canvas, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t pixel) {
        /* Straight ones are fills. */
        if (y0 == y1) {
                const int32_t left = x0 < x1 ? x0 : x1;
                bmds_canvas_fill(canvas, left, y0, (x0 < x1 ? x1 - x0 : x0 - x1) + 1, 1, pixel);
                return;
        }
        if (x0 == x1) {
                const int32_t top = y0 < y1 ? y0 : y1;
                bmds_canvas_fill(canvas, x0, top, 1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1, pixel);
                return;
        }
        /* Bresenham's, each pixel clipped; the damage its bounds, once. */
        const edges_t k = edges_of(&canvas->clip);
        const int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
        const int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
        const edges_t bounds = {x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, (x0 < x1 ? x1 : x0) + 1, (y0 < y1 ? y1 : y0) + 1};
        int32_t       err = dx + dy, x = x0, y = y0;
        for (;;) {
                if (x >= k.x0 && x < k.x1 && y >= k.y0 && y < k.y1) put_run(at(canvas, x, y), canvas->format, 1, pixel);
                if (x == x1 && y == y1) break;
                const int32_t e2 = 2 * err;
                if (e2 >= dy) err += dy, x += sx;
                if (e2 <= dx) err += dx, y += sy;
        }
        damage_edges(canvas, clipped(canvas, bounds));
}

void bmds_canvas_blit(bmds_canvas_t *canvas, int32_t x, int32_t y, const bmds_game_image_t *image,
                      const bmds_rect_t *source) {
        if (image == NULL || image->format != canvas->format) return;
        /* The source, cut to the image; then where it lands, cut to the clip. */
        edges_t s = {0, 0, image->width, image->height};
        if (source != NULL) {
                const edges_t r = edges_of(source);
                if (r.x0 > s.x0) s.x0 = r.x0;
                if (r.y0 > s.y0) s.y0 = r.y0;
                if (r.x1 < s.x1) s.x1 = r.x1;
                if (r.y1 < s.y1) s.y1 = r.y1;
                x += s.x0 - r.x0;
                y += s.y0 - r.y0;
        }
        if (is_empty(s)) return;
        const edges_t d = clipped(canvas, (edges_t){x, y, x + (s.x1 - s.x0), y + (s.y1 - s.y0)});
        if (is_empty(d)) return;
        s.x0 += d.x0 - x;
        s.y0 += d.y0 - y;
        const uint32_t bpp = bytes_per_pixel(canvas->format);
        const int32_t  w = d.x1 - d.x0;
        for (int32_t row = 0; row < d.y1 - d.y0; ++row) {
                const uint8_t *from = image->pixels + (size_t)(s.y0 + row) * image->stride + (size_t)s.x0 * bpp;
                uint8_t       *to = at(canvas, d.x0, d.y0 + row);
                if ((image->flags & BMDS_GAME_IMAGE_KEYED) == 0) {
                        memcpy(to, from, (size_t)w * bpp);
                        continue;
                }
                for (int32_t i = 0; i < w; ++i, from += bpp, to += bpp)
                        if (!is_pixel(from, canvas->format, image->key)) memcpy(to, from, bpp);
        }
        damage_edges(canvas, d);
}

void bmds_canvas_damage(bmds_canvas_t *canvas, const bmds_rect_t *rect) {
        const edges_t c = {0, 0, canvas->width, canvas->height};
        edges_t       e = edges_of(rect);
        if (e.x0 < c.x0) e.x0 = c.x0;
        if (e.y0 < c.y0) e.y0 = c.y0;
        if (e.x1 > c.x1) e.x1 = c.x1;
        if (e.y1 > c.y1) e.y1 = c.y1;
        damage_edges(canvas, e);
}

void bmds_game_damage_in_clip(bmds_canvas_t *canvas, const bmds_rect_t *rect) {
        damage_edges(canvas, clipped(canvas, edges_of(rect)));
}

void bmds_canvas_damage_all(bmds_canvas_t *canvas) {
        canvas->damage_count = 1;
        canvas->damage[0] = rect_of((edges_t){0, 0, canvas->width, canvas->height});
}

void bmds_canvas_damage_clear(bmds_canvas_t *canvas) {
        canvas->damage_count = 0;
}

void bmds_canvas_copy(bmds_canvas_t *to, const bmds_canvas_t *from, const bmds_rect_t *rects, uint32_t count) {
        if (to->format != from->format || to->width != from->width || to->height != from->height) return;
        const uint32_t bpp = bytes_per_pixel(to->format);
        for (uint32_t i = 0; i < count; ++i) {
                edges_t e = edges_of(&rects[i]);
                if (e.x0 < 0) e.x0 = 0;
                if (e.y0 < 0) e.y0 = 0;
                if (e.x1 > to->width) e.x1 = to->width;
                if (e.y1 > to->height) e.y1 = to->height;
                if (is_empty(e)) continue;
                for (int32_t y = e.y0; y < e.y1; ++y)
                        memcpy(at(to, e.x0, y), at(from, e.x0, y), (size_t)(e.x1 - e.x0) * bpp);
        }
}
