/**
 * @file      ui.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's drawing helpers (devplan/luckyduck.md §3, §9).
 */

#include <string.h>
#include "ui.h"

static const art_glyph_t *glyph(int font, char ch) {
        const unsigned char u = (unsigned char)ch;
        if (u < 32 || u > 126) return NULL;
        const art_glyph_t *g = &art_fonts[font].glyph[u - 32];
        return g->advance != 0 ? g : NULL;
}

/* The kerning between @p a and @p b in font @p font (pixels; negative: closer). */
static int32_t kern(int font, char a, char b) {
        const art_kern_t *k = art_kern[font];
        uint32_t          lo = 0, hi = art_kern_count[font];
        const uint32_t    want = (uint32_t)(unsigned char)a << 8 | (unsigned char)b;
        while (lo < hi) {
                const uint32_t mid = (lo + hi) / 2, at = (uint32_t)k[mid].a << 8 | k[mid].b;
                if (at == want) return k[mid].dx;
                if (at < want)
                        lo = mid + 1;
                else
                        hi = mid;
        }
        return 0;
}

int32_t ui_text_width(int font, const char *text) {
        int32_t w = 0;
        for (const char *p = text; *p != '\0'; ++p) {
                const art_glyph_t *g = glyph(font, *p);
                if (g != NULL) w += g->advance + (p[1] != '\0' ? kern(font, p[0], p[1]) : 0);
        }
        return w;
}

int32_t ui_text(bmds_canvas_t *c, int font, int32_t x, int32_t y, const char *text) {
        int32_t pen = x;
        for (const char *p = text; *p != '\0'; ++p) {
                const art_glyph_t *g = glyph(font, *p);
                if (g == NULL) continue;
                if (*p != ' ') {
                        const bmds_rect_t src = {g->x, g->y, g->w, g->h};
                        bmds_canvas_blit(c, pen - g->left, y - g->top, &app.font, &src);
                }
                pen += g->advance + (p[1] != '\0' ? kern(font, p[0], p[1]) : 0);
        }
        return pen - x;
}

void ui_text_centred(bmds_canvas_t *c, int font, int32_t cx, int32_t y, const char *text) {
        (void)ui_text(c, font, cx - ui_text_width(font, text) / 2, y, text);
        return;
}

bmds_rect_t ui_text_rect(int font, int32_t x, int32_t y, const char *text) {
        const art_glyph_t *any = glyph(font, 'A');
        const int32_t      left = any != NULL ? any->left : 0, top = any != NULL ? any->top : 0;
        const int32_t      h = any != NULL ? any->h : art_fonts[font].cap;
        return rect(x - left, y - top, ui_text_width(font, text) + 2 * left + 4, h);
}

void ui_round_fill(bmds_canvas_t *c, bmds_rect_t r, int32_t radius, uint32_t rgb) {
        const uint32_t pixel = bmds_canvas_pixel(c, rgb);
        const int32_t  h = (int32_t)r.height, w = (int32_t)r.width;
        if (radius * 2 > h) radius = h / 2;
        if (radius * 2 > w) radius = w / 2;
        for (int32_t row = 0; row < h; ++row) {
                /* The corners' inset on this row: a circle's. */
                int32_t       inset = 0;
                const int32_t dy = row < radius ? radius - row : row >= h - radius ? row - (h - radius - 1) : 0;
                if (dy > 0) {
                        int32_t dx = radius;
                        while (dx > 0 && dx * dx + dy * dy > radius * radius) --dx;
                        inset = radius - dx;
                }
                bmds_canvas_fill(c, r.x + inset, r.y + row, w - 2 * inset, 1, pixel);
        }
        return;
}

static bmds_rect_t inset(bmds_rect_t r, int32_t by) {
        return rect(r.x + by, r.y + by, (int32_t)r.width - 2 * by, (int32_t)r.height - 2 * by);
}

void ui_panel(bmds_canvas_t *c, bmds_rect_t r) {
        ui_round_fill(c, r, 14, 0x3a2410);
        ui_round_fill(c, inset(r, 2), 12, C_PANEL_EDGE);
        ui_round_fill(c, inset(r, 5), 10, C_PANEL);
        return;
}

void ui_line(bmds_canvas_t *c, int32_t x, int32_t y, const char *text, uint32_t rgb) {
        (void)bmds_canvas_text(c, x, y, text, bmds_canvas_pixel(c, rgb), 1);
        return;
}

void ui_button(bmds_canvas_t *c, const ui_button_t *b, int state) {
        static const uint32_t faces[4][2] = {
                {0x9a5a24, 0x6a3a14}, /* normal: wood */
                {0xc07a34, 0x8a5020}, /* hover */
                {0x5a3410, 0x7a4818}, /* pressed */
                {0x5a5048, 0x403830}, /* disabled */
        };
        const bmds_rect_t r = b->r;
        ui_round_fill(c, r, 8, 0x2a180a);
        ui_round_fill(c, inset(r, 2), 6, faces[state][1]);
        bmds_rect_t top = inset(r, 2);
        top.height = top.height / 2;
        ui_round_fill(c, top, 6, faces[state][0]);
        bmds_canvas_fill(c, top.x, top.y + (int32_t)top.height - 6, (int32_t)top.width, 6,
                         bmds_canvas_pixel(c, faces[state][0]));
        const int32_t cap = art_fonts[ART_FONT_SMALL].cap;
        const int32_t dy = state == UI_PRESSED ? 1 : 0;
        ui_text_centred(c, ART_FONT_SMALL, r.x + (int32_t)r.width / 2, r.y + ((int32_t)r.height - cap) / 2 + dy,
                        b->label);
        return;
}

