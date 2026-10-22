/**
 * @file      shapes.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's shapes (devplan/scripting.md step 4): ellipses (and so circles), outlined and
 * filled, filled polygons, flood fill, and reading pixels back. Drawn as runs of fills; the
 * damage is the shape's bounds, once.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game_internal.h"

/* Draws with bmds_canvas_fill, then records only @p bounds as the damage (not each run's). */
typedef struct {
                bmds_rect_t damage[BMDS_CANVAS_MAX_DAMAGE];
                uint32_t    count;
} saved_damage_t;

/* The damage set aside, and the list emptied: each run's fill then merges into a list of a few
   rectangles, not into all the frame's so far (a full list made a small circle's rows cost ~2 us
   each). */
static void save_damage(bmds_canvas_t *c, saved_damage_t *s) {
        memcpy(s->damage, c->damage, sizeof(s->damage));
        s->count = c->damage_count;
        c->damage_count = 0;
        return;
}

static void restore_damage(bmds_canvas_t *c, const saved_damage_t *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        memcpy(c->damage, s->damage, sizeof(s->damage));
        c->damage_count = s->count;
        if (x1 <= x0 || y1 <= y0) return;
        const bmds_rect_t bounds = {x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
        bmds_game_damage_in_clip(c, &bounds);
        return;
}

/* Half the width of an ellipse of radii @p rx, @p ry at row @p y from its centre: the pixels
   whose centres are within radii half a pixel larger (a circle then matches the midpoint
   algorithm's: radius 5's top row is 5 pixels, not 1). */
static int32_t half_width(int32_t rx, int32_t ry, int32_t y) {
        const float ey = (float)ry + 0.5f;
        const float t = 1.0f - ((float)y * (float)y) / (ey * ey);
        return t <= 0.0f ? 0 : (int32_t)(((float)rx + 0.5f) * sqrtf(t));
}

void bmds_canvas_ellipse(bmds_canvas_t *canvas, int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t pixel) {
        if (rx < 0 || ry < 0) return;
        saved_damage_t saved;
        save_damage(canvas, &saved);
        for (int32_t y = -ry; y <= ry; ++y) {
                /* Each row from the next row out's edge to its own, so the outline has no gaps. */
                const int32_t ay = y < 0 ? -y : y;
                const int32_t outer = half_width(rx, ry, ay);
                const int32_t inner = ay == ry ? -1 : half_width(rx, ry, ay + 1);
                int32_t       from = inner + 1;
                if (from > outer) from = outer;
                const int32_t n = outer - from + 1;
                bmds_canvas_fill(canvas, cx + from, cy + y, n, 1, pixel);
                if (from > 0 || outer > 0)
                        bmds_canvas_fill(canvas, cx - outer, cy + y, from == 0 ? n - 1 : n, 1, pixel);
        }
        restore_damage(canvas, &saved, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
        return;
}

void bmds_canvas_fill_ellipse(bmds_canvas_t *canvas, int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t pixel) {
        if (rx < 0 || ry < 0) return;
        saved_damage_t saved;
        save_damage(canvas, &saved);
        for (int32_t y = -ry; y <= ry; ++y) {
                const int32_t w = half_width(rx, ry, y < 0 ? -y : y);
                bmds_canvas_fill(canvas, cx - w, cy + y, 2 * w + 1, 1, pixel);
        }
        restore_damage(canvas, &saved, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
        return;
}

static int compare_floats(const void *a, const void *b) {
        const float x = *(const float *)a, y = *(const float *)b;
        return x < y ? -1 : x > y ? 1 : 0;
}

int bmds_canvas_fill_polygon(bmds_canvas_t *canvas, const int32_t *xy, uint32_t points, uint32_t pixel) {
        if (xy == NULL || points < 3) return 0;
        int32_t top = xy[1], bottom = xy[1], left = xy[0], right = xy[0];
        for (uint32_t i = 1; i < points; ++i) {
                const int32_t x = xy[2 * i], y = xy[2 * i + 1];
                if (y < top) top = y;
                if (y > bottom) bottom = y;
                if (x < left) left = x;
                if (x > right) right = x;
        }
        /* Rows outside the clip aren't scanned. */
        const int32_t clip_top = canvas->clip.y, clip_bottom = canvas->clip.y + (int32_t)canvas->clip.height - 1;
        const int32_t y0 = top > clip_top ? top : clip_top, y1 = bottom < clip_bottom ? bottom : clip_bottom;
        float        *crossings = malloc(sizeof(float) * points);
        if (crossings == NULL) return -1;
        saved_damage_t saved;
        save_damage(canvas, &saved);
        for (int32_t y = y0; y <= y1; ++y) {
                /* Where the edges cross the row's middle; filled between pairs (even-odd). */
                const float yc = (float)y + 0.5f;
                uint32_t    n = 0;
                for (uint32_t i = 0; i < points; ++i) {
                        const uint32_t j = i + 1 == points ? 0 : i + 1;
                        const float    ax = (float)xy[2 * i], ay = (float)xy[2 * i + 1];
                        const float    bx = (float)xy[2 * j], by = (float)xy[2 * j + 1];
                        if ((ay <= yc && by > yc) || (by <= yc && ay > yc))
                                crossings[n++] = ax + (yc - ay) * (bx - ax) / (by - ay);
                }
                qsort(crossings, n, sizeof(float), compare_floats);
                for (uint32_t k = 0; k + 1 < n; k += 2) {
                        const int32_t xa = (int32_t)ceilf(crossings[k] - 0.5f);
                        const int32_t xb = (int32_t)floorf(crossings[k + 1] - 0.5f);
                        if (xb >= xa) bmds_canvas_fill(canvas, xa, y, xb - xa + 1, 1, pixel);
                }
        }
        free(crossings);
        restore_damage(canvas, &saved, left, top, right + 1, bottom + 1);
        return 0;
}

static uint8_t *pixel_at(const bmds_canvas_t *c, int32_t x, int32_t y) {
        const uint32_t bpp = c->format == BMDS_PIXEL_L8 ? 1u : c->format == BMDS_PIXEL_RGB565 ? 2u : 3u;
        return c->pixels + (size_t)y * c->stride + (size_t)x * bpp;
}

uint32_t bmds_canvas_get(const bmds_canvas_t *canvas, int32_t x, int32_t y) {
        if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) return 0;
        const uint8_t *p = pixel_at(canvas, x, y);
        if (canvas->format == BMDS_PIXEL_L8) return p[0];
        if (canvas->format == BMDS_PIXEL_RGB565) return *(const uint16_t *)(const void *)p;
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

uint32_t bmds_canvas_rgb(const bmds_canvas_t *canvas, uint32_t pixel) {
        if (canvas->format == BMDS_PIXEL_L8) return pixel & 0xffu;
        if (canvas->format == BMDS_PIXEL_RGB565) {
                /* Each channel's top bits repeated into its bottom ones, so white is 0xffffff. */
                const uint32_t r = (pixel >> 11) & 0x1fu, g = (pixel >> 5) & 0x3fu, b = pixel & 0x1fu;
                return ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
        }
        return pixel & 0xffffffu;
}

int bmds_canvas_flood_fill(bmds_canvas_t *canvas, int32_t x, int32_t y, uint32_t pixel) {
        const int32_t cx0 = canvas->clip.x, cy0 = canvas->clip.y;
        const int32_t cx1 = cx0 + (int32_t)canvas->clip.width, cy1 = cy0 + (int32_t)canvas->clip.height;
        if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return 0;
        const uint32_t mask = canvas->format == BMDS_PIXEL_L8       ? 0xffu
                              : canvas->format == BMDS_PIXEL_RGB565 ? 0xffffu
                                                                    : 0xffffffu;
        const uint32_t target = bmds_canvas_get(canvas, x, y);
        if (target == (pixel & mask)) return 0;
        /* Spans: each popped seed is widened to its run of the target, filled, and the rows above
           and below seeded where the target continues. */
        size_t   capacity = 256, count = 0;
        int32_t *stack = malloc(sizeof(int32_t) * 2 * capacity);
        if (stack == NULL) return -1;
        saved_damage_t saved;
        save_damage(canvas, &saved);
        int32_t minx = x, maxx = x, miny = y, maxy = y;
        stack[count * 2] = x;
        stack[count * 2 + 1] = y;
        ++count;
        int result = 0;
        while (count > 0) {
                --count;
                const int32_t sx = stack[count * 2], sy = stack[count * 2 + 1];
                if (bmds_canvas_get(canvas, sx, sy) != target) continue;
                int32_t a = sx, b = sx;
                while (a > cx0 && bmds_canvas_get(canvas, a - 1, sy) == target) --a;
                while (b + 1 < cx1 && bmds_canvas_get(canvas, b + 1, sy) == target) ++b;
                bmds_canvas_fill(canvas, a, sy, b - a + 1, 1, pixel);
                if (a < minx) minx = a;
                if (b > maxx) maxx = b;
                if (sy < miny) miny = sy;
                if (sy > maxy) maxy = sy;
                for (int dir = -1; dir <= 1; dir += 2) {
                        const int32_t ny = sy + dir;
                        if (ny < cy0 || ny >= cy1) continue;
                        int in_run = 0;
                        for (int32_t i = a; i <= b; ++i) {
                                const int is_target = bmds_canvas_get(canvas, i, ny) == target;
                                if (is_target && !in_run) {
                                        if (count == capacity) {
                                                int32_t *grown = realloc(stack, sizeof(int32_t) * 4 * capacity);
                                                if (grown == NULL) {
                                                        result = -1;
                                                        goto done;
                                                }
                                                stack = grown;
                                                capacity *= 2;
                                        }
                                        stack[count * 2] = i;
                                        stack[count * 2 + 1] = ny;
                                        ++count;
                                }
                                in_run = is_target;
                        }
                }
        }
done:
        free(stack);
        restore_damage(canvas, &saved, minx, miny, maxx + 1, maxy + 1);
        return result;
}
