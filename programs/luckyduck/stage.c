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
                        const int32_t tw = ui_text_width(it->font, it->text);
                        const int32_t x = (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x;
                        return ui_text_rect(it->font, x, it->y, it->text);
                }
                case STAGE_LABEL: {
                        const int32_t scale = it->font != 0 ? it->font : 1;
                        const int32_t tw = bmds_game_text_width(it->text, scale);
                        const int32_t x = (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x;
                        return rect(x, it->y, tw, BMDS_GAME_GLYPH_HEIGHT * scale);
                }
                case STAGE_BUTTON:
                case STAGE_PANEL:
                case STAGE_BLEND: return rect(it->x, it->y, it->w, it->h);
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
                        else if (it->w != 0 || it->h != 0 || (it->flags & STAGE_MIRROR) != 0)
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
                        const int32_t tw = ui_text_width(it->font, it->text);
                        (void)ui_text(c, it->font, (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x, it->y,
                                      it->text);
                        break;
                }
                case STAGE_LABEL: {
                        const int32_t scale = it->font != 0 ? it->font : 1;
                        const int32_t tw = bmds_game_text_width(it->text, scale);
                        (void)bmds_canvas_text(c, (it->flags & STAGE_CENTRE) != 0 ? it->x - tw / 2 : it->x, it->y,
                                               it->text, bmds_canvas_pixel(c, it->rgb), scale);
                        break;
                }
                case STAGE_BUTTON: {
                        const ui_button_t b = {rect(it->x, it->y, it->w, it->h), it->text, 0};
                        ui_button(c, &b, it->font);
                        break;
                }
                case STAGE_PANEL: ui_panel(c, rect(it->x, it->y, it->w, it->h)); break;
                case STAGE_BLEND: bmds_canvas_fill_blend(c, it->x, it->y, it->w, it->h, it->rgb, it->alpha); break;
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

static void redraw(const stage_item_t *items, const uint16_t *order, uint32_t n, bmds_canvas_t *c, bmds_rect_t r) {
        bmds_canvas_set_clip(c, &r);
        const bmds_rect_t clip = c->clip;
        if (clip.width == 0 || clip.height == 0) {
                bmds_canvas_set_clip(c, NULL);
                return;
        }
        felt_restore(c, clip);
        for (uint32_t i = 0; i < n; ++i) {
                const stage_item_t *it = &items[order[i]];
                const bmds_rect_t   b = stage_bounds(it);
                if (meets(&b, &clip)) stage_draw_item(c, it);
        }
        bmds_canvas_set_clip(c, NULL);
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
                for (uint32_t i = 0; i < s->dirty_count; ++i) redraw(s->now, order, n, c, s->dirty[i]);
                s->dirty_count = 0;
        }
        memcpy(s->shown, s->now, sizeof(s->now));
        return;
}
