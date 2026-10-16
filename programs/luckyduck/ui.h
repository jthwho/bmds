/**
 * @file      ui.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's drawing helpers (devplan/luckyduck.md §3, §9): the headline letters, rounded
 * panels, buttons (each with its key), and the cards' tiles.
 */

#ifndef LUCKYDUCK_UI_H
#define LUCKYDUCK_UI_H

#include "luckyduck.h"

/** @brief Draws @p text in headline font @p font from (@p x, @p y) (its capitals' top-left). @return Its width. */
int32_t ui_text(bmds_canvas_t *c, int font, int32_t x, int32_t y, const char *text);

/** @brief @p text's width in font @p font. */
int32_t ui_text_width(int font, const char *text);

/** @brief Draws @p text centred across @p cx. */
void ui_text_centred(bmds_canvas_t *c, int font, int32_t cx, int32_t y, const char *text);

/** @brief The rectangle @p text covers drawn at (@p x, @p y) in font @p font (its shadow and outline in). */
bmds_rect_t ui_text_rect(int font, int32_t x, int32_t y, const char *text);

/** @brief Fills @p r with its corners rounded by @p radius. */
void ui_round_fill(bmds_canvas_t *c, bmds_rect_t r, int32_t radius, uint32_t rgb);

/** @brief A panel: a dark rounded box in a gold edge. */
void ui_panel(bmds_canvas_t *c, bmds_rect_t r);

/** @brief A line of `libbmdsgame`'s 8×16 text at (@p x, @p y). */
void ui_line(bmds_canvas_t *c, int32_t x, int32_t y, const char *text, uint32_t rgb);

/** @brief A button's states. */
enum {
        UI_NORMAL,
        UI_HOVER,
        UI_PRESSED,
        UI_DISABLED
};

/** @brief A button: where, what it says, its key. */
typedef struct {
                bmds_rect_t r;
                const char *label;
                uint32_t    key; /**< Its HID key, or 0. */
} ui_button_t;

/** @brief Draws button @p b in @p state. */
void ui_button(bmds_canvas_t *c, const ui_button_t *b, int state);

/** @brief The button of @p count under (@p x, @p y), or -1. */
int ui_button_at(const ui_button_t *b, int count, int32_t x, int32_t y);

/** @brief Card tile @p tile (suit × 13 + rank, or an `ART_CARD_...`) at (@p x, @p y). */
void ui_card(bmds_canvas_t *c, int32_t x, int32_t y, int tile);

/** @brief Card tile @p tile's place in the sheet. */
bmds_rect_t ui_card_source(int tile);

/** @brief A card's corners' radius: inside it, a card is opaque right across and right down. */
#define UI_CARD_RADIUS 7

/**
 * @brief What of @p c still shows under @p count cards (their rectangles) drawn over it: its
 *        edges cut where a card covers them right across or right down. Drawing only that skips
 *        the cards under a column's (devplan/luckyduck.md §10: PSRAM is what drawing costs).
 */
bmds_rect_t ui_uncovered(bmds_rect_t c, const bmds_rect_t *cards, int count);

/** @brief Chips' values, `ART_CHIP_0` up. */
#define UI_CHIP_KINDS 6
extern const int32_t ui_chip_value[UI_CHIP_KINDS];

/** @brief A pile of chips worth @p dollars (the fewest, the biggest at the bottom; at most 14
 *         drawn), its bottom chip's top-left at (@p x, @p y): each chip 5 pixels above the last. */
void ui_chips(bmds_canvas_t *c, int32_t x, int32_t y, uint32_t dollars);

/** @brief The rectangle ui_chips() covers. */
bmds_rect_t ui_chips_rect(int32_t x, int32_t y, uint32_t dollars);

/** @brief Sprite @p sprite with its top-left at (@p x, @p y); @p mirror: facing the other way. */
void ui_sprite(bmds_canvas_t *c, int32_t x, int32_t y, int sprite, int mirror);

#endif /* LUCKYDUCK_UI_H */
