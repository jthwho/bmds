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
                if (bpp == 2) { /* RGB565: a word at a time, no call per pixel */
                        const uint16_t *f = (const uint16_t *)(const void *)from;
                        uint16_t       *t = (uint16_t *)(void *)to;
                        const uint16_t  key = (uint16_t)image->key;
                        for (int32_t i = 0; i < w; ++i) {
                                const uint16_t v = f[i];
                                if (v != key) t[i] = v;
                        }
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

/* The canvas's edges clipped to it. */
static edges_t in_canvas(const bmds_canvas_t *c, edges_t e) {
        if (e.x0 < 0) e.x0 = 0;
        if (e.y0 < 0) e.y0 = 0;
        if (e.x1 > c->width) e.x1 = c->width;
        if (e.y1 > c->height) e.y1 = c->height;
        return e;
}

void bmds_game_copy_except(bmds_canvas_t *to, const bmds_canvas_t *from, const bmds_rect_t *rects, uint32_t count,
                           const bmds_rect_t *except, uint32_t excepts) {
        if (to->format != from->format || to->width != from->width || to->height != from->height) return;
        const uint32_t bpp = bytes_per_pixel(to->format);
        for (uint32_t i = 0; i < count; ++i) {
                const edges_t r = in_canvas(to, edges_of(&rects[i]));
                if (is_empty(r)) continue;
                /* Bands of rows across which the same rectangles are left out: each band's rows are
                   the gaps between them. */
                int32_t y = r.y0;
                while (y < r.y1) {
                        int32_t next = r.y1;
                        int32_t gx0[BMDS_GAME_REPAINTED_MAX], gx1[BMDS_GAME_REPAINTED_MAX];
                        int     n = 0;
                        for (uint32_t k = 0; k < excepts; ++k) {
                                const edges_t e = edges_of(&except[k]);
                                if (e.x1 <= r.x0 || e.x0 >= r.x1 || is_empty(e)) continue;
                                if (e.y0 > y) {
                                        if (e.y0 < next) next = e.y0;
                                        continue;
                                }
                                if (e.y1 <= y) continue;
                                if (e.y1 < next) next = e.y1;
                                if (n == (int)BMDS_GAME_REPAINTED_MAX) continue; /* never: excepts are fewer */
                                /* Kept sorted by left edge. */
                                int at_n = n++;
                                while (at_n > 0 && gx0[at_n - 1] > e.x0) {
                                        gx0[at_n] = gx0[at_n - 1];
                                        gx1[at_n] = gx1[at_n - 1];
                                        --at_n;
                                }
                                gx0[at_n] = e.x0;
                                gx1[at_n] = e.x1;
                        }
                        /* The spans of the band not left out. */
                        int32_t x = r.x0;
                        for (int k = 0; k <= n && x < r.x1; ++k) {
                                const int32_t end = k < n ? (gx0[k] < r.x1 ? gx0[k] : r.x1) : r.x1;
                                if (end > x)
                                        for (int32_t row = y; row < next; ++row)
                                                memcpy(at(to, x, row), at(from, x, row), (size_t)(end - x) * bpp);
                                if (k < n && gx1[k] > x) x = gx1[k];
                        }
                        y = next;
                }
        }
        return;
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

/* ---- Scaled, turned, shaded and blended drawing (devplan/luckyduck.md §4) */

/* @p source cut to @p image's bounds; empty: x1 <= x0. */
static edges_t source_of(const bmds_game_image_t *image, const bmds_rect_t *source) {
        edges_t s = {0, 0, image->width, image->height};
        if (source != NULL) {
                const edges_t r = edges_of(source);
                if (r.x0 > s.x0) s.x0 = r.x0;
                if (r.y0 > s.y0) s.y0 = r.y0;
                if (r.x1 < s.x1) s.x1 = r.x1;
                if (r.y1 < s.y1) s.y1 = r.y1;
        }
        return s;
}

/* Copies the pixel at @p from to @p to unless it is the image's key. */
static inline void put_keyed(uint8_t *to, const uint8_t *from, const bmds_game_image_t *image, uint32_t bpp) {
        if (bpp == 2) {
                const uint16_t v = *(const uint16_t *)(const void *)from;
                if ((image->flags & BMDS_GAME_IMAGE_KEYED) == 0 || v != (uint16_t)image->key)
                        *(uint16_t *)(void *)to = v;
                return;
        }
        if ((image->flags & BMDS_GAME_IMAGE_KEYED) != 0 && is_pixel(from, image->format, image->key)) return;
        memcpy(to, from, bpp);
}

void bmds_canvas_blit_scaled(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height,
                             const bmds_game_image_t *image, const bmds_rect_t *source) {
        if (image == NULL || image->format != canvas->format || width == 0 || height == 0) return;
        const edges_t s = source_of(image, source);
        if (is_empty(s)) return;
        const int     mirror_x = width < 0, mirror_y = height < 0;
        const int32_t w = mirror_x ? -width : width, h = mirror_y ? -height : height;
        const edges_t d = clipped(canvas, (edges_t){x, y, (int32_t)((int64_t)x + w), (int32_t)((int64_t)y + h)});
        if (is_empty(d)) return;
        const uint32_t bpp = bytes_per_pixel(canvas->format);
        const int32_t  sw = s.x1 - s.x0, sh = s.y1 - s.y0;
        /* Each destination pixel's centre mapped into the source, in 1/65536ths. */
        const int64_t step_x = ((int64_t)sw << 16) / w, step_y = ((int64_t)sh << 16) / h;
        for (int32_t row = d.y0; row < d.y1; ++row) {
                int32_t sy = (int32_t)(((int64_t)(row - y) * step_y + step_y / 2) >> 16);
                if (sy >= sh) sy = sh - 1;
                if (mirror_y) sy = sh - 1 - sy;
                const uint8_t *line = image->pixels + (size_t)(s.y0 + sy) * image->stride;
                uint8_t       *to = at(canvas, d.x0, row);
                int64_t        u = (int64_t)(d.x0 - x) * step_x + step_x / 2;
                if (bpp == 2 && step_x <= INT32_MAX / 4 && u + step_x * (d.x1 - d.x0) <= INT32_MAX) {
                        /* RGB565 in 32 bits, the core's width (a card's flip, a sprite's size). */
                        const uint16_t *from = (const uint16_t *)(const void *)(line + (size_t)s.x0 * 2);
                        uint16_t       *p = (uint16_t *)(void *)to;
                        const uint16_t  key = (uint16_t)image->key;
                        const int       keyed = (image->flags & BMDS_GAME_IMAGE_KEYED) != 0;
                        const int32_t   step = (int32_t)step_x;
                        int32_t         v = (int32_t)u;
                        for (int32_t col = d.x0; col < d.x1; ++col, ++p, v += step) {
                                int32_t sx = v >> 16;
                                if (sx >= sw) sx = sw - 1;
                                const uint16_t px = from[mirror_x ? sw - 1 - sx : sx];
                                if (!keyed || px != key) *p = px;
                        }
                        continue;
                }
                for (int32_t col = d.x0; col < d.x1; ++col, to += bpp, u += step_x) {
                        int32_t sx = (int32_t)(u >> 16);
                        if (sx >= sw) sx = sw - 1;
                        put_keyed(to, line + (size_t)(s.x0 + (mirror_x ? sw - 1 - sx : sx)) * bpp, image, bpp);
                }
        }
        damage_edges(canvas, d);
}

/* The sine of @p turn (of BMDS_GAME_TURN) in 1/65536ths, without libm: a quarter's by its
   Taylor polynomial to the ninth power (within a 65536th), the rest by symmetry. */
static int32_t sine16(uint32_t turn) {
        turn &= BMDS_GAME_TURN - 1;
        const uint32_t quarter = BMDS_GAME_TURN / 4;
        const int      negative = turn >= BMDS_GAME_TURN / 2;
        uint32_t       t = turn % (BMDS_GAME_TURN / 2);
        if (t > quarter) t = BMDS_GAME_TURN / 2 - t;
        /* sin(pi/2 * u) for u = t / quarter in [0, 1], in Horner's form: doubles are software on
           the P4, but a dozen operations a call are nothing next to the pixels. */
        const double  u = (double)t / quarter, u2 = u * u;
        const double  v = u * (1.5707963267948966 +
                              u2 * (-0.6459640975062462 +
                                    u2 * (0.0796926262461670 + u2 * (-0.0046817541353187 + u2 * 0.0001604411847874))));
        const int32_t r = (int32_t)(v * 65536.0 + 0.5);
        return negative ? -r : r;
}

/* floor(a / b), b > 0. */
static int64_t floor_div(int64_t a, int64_t b) {
        const int64_t q = a / b;
        return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* Narrows [@p k0, @p k1) to the steps k at which @p from + k * @p step lies in [0, @p limit).
   @return Whether any are left. */
static int span(int64_t from, int64_t step, int64_t limit, int32_t *k0, int32_t *k1) {
        int64_t lo, hi; /* k in [lo, hi] */
        if (step == 0) {
                if (from < 0 || from >= limit) return 0;
                return *k0 < *k1;
        }
        if (step > 0) {
                lo = -floor_div(from, step); /* ceil(-from / step) */
                hi = floor_div(limit - 1 - from, step);
        } else {
                lo = -floor_div(limit - 1 - from, -step); /* ceil((from - (limit - 1)) / -step) */
                hi = floor_div(from, -step);
        }
        if (lo > *k0) *k0 = lo > *k1 ? *k1 : (int32_t)lo;
        if (hi + 1 < *k1) *k1 = hi + 1 < *k0 ? *k0 : (int32_t)(hi + 1);
        return *k0 < *k1;
}

void bmds_canvas_blit_rotated(bmds_canvas_t *canvas, int32_t cx, int32_t cy, uint32_t turn, uint32_t scale,
                              const bmds_game_image_t *image, const bmds_rect_t *source) {
        if (image == NULL || image->format != canvas->format || scale == 0) return;
        const edges_t s = source_of(image, source);
        if (is_empty(s)) return;
        const int32_t sw = s.x1 - s.x0, sh = s.y1 - s.y0;
        const int32_t sn = sine16(turn), cs = sine16(turn + BMDS_GAME_TURN / 4);
        /* The turned picture's bounds: half of each side's reach, plus a pixel for rounding. */
        const int64_t asn = sn < 0 ? -sn : sn, acs = cs < 0 ? -cs : cs;
        const int32_t ex = (int32_t)(((acs * sw + asn * sh) * scale / BMDS_GAME_SCALE) >> 17) + 1;
        const int32_t ey = (int32_t)(((asn * sw + acs * sh) * scale / BMDS_GAME_SCALE) >> 17) + 1;
        const edges_t d = clipped(canvas, (edges_t){cx - ex, cy - ey, cx + ex + 1, cy + ey + 1});
        if (is_empty(d)) return;
        /* The inverse: a destination pixel's offset from the centre (dx, dy) comes from
           u = (dx cos + dy sin) / k, v = (dy cos - dx sin) / k, k the scale; in 1/65536ths. */
        const int64_t  ux = (int64_t)cs * BMDS_GAME_SCALE / scale, vx = -(int64_t)sn * BMDS_GAME_SCALE / scale;
        const int64_t  uy = (int64_t)sn * BMDS_GAME_SCALE / scale, vy = (int64_t)cs * BMDS_GAME_SCALE / scale;
        const uint32_t bpp = bytes_per_pixel(canvas->format);
        const int64_t  half_w = (int64_t)sw << 15, half_h = (int64_t)sh << 15;
        const int      keyed = (image->flags & BMDS_GAME_IMAGE_KEYED) != 0;
        for (int32_t row = d.y0; row < d.y1; ++row) {
                /* The first pixel's centre, from the centre (cx, cy) a pixel's corner. */
                const int64_t dy2 = 2 * (int64_t)(row - cy) + 1, dx2 = 2 * (int64_t)(d.x0 - cx) + 1;
                const int64_t u0 = ((dx2 * ux + dy2 * uy) >> 1) + half_w;
                const int64_t v0 = ((dx2 * vx + dy2 * vy) >> 1) + half_h;
                /* The columns whose (u, v) fall inside the source, worked out once a row: a turned
                   picture covers about half its bounds, and no pixel is tested. */
                int32_t k0 = 0, k1 = d.x1 - d.x0;
                if (!span(u0, ux, (int64_t)sw << 16, &k0, &k1) || !span(v0, vx, (int64_t)sh << 16, &k0, &k1)) continue;
                /* 32 bits from here: the core's own width (|u|, |v| stay within the source's reach). */
                int32_t        u = (int32_t)(u0 + ux * k0), v = (int32_t)(v0 + vx * k0);
                const int32_t  dux = (int32_t)ux, dvx = (int32_t)vx;
                uint8_t       *to = at(canvas, d.x0 + k0, row);
                const uint8_t *base = image->pixels + (size_t)s.y0 * image->stride + (size_t)s.x0 * bpp;
                if (bpp == 2) {
                        uint16_t      *p = (uint16_t *)(void *)to;
                        const uint16_t key = (uint16_t)image->key;
                        for (int32_t k = k0; k < k1; ++k, ++p, u += dux, v += dvx) {
                                const uint16_t px =
                                        *(const uint16_t *)(const void *)(base + (size_t)(v >> 16) * image->stride +
                                                                          (size_t)(u >> 16) * 2);
                                if (!keyed || px != key) *p = px;
                        }
                        continue;
                }
                for (int32_t k = k0; k < k1; ++k, to += bpp, u += dux, v += dvx)
                        put_keyed(to, base + (size_t)(v >> 16) * image->stride + (size_t)(u >> 16) * bpp, image, bpp);
        }
        damage_edges(canvas, d);
}

/* @p p's pixel darkened to @p level of 32. */
static void shade_pixel(uint8_t *p, uint32_t format, uint32_t level) {
        if (format == BMDS_PIXEL_RGB565) {
                const uint32_t v = *(uint16_t *)(void *)p;
                /* Red and blue together, green alone: neither product reaches the other's bits. */
                *(uint16_t *)(void *)p =
                        (uint16_t)((((v & 0xf81fu) * level) >> 5 & 0xf81fu) | (((v & 0x07e0u) * level) >> 5 & 0x07e0u));
        } else if (format == BMDS_PIXEL_BGR24) {
                for (int i = 0; i < 3; ++i) p[i] = (uint8_t)((p[i] * level) >> 5);
        }
}

static uint32_t level32(uint32_t level) {
        return level >= BMDS_GAME_OPAQUE ? 32u : level / (BMDS_GAME_OPAQUE / 32);
}

void bmds_canvas_shade(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t level) {
        if (width <= 0 || height <= 0) return;
        const edges_t d =
                clipped(canvas, (edges_t){x, y, (int32_t)((int64_t)x + width), (int32_t)((int64_t)y + height)});
        if (is_empty(d) || canvas->format == BMDS_PIXEL_L8) return;
        const uint32_t l = level32(level), bpp = bytes_per_pixel(canvas->format);
        for (int32_t row = d.y0; row < d.y1; ++row) {
                uint8_t *p = at(canvas, d.x0, row);
                for (int32_t col = d.x0; col < d.x1; ++col, p += bpp) shade_pixel(p, canvas->format, l);
        }
        damage_edges(canvas, d);
}

void bmds_canvas_shade_image(bmds_canvas_t *canvas, int32_t x, int32_t y, const bmds_game_image_t *image,
                             const bmds_rect_t *source, uint32_t level) {
        if (image == NULL || image->format != canvas->format || canvas->format == BMDS_PIXEL_L8) return;
        edges_t s = source_of(image, source);
        if (source != NULL) {
                x += s.x0 - source->x;
                y += s.y0 - source->y;
        }
        if (is_empty(s)) return;
        const edges_t d = clipped(canvas, (edges_t){x, y, x + (s.x1 - s.x0), y + (s.y1 - s.y0)});
        if (is_empty(d)) return;
        s.x0 += d.x0 - x;
        s.y0 += d.y0 - y;
        const uint32_t l = level32(level), bpp = bytes_per_pixel(canvas->format);
        const int      keyed = (image->flags & BMDS_GAME_IMAGE_KEYED) != 0;
        for (int32_t row = 0; row < d.y1 - d.y0; ++row) {
                const uint8_t *from = image->pixels + (size_t)(s.y0 + row) * image->stride + (size_t)s.x0 * bpp;
                uint8_t       *to = at(canvas, d.x0, d.y0 + row);
                for (int32_t i = 0; i < d.x1 - d.x0; ++i, from += bpp, to += bpp)
                        if (!keyed || !is_pixel(from, image->format, image->key)) shade_pixel(to, canvas->format, l);
        }
        damage_edges(canvas, d);
}

void bmds_canvas_fill_blend(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t rgb,
                            uint32_t alpha) {
        if (width <= 0 || height <= 0) return;
        if (canvas->format == BMDS_PIXEL_L8) {
                if (alpha >= BMDS_GAME_OPAQUE / 2) bmds_canvas_fill(canvas, x, y, width, height, rgb);
                return;
        }
        const edges_t d =
                clipped(canvas, (edges_t){x, y, (int32_t)((int64_t)x + width), (int32_t)((int64_t)y + height)});
        if (is_empty(d)) return;
        const uint32_t a = level32(alpha), bpp = bytes_per_pixel(canvas->format);
        if (canvas->format == BMDS_PIXEL_RGB565) {
                /* Green spread to the top half: the three fields apart, blended in one multiply each. */
                const uint32_t c = bmds_canvas_pixel(canvas, rgb);
                const uint32_t over = ((c | c << 16) & 0x07e0f81fu) * a;
                for (int32_t row = d.y0; row < d.y1; ++row) {
                        uint16_t *p = (uint16_t *)(void *)at(canvas, d.x0, row);
                        for (int32_t col = d.x0; col < d.x1; ++col, ++p) {
                                const uint32_t under = ((uint32_t)*p | (uint32_t)*p << 16) & 0x07e0f81fu;
                                const uint32_t m = ((under * (32 - a) + over) >> 5) & 0x07e0f81fu;
                                *p = (uint16_t)(m | m >> 16);
                        }
                }
        } else {
                const uint8_t c[3] = {(uint8_t)rgb, (uint8_t)(rgb >> 8), (uint8_t)(rgb >> 16)};
                for (int32_t row = d.y0; row < d.y1; ++row) {
                        uint8_t *p = at(canvas, d.x0, row);
                        for (int32_t col = d.x0; col < d.x1; ++col, p += bpp)
                                for (int i = 0; i < 3; ++i) p[i] = (uint8_t)((p[i] * (32 - a) + c[i] * a) >> 5);
                }
        }
        damage_edges(canvas, d);
}
