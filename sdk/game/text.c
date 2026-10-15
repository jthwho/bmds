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

int32_t bmds_canvas_text(bmds_canvas_t *canvas, int32_t x, int32_t y, const char *text, uint32_t pixel, int32_t scale) {
        if (scale < 1) scale = 1;
        int32_t       cx = x, cy = y, widest = 0;
        bmds_canvas_t quiet = *canvas; /* draws without damage: the text's bounds are added once */
        quiet.damage_count = 0;
        while (*text != '\0') {
                const uint32_t c = next_char(&text);
                if (c == '\n') {
                        cx = x;
                        cy += BMDS_GAME_GLYPH_HEIGHT * scale;
                        continue;
                }
                const unsigned char *g = glyph_of(c);
                for (int32_t row = 0; row < BMDS_GAME_GLYPH_HEIGHT; ++row) {
                        const unsigned bits = g[row];
                        if (bits == 0) continue;
                        /* Runs of set bits as fills. */
                        for (int32_t col = 0; col < BMDS_GAME_GLYPH_WIDTH;) {
                                if ((bits & (0x80u >> col)) == 0) {
                                        ++col;
                                        continue;
                                }
                                int32_t end = col;
                                while (end < BMDS_GAME_GLYPH_WIDTH && (bits & (0x80u >> end)) != 0) ++end;
                                bmds_canvas_fill(&quiet, cx + col * scale, cy + row * scale, (end - col) * scale, scale,
                                                 pixel);
                                col = end;
                        }
                }
                cx += BMDS_GAME_GLYPH_WIDTH * scale;
                if (cx - x > widest) widest = cx - x;
        }
        if (widest > 0) {
                const bmds_rect_t r = {x, y, (uint32_t)widest, (uint32_t)(cy - y + BMDS_GAME_GLYPH_HEIGHT * scale)};
                bmds_game_damage_in_clip(canvas, &r);
        }
        return widest;
}