int ui_button_at(const ui_button_t *b, int count, int32_t x, int32_t y) {
        for (int i = 0; i < count; ++i)
                if (contains(&b[i].r, x, y)) return i;
        return -1;
}

bmds_rect_t ui_card_source(int tile) {
        return rect((tile % ART_CARD_COLUMNS) * ART_CARD_W, (tile / ART_CARD_COLUMNS) * ART_CARD_H, ART_CARD_W,
                    ART_CARD_H);
}

void ui_card(bmds_canvas_t *c, int32_t x, int32_t y, int tile) {
        const bmds_rect_t src = ui_card_source(tile);
        bmds_canvas_blit(c, x, y, &app.cards, &src);
        return;
}

void ui_sprite(bmds_canvas_t *c, int32_t x, int32_t y, int sprite, int mirror) {
        const art_tile_t *t = &art_sprites[sprite];
        const bmds_rect_t src = {t->x, t->y, t->w, t->h};
        if (mirror)
                bmds_canvas_blit_scaled(c, x, y, -(int32_t)t->w, t->h, &app.sprites, &src);
        else
                bmds_canvas_blit(c, x, y, &app.sprites, &src);
        return;
}

/* @p c with the edge cut off that @p o covers right across (a band of rows, or of columns). */
static int cut(bmds_rect_t *c, const bmds_rect_t *o) {
        const int32_t cx1 = c->x + (int32_t)c->width, cy1 = c->y + (int32_t)c->height;
        const int32_t ox1 = o->x + (int32_t)o->width, oy1 = o->y + (int32_t)o->height;
        if (c->width == 0 || c->height == 0) return 0;
        if (o->x <= c->x && ox1 >= cx1 && o->y < cy1 && oy1 > c->y) { /* rows, right across */
                if (o->y <= c->y && oy1 >= cy1) {
                        c->width = c->height = 0;
                } else if (o->y <= c->y) {
                        c->height = (uint32_t)(cy1 - oy1);
                        c->y = oy1;
                } else if (oy1 >= cy1) {
                        c->height = (uint32_t)(o->y - c->y);
                } else {
                        return 0; /* a band through the middle: both halves still show */
                }
                return 1;
        }
        if (o->y <= c->y && oy1 >= cy1 && o->x < cx1 && ox1 > c->x) { /* columns, right down */
                if (o->x <= c->x && ox1 >= cx1) {
                        c->width = c->height = 0;
                } else if (o->x <= c->x) {
                        c->width = (uint32_t)(cx1 - ox1);
                        c->x = ox1;
                } else if (ox1 >= cx1) {
                        c->width = (uint32_t)(o->x - c->x);
                } else {
                        return 0;
                }
                return 1;
        }
        return 0;
}

bmds_rect_t ui_uncovered(bmds_rect_t c, const bmds_rect_t *cards, int count) {
        /* Again until nothing more is cut: one cut can make another possible. */
        for (int changed = 1; changed && c.width > 0;) {
                changed = 0;
                for (int i = 0; i < count && c.width > 0; ++i) {
                        const bmds_rect_t rows = rect(cards[i].x, cards[i].y + UI_CARD_RADIUS, (int32_t)cards[i].width,
                                                      (int32_t)cards[i].height - 2 * UI_CARD_RADIUS);
                        const bmds_rect_t cols =
                                rect(cards[i].x + UI_CARD_RADIUS, cards[i].y,
                                     (int32_t)cards[i].width - 2 * UI_CARD_RADIUS, (int32_t)cards[i].height);
                        changed |= cut(&c, &rows);
                        changed |= cut(&c, &cols);
                }
        }
        return c;
}

/* ---- Chips */

const int32_t ui_chip_value[UI_CHIP_KINDS] = {1, 5, 25, 100, 500, 1000};

#define PILE_MAX 14
#define CHIP_STEP 5

/* The chips (their kinds, the biggest first) making @p dollars, at most PILE_MAX. @return How many. */
static int pile(uint32_t dollars, uint8_t *kinds) {
        int n = 0;
        for (int k = UI_CHIP_KINDS - 1; k >= 0 && n < PILE_MAX; --k)
                while (dollars >= (uint32_t)ui_chip_value[k] && n < PILE_MAX) {
                        kinds[n++] = (uint8_t)k;
                        dollars -= (uint32_t)ui_chip_value[k];
                }
        return n;
}

bmds_rect_t ui_chips_rect(int32_t x, int32_t y, uint32_t dollars) {
        uint8_t   kinds[PILE_MAX];
        const int n = pile(dollars, kinds);
        if (n == 0) return rect(0, 0, 0, 0);
        const art_tile_t *t = &art_sprites[ART_CHIP_0];
        return rect(x, y - (n - 1) * CHIP_STEP, t->w, t->h + (n - 1) * CHIP_STEP);
}

void ui_chips(bmds_canvas_t *c, int32_t x, int32_t y, uint32_t dollars) {
        uint8_t   kinds[PILE_MAX];
        const int n = pile(dollars, kinds);
        for (int i = 0; i < n; ++i) ui_sprite(c, x, y - i * CHIP_STEP, ART_CHIP_0 + kinds[i], 0);
        return;
}
