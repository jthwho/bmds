/**
 * @file      text.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's text (devplan/games.md §13): unscii's 8x16 glyphs, ASCII and Latin-1.
 */

#include "game_internal.h"

/* The next character of UTF-8 @p *text (advanced past it); a malformed byte is one '?'. */
static uint32_t next_char(const char **text) {
        const uint8_t *p = (const uint8_t *)*text;
        uint32_t       c = p[0];
        int more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (more < 0) {
                *text += 1;
                return '?';
        }
        c = more == 0 ? c : more == 1 ? (c & 0x1f) : more == 2 ? (c & 0x0f) : (c & 0x07);
        for (int i = 1; i <= more; ++i) {
                if ((p[i] & 0xc0) != 0x80) {
                        *text += i;
                        return '?';
                }
                c = c << 6 | (p[i] & 0x3fu);
        }
        *text += 1 + more;
        return c;
}

/* The glyph for @p c: its own, or '?'. */
static const unsigned char *glyph_of(uint32_t c) {
        if (c >= 0x20 && c <= 0x7e) return bmds_game_font16[c - 0x20];
        if (c >= 0xa0 && c <= 0xff) return bmds_game_font16[c - 0xa0 + 95];
        return bmds_game_font16['?' - 0x20];
}

int32_t bmds_game_text_width(const char *text, int32_t scale) {
        if (scale < 1) scale = 1;
        int32_t widest = 0, line = 0;
        while (*text != '\0') {
                if (next_char(&text) == '\n') {
                        line = 0;
                        continue;
                }
                line += BMDS_GAME_GLYPH_WIDTH * scale;
                if (line > widest) widest = line;
        }
        return widest;
}

/* Pixel @p pixel at @p p, @p bpp bytes (1, 2 or 3; little-endian, as the canvas keeps them). */
static void put_pixel(uint8_t *p, uint32_t bpp, uint32_t pixel) {
        p[0] = (uint8_t)pixel;
        if (bpp > 1) p[1] = (uint8_t)(pixel >> 8);
        if (bpp > 2) p[2] = (uint8_t)(pixel >> 16);
}

/* A glyph's set bits written straight into the canvas, each a scale x scale block, cut to @p k
   (the clip's edges): a fill a run of bits cost ~2 us a call, damage merging included (a line of
   nine letters was 0.6 ms); the damage is the text's bounds, added once by the caller. */
static void draw_glyph(bmds_canvas_t *c, const unsigned char *g, int32_t x, int32_t y, int32_t scale, uint32_t pixel,
                       const int32_t k[4], uint32_t bpp) {
        if (x >= k[2] || y >= k[3] || x + BMDS_GAME_GLYPH_WIDTH * scale <= k[0] ||
            y + BMDS_GAME_GLYPH_HEIGHT * scale <= k[1])
                return;
        for (int32_t row = 0; row < BMDS_GAME_GLYPH_HEIGHT; ++row) {
                const unsigned bits = g[row];
                if (bits == 0) continue;
                int32_t y0 = y + row * scale, y1 = y0 + scale;
                if (y0 < k[1]) y0 = k[1];
                if (y1 > k[3]) y1 = k[3];
                if (y1 <= y0) continue;
                for (int32_t col = 0; col < BMDS_GAME_GLYPH_WIDTH; ++col) {
                        if ((bits & (0x80u >> col)) == 0) continue;
                        int32_t x0 = x + col * scale, x1 = x0 + scale;
                        if (x0 < k[0]) x0 = k[0];
                        if (x1 > k[2]) x1 = k[2];
                        for (int32_t py = y0; py < y1; ++py) {
                                uint8_t *p = c->pixels + (size_t)py * c->stride + (size_t)x0 * bpp;
                                for (int32_t px = x0; px < x1; ++px, p += bpp) put_pixel(p, bpp, pixel);
                        }
                }
        }
}

int32_t bmds_canvas_text(bmds_canvas_t *canvas, int32_t x, int32_t y, const char *text, uint32_t pixel, int32_t scale) {
        if (scale < 1) scale = 1;
        const uint32_t bpp = canvas->format == BMDS_PIXEL_L8 ? 1u : canvas->format == BMDS_PIXEL_RGB565 ? 2u : 3u;
        const int32_t  k[4] = {canvas->clip.x, canvas->clip.y, (int32_t)(canvas->clip.x + (int64_t)canvas->clip.width),
                               (int32_t)(canvas->clip.y + (int64_t)canvas->clip.height)};
        int32_t        cx = x, cy = y, widest = 0;
        while (*text != '\0') {
                const uint32_t c = next_char(&text);
                if (c == '\n') {
                        cx = x;
                        cy += BMDS_GAME_GLYPH_HEIGHT * scale;
                        continue;
                }
                draw_glyph(canvas, glyph_of(c), cx, cy, scale, pixel, k, bpp);
                cx += BMDS_GAME_GLYPH_WIDTH * scale;
                if (cx - x > widest) widest = cx - x;
        }
        if (widest > 0) {
                const bmds_rect_t r = {x, y, (uint32_t)widest, (uint32_t)(cy - y + BMDS_GAME_GLYPH_HEIGHT * scale)};
                bmds_game_damage_in_clip(canvas, &r);
        }
        return widest;
}
