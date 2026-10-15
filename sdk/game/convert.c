/**
 * @file      convert.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's image conversion (devplan/games.md §13): decoded BGRA32 pixels into a canvas's
 * format, the transparent ones into its key. No system calls: tested on the host.
 */

#include "game_internal.h"

int bmds_game_converter_init(bmds_game_converter_t *converter, const bmds_game_load_t *how) {
        if (how == NULL || how->size < sizeof(bmds_game_load_t)) return -1;
        converter->how = how;
        converter->have_last = 0;
        switch (how->format) {
                case BMDS_PIXEL_L8:
                        if (how->palette == NULL || how->palette_count == 0 || how->palette_count > 256 ||
                            how->key > 255)
                                return -1;
                        converter->bpp = 1;
                        converter->key = how->key;
                        return 0;
                case BMDS_PIXEL_RGB565:
                        converter->bpp = 2;
                        converter->key = BMDS_GAME_KEY_RGB565;
                        return 0;
                case BMDS_PIXEL_BGR24:
                        converter->bpp = 3;
                        converter->key = BMDS_GAME_KEY_BGR24;
                        return 0;
                default: return -1;
        }
}

/* The palette's nearest index to @p rgb (the key's left out), weighted as the eye sees. */
static uint32_t nearest(bmds_game_converter_t *c, uint32_t rgb) {
        if (c->have_last && c->last_rgb == rgb) return c->last_index;
        const bmds_game_load_t *how = c->how;
        const int32_t r = (int32_t)(rgb >> 16 & 0xffu), g = (int32_t)(rgb >> 8 & 0xffu), b = (int32_t)(rgb & 0xffu);
        uint32_t      best = how->key == 0 ? 1 : 0, best_distance = UINT32_MAX;
        for (uint32_t i = 0; i < how->palette_count; ++i) {
                if (i == how->key) continue;
                const uint32_t p = how->palette[i];
                const int32_t  dr = r - (int32_t)(p >> 16 & 0xffu), dg = g - (int32_t)(p >> 8 & 0xffu),
                              db = b - (int32_t)(p & 0xffu);
                const uint32_t distance = (uint32_t)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
                if (distance < best_distance) {
                        best_distance = distance;
                        best = i;
                        if (distance == 0) break;
                }
        }
        c->last_rgb = rgb;
        c->last_index = best;
        c->have_last = 1;
        return best;
}

uint32_t bmds_game_convert(bmds_game_converter_t *converter, const uint8_t *in, uint8_t *out, uint32_t count) {
        const bmds_game_load_t *how = converter->how;
        const int               keyed_rgb = (how->flags & BMDS_GAME_LOAD_KEY_RGB) != 0;
        uint32_t                transparent = 0;
        for (uint32_t i = 0; i < count; ++i, in += 4, out += converter->bpp) {
                const uint32_t rgb = (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16;
                const int      clear = in[3] < 128 || (keyed_rgb && rgb == (how->key_rgb & 0xffffffu));
                uint32_t       v;
                if (clear) {
                        v = converter->key;
                        ++transparent;
                } else if (how->format == BMDS_PIXEL_L8) {
                        v = nearest(converter, rgb);
                } else if (how->format == BMDS_PIXEL_RGB565) {
                        v = (rgb >> 8 & 0xf800u) | (rgb >> 5 & 0x07e0u) | (rgb >> 3 & 0x001fu);
                        if (v == BMDS_GAME_KEY_RGB565) v |= 0x0020u; /* opaque: a step of green off the key */
                } else {
                        v = rgb;
                        if (v == BMDS_GAME_KEY_BGR24) v |= 0x000100u;
                }
                /* Bytes written after this pixel's are read: in place, nothing unread is overwritten. */
                out[0] = (uint8_t)v;
                if (converter->bpp > 1) out[1] = (uint8_t)(v >> 8);
                if (converter->bpp > 2) out[2] = (uint8_t)(v >> 16);
        }
        return transparent;
}
