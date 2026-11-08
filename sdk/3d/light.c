/**
 * @file      light.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's light tables (devplan/3d.md §5a): Doom's COLORMAP, `table[shade][texel]`, the
 * nearest palette entry to each texel's colour at each shade. Exact, and quick: the candidates
 * sorted by a weighted brightness (3r + 4g + 2b), each search starting where that brightness is and
 * going outwards (up, then down) until no candidate further along can be nearer (by Cauchy and Schwarz, a
 * brightness difference d means a weighted distance of at least d² / 9); and a small cache of
 * colours already found (dark shades repeat).
 */

#include <stdint.h>
#include <string.h>
#include "3d_internal.h"

typedef struct {
                int16_t r, g, b, key; /* key: 3r + 4g + 2b, 0..2295 */
                uint8_t index;
} candidate_t;

#define CACHE 256u

static int32_t key_of(int32_t r, int32_t g, int32_t b) {
        return 3 * r + 4 * g + 2 * b;
}

/* The nearest of @p n candidates (sorted by key) to (r, g, b): its palette index; of equals, the
   lowest. Upwards from the colour's brightness, then downwards, each until brightness alone rules
   the rest out (a candidate exactly at the bound is still compared: it may tie). Two plain loops
   compare a fifth more candidates than one choosing the nearer side each time, and take half the
   time (no branch a candidate to guess). */
static uint8_t nearest(const candidate_t *c, int32_t n, int32_t r, int32_t g, int32_t b) {
        const int32_t key = key_of(r, g, b);
        int32_t       lo = 0, hi = n;
        while (lo < hi) { /* the first at or above key */
                const int32_t m = (lo + hi) >> 1;
                if (c[m].key < key)
                        lo = m + 1;
                else
                        hi = m;
        }
        uint32_t best = UINT32_MAX / 9u; /* above any distance (9 × 255² at most); 9 × it fits */
        uint8_t  found = 0;
        for (int32_t i = lo; i < n; ++i) {
                const int32_t dk = c[i].key - key;
                if ((uint32_t)(dk * dk) > 9u * best) break;
                const int32_t  dr = c[i].r - r, dg = c[i].g - g, db = c[i].b - b;
                const uint32_t d = (uint32_t)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
                if (d < best || (d == best && c[i].index < found)) best = d, found = c[i].index;
        }
        for (int32_t i = lo - 1; i >= 0; --i) {
                const int32_t dk = key - c[i].key;
                if ((uint32_t)(dk * dk) > 9u * best) break;
                const int32_t  dr = c[i].r - r, dg = c[i].g - g, db = c[i].b - b;
                const uint32_t d = (uint32_t)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
                if (d < best || (d == best && c[i].index < found)) best = d, found = c[i].index;
        }
        return found;
}

int bmds_3d_light_table(uint8_t *table, const uint32_t *palette, uint32_t first, uint32_t count, uint32_t dark) {
        if (table == NULL || palette == NULL || count == 0 || first > 255u || count > 256u - first) return -BMDS_EINVAL;
        /* The candidates, by key (insertion: at most 256, once). */
        candidate_t c[256];
        int32_t     n = 0;
        for (uint32_t i = first; i < first + count; ++i) {
                candidate_t x;
                x.r = (int16_t)(palette[i] >> 16 & 0xffu);
                x.g = (int16_t)(palette[i] >> 8 & 0xffu);
                x.b = (int16_t)(palette[i] & 0xffu);
                x.key = (int16_t)key_of(x.r, x.g, x.b);
                x.index = (uint8_t)i;
                int32_t j = n++;
                while (j > 0 && c[j - 1].key > x.key) {
                        c[j] = c[j - 1];
                        --j;
                }
                c[j] = x;
        }
        uint32_t cache_rgb[CACHE];
        uint8_t  cache_index[CACHE];
        memset(cache_rgb, 0xff, sizeof(cache_rgb)); /* no colour is 0xffffffff */
        const int32_t dr = (int32_t)(dark >> 16 & 0xffu), dg = (int32_t)(dark >> 8 & 0xffu),
                      db = (int32_t)(dark & 0xffu);
        const uint32_t top = BMDS_3D_LIGHT_SHADES - 1;
        for (uint32_t s = 0; s <= top; ++s) {
                const int32_t w = (int32_t)s, wd = (int32_t)(top - s), half = (int32_t)top / 2;
                uint8_t      *out = table + s * 256u;
                for (uint32_t i = 0; i < 256u; ++i) {
                        const uint32_t p = palette[i];
                        const int32_t  r = (dr * wd + (int32_t)(p >> 16 & 0xffu) * w + half) / (int32_t)top;
                        const int32_t  g = (dg * wd + (int32_t)(p >> 8 & 0xffu) * w + half) / (int32_t)top;
                        const int32_t  b = (db * wd + (int32_t)(p & 0xffu) * w + half) / (int32_t)top;
                        const uint32_t rgb = (uint32_t)(r << 16 | g << 8 | b);
                        const uint32_t slot = (rgb ^ rgb >> 8 ^ rgb >> 16) & (CACHE - 1u);
                        if (cache_rgb[slot] != rgb) {
                                cache_rgb[slot] = rgb;
                                cache_index[slot] = nearest(c, n, r, g, b);
                        }
                        out[i] = cache_index[slot];
                }
        }
        return 0;
}
