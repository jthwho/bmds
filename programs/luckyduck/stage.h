/**
 * @file      stage.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's stage (devplan/luckyduck.md §4): a scene lays out every frame as items by key
 * (sprites, cards, text, buttons, panels, shadows); the stage compares them with the last frame's
 * and redraws only the rectangles where something differs, from the felt up, in order.
 */

#ifndef LUCKYDUCK_STAGE_H
#define LUCKYDUCK_STAGE_H

#include "luckyduck.h"

#define STAGE_KEYS 256
#define STAGE_DIRTY 32

/** @brief `stage_item_t::kind`. */
enum {
        STAGE_NONE,
        STAGE_SPRITE, /**< `tile` of sprites.png. */
        STAGE_CARD,   /**< `tile` of cards.png. */
        STAGE_SHADE,  /**< The shape of sprite `tile` (or card, `STAGE_ON_CARD`) darkened to `alpha`/256. */
        STAGE_TEXT,   /**< `text` in headline font `font`. */
        STAGE_LABEL,  /**< `text` in 8×16 at scale `font` (0: 1), colour `rgb`. */
        STAGE_BUTTON, /**< A button: `w`×`h`, `text` its label, `font` its state (`UI_...`). */
        STAGE_PANEL,  /**< A panel `w`×`h`. */
        STAGE_BLEND,  /**< `rgb` over `w`×`h`, `alpha`/256 of it. */
        STAGE_CHIPS   /**< A pile worth `rgb` dollars, its bottom chip's top-left at (`x`, `y`). */
};

/** @brief `stage_item_t::flags`. */
#define STAGE_MIRROR 1u  /**< Facing the other way. */
#define STAGE_CENTRE 2u  /**< Text: `x` is its middle. */
#define STAGE_ON_CARD 4u /**< A shade: of a card's shape. */

/** @brief An item: compared whole with the last frame's (so zeroed before it is filled). */
typedef struct {
                uint8_t  kind;
                uint8_t  z; /**< Drawn in order of z, then key. */
                uint8_t  font;
                uint8_t  flags;
                uint16_t tile;
                uint16_t turn;  /**< Of `BMDS_GAME_TURN`: turned about its middle. */
                int16_t  x, y;  /**< Its top-left. */
                int16_t  w, h;  /**< A sprite's or card's drawn size (0: its own); a box's. */
                uint16_t scale; /**< With `turn`, of 256 (0: 256). */
                uint8_t  alpha;
                uint8_t  pad;
                uint32_t rgb;
                char     text[36];
} stage_item_t;

/** @brief A stage: this frame's items, the last frame's, what to redraw. */
typedef struct {
                stage_item_t now[STAGE_KEYS];
                stage_item_t shown[STAGE_KEYS];
                bmds_rect_t  dirty[STAGE_DIRTY];
                uint32_t     dirty_count;
} stage_t;

/** @brief Starts a frame's layout: no items. */
void stage_begin(stage_t *s);

/** @brief Item @p key, zeroed, to fill in. */
stage_item_t *stage_put(stage_t *s, uint32_t key, uint8_t kind, uint8_t z, int32_t x, int32_t y);

/** @brief The rectangle item @p it covers. */
bmds_rect_t stage_bounds(const stage_item_t *it);

/** @brief Asks for @p r to be drawn again next time. */
void stage_dirty(stage_t *s, bmds_rect_t r);

/** @brief Draws what changed since the last frame (and what was asked for), and keeps this one. */
void stage_draw(stage_t *s, bmds_canvas_t *c);

/** @brief Draws this frame's items inside @p r, from the felt up. */
void stage_redraw(const stage_t *s, bmds_canvas_t *c, bmds_rect_t r);

/** @brief Draws one item (clipped as @p c is). */
void stage_draw_item(bmds_canvas_t *c, const stage_item_t *it);

#endif /* LUCKYDUCK_STAGE_H */
