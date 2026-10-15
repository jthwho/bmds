/**
 * @file      game_internal.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's own declarations (devplan/games.md §13).
 */

#ifndef BMDS_GAME_INTERNAL_H
#define BMDS_GAME_INTERNAL_H

#include <bmds/game.h>

/** @brief Glyphs in the font: ASCII 0x20-0x7E, then Latin-1 0xA0-0xFF. */
#define BMDS_GAME_FONT_GLYPHS 191

extern const unsigned char bmds_game_font16[BMDS_GAME_FONT_GLYPHS][16];

/** @brief Adds @p rect, cut to the canvas's clip (what drawing there can change), to its damage. */
void bmds_game_damage_in_clip(bmds_canvas_t *canvas, const bmds_rect_t *rect);

/** @brief Adds @p rect to @p list (of @p count, at most @p max): merged with what it meets. */
void bmds_game_rects_add(bmds_rect_t *list, uint32_t *count, uint32_t max, const bmds_rect_t *rect);

/** @brief The transparent RGB565 and BGR24 value of a loaded image: magenta. */
#define BMDS_GAME_KEY_RGB565 0xf81fu
#define BMDS_GAME_KEY_BGR24 0xff00ffu

/** @brief Turns BGRA32 pixels into a loaded image's (`bmds_game_load_t`). */
typedef struct bmds_game_converter_t {
                const bmds_game_load_t *how;
                uint32_t                bpp;
                uint32_t                key;      /* the transparent pixel value */
                uint32_t                last_rgb; /* L8: the last colour looked up, and its index */
                uint32_t                last_index;
                int                     have_last;
} bmds_game_converter_t;

/** @brief Readies @p converter for @p how. @return 0, or -1 (@p how can't be loaded). */
int bmds_game_converter_init(bmds_game_converter_t *converter, const bmds_game_load_t *how);

/**
 * @brief Converts @p count BGRA32 pixels at @p in to @p out (which may be @p in: written behind
 *        what is read).
 * @return The pixels made transparent.
 */
uint32_t bmds_game_convert(bmds_game_converter_t *converter, const uint8_t *in, uint8_t *out, uint32_t count);

#endif /* BMDS_GAME_INTERNAL_H */
