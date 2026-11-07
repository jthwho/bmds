/**
 * @file      pal.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The show's palette (devplan/demo.md §7): each part's base palette (entries 0 to PAL_PARTS - 1),
 * crossfaded from what was shown when the part changes, with flashes mixed in on top (a kick, a
 * click), and the overlay's four entries above the parts'. Plain code: host-tested.
 */

#ifndef DEMO1_PAL_H
#define DEMO1_PAL_H

#include <stdint.h>

/** @brief The entries the parts own; the rest are the overlay's. */
#define PAL_PARTS 252u
#define PAL_TEXT 255u   /**< The overlay's text. */
#define PAL_DIM 254u    /**< Its quieter text. */
#define PAL_PANEL 253u  /**< Its background. */
#define PAL_ACCENT 252u /**< A bar or a mark. */

typedef struct {
                uint32_t base[256];          /* the part's colours, 0xRRGGBB */
                uint32_t from[256];          /* what was shown when a fade began */
                uint32_t shown[256];         /* the last made */
                float    fade, fade_seconds; /* the crossfade so far, and its length (0: none) */
                float    flash;              /* 0..1, decaying */
                float    flash_decay;        /* a second's fraction lost */
                uint32_t flash_rgb;
                int      changed; /* shown differs from what was last given out */
} pal_t;

/** @brief Starts with every entry black but the overlay's. */
void pal_init(pal_t *pal);

/** @brief A new base palette (@p rgb: PAL_PARTS entries), faded to over @p seconds (0: at once). */
void pal_set_base(pal_t *pal, const uint32_t *rgb, float seconds);

/** @brief Changes base entries [@p first, @p first + @p count) at once, with no fade (cycling). */
void pal_set_entries(pal_t *pal, uint32_t first, uint32_t count, const uint32_t *rgb);

/** @brief Rotates base entries [@p first, @p first + @p count) by @p steps (colour cycling). */
void pal_rotate(pal_t *pal, uint32_t first, uint32_t count, int32_t steps);

/** @brief Mixes @p rgb over everything by @p strength (0..1), losing @p per_second of it a second. */
void pal_flash(pal_t *pal, uint32_t rgb, float strength, float per_second);

/**
 * @brief Moves the fade and the flash on by @p dt seconds and makes the palette shown.
 * @return The palette (256 entries) if it changed since the last call, else NULL.
 */
const uint32_t *pal_update(pal_t *pal, float dt);

#endif // DEMO1_PAL_H
