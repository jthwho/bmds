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

#endif /* BMDS_GAME_INTERNAL_H */
