/**
 * @file      palette.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's palettes (devplan/3d.md §8): 256 colours of an L8 surface, 0xRRGGBB each, made
 * into ramps (what libbmds3d shades along), mixed (fades, flashes) and rotated (colour cycling).
 * Arrays of the caller's; bmds_game_palette() shows one from the next frame on. No allocation,
 * no system calls but bmds_game_palette()'s.
 */

#ifndef BMDS_PALETTE_H
#define BMDS_PALETTE_H

#include <stdint.h>
#include <bmds/game.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Writes @p count entries from @p first: @p from to @p to (0xRRGGBB, both included),
 *        evenly between in RGB. One entry: @p to.
 */
void bmds_palette_ramp(uint32_t *palette, uint32_t first, uint32_t count, uint32_t from, uint32_t to);

/** @brief As bmds_palette_ramp(), through @p mid half way (dark, a colour, white: a lit surface's ramp). */
void bmds_palette_ramp3(uint32_t *palette, uint32_t first, uint32_t count, uint32_t from, uint32_t mid, uint32_t to);

/**
 * @brief Writes @p count entries of @p out as @p a mixed towards @p b by @p amount (0: @p a, 256:
 *        @p b): a fade between palettes, or towards one colour with @p b all that colour. @p out
 *        may be @p a or @p b.
 */
void bmds_palette_mix(uint32_t *out, const uint32_t *a, const uint32_t *b, uint32_t count, uint32_t amount);

/** @brief As bmds_palette_mix(), every entry towards the one colour @p rgb (a flash, a fade to black). */
void bmds_palette_mix_rgb(uint32_t *out, const uint32_t *a, uint32_t count, uint32_t rgb, uint32_t amount);

/**
 * @brief Rotates entries [@p first, @p first + @p count) by @p steps: positive moves each colour to
 *        a higher index (the last wraps to the first). Colour cycling.
 */
void bmds_palette_rotate(uint32_t *palette, uint32_t first, uint32_t count, int32_t steps);

/** @brief Returns the colour of hue @p h (0..1, wrapping), saturation @p s and value @p v (0..1). */
uint32_t bmds_palette_hsv(float h, float s, float v);

/**
 * @brief Shows @p count entries of @p rgb from @p first on @p game's L8 surface, from the next
 *        frame presented on (the whole frame is converted again).
 * @return 0, or `-BMDS_EINVAL` (out of range), or the surface call's error (a surface not L8).
 */
int bmds_game_palette(bmds_game_t *game, uint32_t first, uint32_t count, const uint32_t *rgb);

#ifdef __cplusplus
}
#endif

#endif // BMDS_PALETTE_H
