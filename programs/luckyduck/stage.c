/**
 * @file      stage.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's stage (devplan/luckyduck.md §4).
 */

#include <string.h>
#include "stage.h"
#include "ui.h"

void stage_begin(stage_t *s) {
        memset(s->now, 0, sizeof(s->now));
        return;
}

stage_item_t *stage_put(stage_t *s, uint32_t key, uint8_t kind, uint8_t z, int32_t x, int32_t y) {
        stage_item_t *it = &s->now[key % STAGE_KEYS];
        memset(it, 0, sizeof(*it));
        it->kind = kind;
        it->z = z;
        it->x = (int16_t)x;
        it->y = (int16_t)y;
        return it;
}

static void tile_size(const stage_item_t *it, int32_t *w, int32_t *h) {
        if (it->kind == STAGE_CARD || (it->kind == STAGE_SHADE && (it->flags & STAGE_ON_CARD) != 0)) {
                *w = ART_CARD_W;
                *h = ART_CARD_H;
        } else {
                *w = art_sprites[it->tile].w;
                *h = art_sprites[it->tile].h;
        }
        return;
}

bmds_rect_t stage_bounds(const stage_item_t *it) {
        int32_t w, h;
        switch (it->kind) {
                case STAGE_SPRITE:
                case STAGE_CARD:
                case STAGE_SHADE:
                        tile_size(it, &w, &h);
                        if (it->turn != 0 || it->scale != 0) {
                                /* Turned about its middle: the circle it can sweep, scaled. */
                                const int32_t scale = it->scale != 0 ? it->scale : 256;
                                int32_t       r = 1;
                                while (r * r * 4 < w * w + h * h) ++r;
                                r = r * scale / 256 + 2;
                                return rect(it->x + w / 2 - r, it->y + h / 2 - r, 2 * r, 2 * r);
                        }
                        if (it->w != 0) w = it->w;
                        if (it->h != 0) h = it->h;
                        return rect(it->x, it->y, w, h);
                case STAGE_TEXT: {
                        const int32_t tw = (it->flags & STAGE_CENTRE) != 0 ? ui_text_ink_width(it->font, it->text) : 0;
                        const int32_t x = (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x;
                        return ui_text_rect(it->font, x, it->y, it->text);
                }
                case STAGE_LABEL: {
                        const int32_t font = bmds_game_font_scaled(it->font != 0 ? it->font : 1);
                        const int32_t tw = bmds_game_text_width(font, it->text);
                        const int32_t x = (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x;
                        return rect(x, it->y, tw, bmds_game_line_height(font));
                }
                case STAGE_BUTTON:
                case STAGE_PANEL:
                case STAGE_BLEND: return rect(it->x, it->y, it->w, it->h);
                case STAGE_CHIPS: return ui_chips_rect(it->x, it->y, it->rgb);
                default: return rect(0, 0, 0, 0);
        }
}

void stage_dirty(stage_t *s, bmds_rect_t r) {
        dirty_add(s->dirty, &s->dirty_count, STAGE_DIRTY, r);
        return;
}

void stage_draw_item(bmds_canvas_t *c, const stage_item_t *it) {
        int32_t     w, h;
        bmds_rect_t src;
        switch (it->kind) {
                case STAGE_SPRITE:
                case STAGE_CARD: {
                        const bmds_game_image_t *image = it->kind == STAGE_CARD ? &app.cards : &app.sprites;
                        tile_size(it, &w, &h);
                        if (it->kind == STAGE_CARD) {
                                src = ui_card_source(it->tile);
                        } else {
                                const art_tile_t *t = &art_sprites[it->tile];
                                src = rect(t->x, t->y, t->w, t->h);
                        }
                        if (it->turn != 0 || it->scale != 0)
                                bmds_canvas_blit_rotated(c, it->x + w / 2, it->y + h / 2, it->turn,
                                                         it->scale != 0 ? it->scale : BMDS_GAME_SCALE, image, &src);
                        else if ((it->w != 0 && it->w != w) || (it->h != 0 && it->h != h) ||
                                 (it->flags & STAGE_MIRROR) != 0) /* its own size: the plain blit, rows copied */
                                bmds_canvas_blit_scaled(c, it->x, it->y,
                                                        (it->w != 0 ? it->w : w) *
                                                                ((it->flags & STAGE_MIRROR) ? -1 : 1),
                                                        it->h != 0 ? it->h : h, image, &src);
                        else
                                bmds_canvas_blit(c, it->x, it->y, image, &src);
                        break;
                }
                case STAGE_SHADE:
                        if ((it->flags & STAGE_ON_CARD) != 0) {
                                src = ui_card_source(it->tile);
                                bmds_canvas_shade_image(c, it->x, it->y, &app.cards, &src, it->alpha);
                        } else {
                                const art_tile_t *t = &art_sprites[it->tile];
                                src = rect(t->x, t->y, t->w, t->h);
                                bmds_canvas_shade_image(c, it->x, it->y, &app.sprites, &src, it->alpha);
                        }
                        break;
                case STAGE_TEXT: {
                        const int32_t tw = (it->flags & STAGE_CENTRE) != 0 ? ui_text_ink_width(it->font, it->text) : 0;
                        (void)ui_text(c, it->font, (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x, it->y,
                                      it->text);
                        break;
                }
                case STAGE_LABEL: {
                        const int32_t font = bmds_game_font_scaled(it->font != 0 ? it->font : 1);
                        const int32_t tw = bmds_game_text_width(font, it->text);
                        (void)bmds_canvas_text(c, font, (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x, it->y,
                                               it->text, bmds_canvas_pixel(c, it->rgb));
                        break;
                }
                case STAGE_BUTTON: {
                        const ui_button_t b = {rect(it->x, it->y, it->w, it->h), it->text, 0};
                        ui_button(c, &b, it->font);
                        break;
                }
                case STAGE_PANEL: ui_panel(c, rect(it->x, it->y, it->w, it->h)); break;
                case STAGE_BLEND: bmds_canvas_fill_blend(c, it->x, it->y, it->w, it->h, it->rgb, it->alpha); break;
                case STAGE_CHIPS: ui_chips(c, it->x, it->y, it->rgb); break;
                default: break;
        }
        return;
}

/* The keys of @p items in drawing order (z, then key). @return How many. */
static uint32_t ordered(const stage_item_t *items, uint16_t *order) {
        uint32_t n = 0;
        for (uint32_t key = 0; key < STAGE_KEYS; ++key) {
                if (items[key].kind == STAGE_NONE) continue;
                uint32_t i = n++;
                while (i > 0 && items[order[i - 1]].z > items[key].z) {
                        order[i] = order[i - 1];
                        --i;
                }
                order[i] = (uint16_t)key;
        }
        return n;
}

/* Takes @p cut out of the @p count rectangles in @p pieces (at most @p max): each piece it meets
   becomes the up to four bands left around it. @return The new count; with no room, @p cut is
   left in (the felt then restored under it too: wasted, never wrong). */
static uint32_t cut_out(bmds_rect_t *pieces, uint32_t count, uint32_t max, bmds_rect_t cut) {
        bmds_rect_t out[48];
        uint32_t    n = 0;
        for (uint32_t i = 0; i < count; ++i) {
                const bmds_rect_t p = pieces[i], k = intersection(p, cut);
                if (k.width == 0 || k.height == 0) {
                        if (n == 48) return count;
                        out[n++] = p;
                        continue;
                }
                const int32_t     px1 = p.x + (int32_t)p.width, py1 = p.y + (int32_t)p.height;
                const int32_t     kx1 = k.x + (int32_t)k.width, ky1 = k.y + (int32_t)k.height;
                const bmds_rect_t bands[4] = {rect(p.x, p.y, (int32_t)p.width, k.y - p.y),   /* above */
                                              rect(p.x, ky1, (int32_t)p.width, py1 - ky1),   /* below */
                                              rect(p.x, k.y, k.x - p.x, (int32_t)k.height),  /* left */
                                              rect(kx1, k.y, px1 - kx1, (int32_t)k.height)}; /* right */
                for (int b = 0; b < 4; ++b) {
                        if (bands[b].width == 0 || bands[b].height == 0) continue;
                        if (n == 48) return count;
                        out[n++] = bands[b];
                }
        }
        if (n > max) return count;
        memcpy(pieces, out, n * sizeof(out[0]));
        return n;
}

/* The pieces of the felt inside @p clip under no card's opaque middle (a card is opaque but for
   its corners; turned ones aren't counted), added to @p out (of @p count, at most @p max):
   what a card is drawn over needn't be restored first (devplan/luckyduck.md §23). @return Whether
   they fitted. */
static int felt_pieces(const stage_item_t *items, const uint16_t *order, uint32_t n, bmds_rect_t clip, bmds_rect_t *out,
                       uint32_t *count, uint32_t max) {
        bmds_rect_t pieces[16] = {clip};
        uint32_t    k = 1;
        for (uint32_t i = 0; i < n && k > 0; ++i) {
                const stage_item_t *it = &items[order[i]];
                if (it->kind != STAGE_CARD || it->turn != 0 || it->scale != 0) continue;
                if (it->tile >= ART_CARD_EMPTY && it->tile != ART_CARD_JOKER) continue; /* the places are hollow */
                const bmds_rect_t b = stage_bounds(it);
                if (b.width <= 2 * UI_CARD_RADIUS || b.height <= 2 * UI_CARD_RADIUS || !meets(&b, &clip)) continue;
                k = cut_out(pieces, k, 16,
                            rect(b.x + UI_CARD_RADIUS, b.y + UI_CARD_RADIUS, (int32_t)b.width - 2 * UI_CARD_RADIUS,
                                 (int32_t)b.height - 2 * UI_CARD_RADIUS));
        }
        if (*count + k > max) return 0;
        memcpy(out + *count, pieces, k * sizeof(pieces[0]));
        *count += k;
        return 1;
}

static void restore_uncovered(const stage_item_t *items, const uint16_t *order, uint32_t n, bmds_canvas_t *c,
                              bmds_rect_t clip) {
        bmds_rect_t pieces[16];
        uint32_t    count = 0;
        if (!felt_pieces(items, order, n, clip, pieces, &count, 16)) {
                pieces[0] = clip;
                count = 1;
        }
        (void)c;
        bmds_game_dma_copy(app.game, FELT_IMAGE, pieces, count);
        return;
}

/* Draws the items inside @p r from the felt up (@p felt: the felt restored first; else already). */
static void redraw_items(const stage_item_t *items, const uint16_t *order, uint32_t n, bmds_canvas_t *c, bmds_rect_t r,
                         int felt) {
        bmds_canvas_set_clip(c, &r);
        const bmds_rect_t clip = c->clip;
        if (clip.width == 0 || clip.height == 0) {
                bmds_canvas_set_clip(c, NULL);
                return;
        }
        bmds_game_repainted(app.game, &clip); /* every pixel of it, from the felt up */
        if (felt) restore_uncovered(items, order, n, c, clip);
        for (uint32_t i = 0; i < n; ++i) {
                const stage_item_t *it = &items[order[i]];
                const bmds_rect_t   b = stage_bounds(it);
                if (meets(&b, &clip)) stage_draw_item(c, it);
        }
        bmds_canvas_set_clip(c, NULL);
        return;
}

static void redraw(const stage_item_t *items, const uint16_t *order, uint32_t n, bmds_canvas_t *c, bmds_rect_t r) {
        redraw_items(items, order, n, c, r, 1);
        return;
}

void stage_redraw(const stage_t *s, bmds_canvas_t *c, bmds_rect_t r) {
        uint16_t       order[STAGE_KEYS];
        const uint32_t n = ordered(s->now, order);
        redraw(s->now, order, n, c, r);
        return;
}

void stage_draw(stage_t *s, bmds_canvas_t *c) {
        for (uint32_t key = 0; key < STAGE_KEYS; ++key) {
                const stage_item_t *a = &s->shown[key], *b = &s->now[key];
                if (memcmp(a, b, sizeof(*a)) == 0) continue;
                if (a->kind != STAGE_NONE) stage_dirty(s, stage_bounds(a));
                if (b->kind != STAGE_NONE) stage_dirty(s, stage_bounds(b));
        }
        if (s->dirty_count > 0) {
                uint16_t       order[STAGE_KEYS];
                const uint32_t n = ordered(s->now, order);
                /* The felt of every rectangle in one 2D-DMA job, then their items: only when no two
                   overlap (a blend drawn twice over one would darken: luckyduck.md §24). */
                int disjoint = 1;
                for (uint32_t i = 0; i < s->dirty_count && disjoint; ++i)
                        for (uint32_t k = i + 1; k < s->dirty_count && disjoint; ++k)
                                disjoint = !meets(&s->dirty[i], &s->dirty[k]);
                static bmds_rect_t felt[STAGE_DIRTY * 16];
                uint32_t           pieces = 0;
                const bmds_rect_t  all = rect(0, 0, c->width, c->height);
                for (uint32_t i = 0; i < s->dirty_count && disjoint; ++i) {
                        const bmds_rect_t r = intersection(s->dirty[i], all);
                        if (r.width == 0 || r.height == 0) continue;
                        bmds_game_repainted(app.game, &r);
                        if (!felt_pieces(s->now, order, n, r, felt, &pieces, STAGE_DIRTY * 16)) disjoint = 0;
                }
                if (disjoint) {
                        bmds_game_dma_copy(app.game, FELT_IMAGE, felt, pieces);
                        for (uint32_t i = 0; i < s->dirty_count; ++i) redraw_items(s->now, order, n, c, s->dirty[i], 0);
                } else {
                        for (uint32_t i = 0; i < s->dirty_count; ++i) redraw(s->now, order, n, c, s->dirty[i]);
                }
                s->dirty_count = 0;
        }
        memcpy(s->shown, s->now, sizeof(s->now));
        return;
}
