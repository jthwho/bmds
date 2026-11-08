/**
 * @file      pal.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The show's palette (pal.h).
 */

#include <string.h>
#include <bmds/palette.h>
#include "pal.h"

static const uint32_t overlay[4] = {0x40e0ff, 0x101820, 0x8090a0, 0xf0f4f8}; /* 252..255 */

void pal_init(pal_t *pal) {
        memset(pal, 0, sizeof(*pal));
        memcpy(pal->base + PAL_PARTS, overlay, sizeof(overlay));
        memcpy(pal->shown, pal->base, sizeof(pal->shown));
        memcpy(pal->from, pal->base, sizeof(pal->from));
        pal->changed = 1;
        return;
}

void pal_set_base(pal_t *pal, const uint32_t *rgb, float seconds) {
        memcpy(pal->from, pal->shown, sizeof(pal->from));
        memcpy(pal->base, rgb, PAL_PARTS * sizeof(uint32_t));
        pal->fade_seconds = seconds > 0.0f ? seconds : 0.0f;
        pal->fade = 0.0f;
        pal->changed = 1;
        return;
}

void pal_set_entries(pal_t *pal, uint32_t first, uint32_t count, const uint32_t *rgb) {
        if (first >= PAL_PARTS) return;
        if (count > PAL_PARTS - first) count = PAL_PARTS - first;
        memcpy(pal->base + first, rgb, count * sizeof(uint32_t));
        pal->changed = 1;
        return;
}

void pal_rotate(pal_t *pal, uint32_t first, uint32_t count, int32_t steps) {
        if (first >= PAL_PARTS) return;
        if (count > PAL_PARTS - first) count = PAL_PARTS - first;
        bmds_palette_rotate(pal->base, first, count, steps);
        pal->changed = 1;
        return;
}

void pal_flash(pal_t *pal, uint32_t rgb, float strength, float per_second) {
        if (strength > pal->flash) {
                pal->flash = strength > 1.0f ? 1.0f : strength;
                pal->flash_rgb = rgb;
        }
        pal->flash_decay = per_second;
        pal->changed = 1;
        return;
}

void pal_veil(pal_t *pal, uint32_t rgb, float amount) {
        amount = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
        if (amount == pal->veil && rgb == pal->veil_rgb) return;
        pal->veil = amount;
        pal->veil_rgb = rgb;
        pal->changed = 1;
        return;
}

const uint32_t *pal_update(pal_t *pal, float dt) {
        int fading = pal->fade_seconds > 0.0f && pal->fade < pal->fade_seconds;
        if (fading) {
                pal->fade += dt;
                pal->changed = 1;
        }
        if (pal->flash > 0.0f) {
                pal->flash -= pal->flash_decay * dt;
                if (pal->flash < 0.0f) pal->flash = 0.0f;
                pal->changed = 1;
        }
        if (!pal->changed) return NULL;
        /* The parts' entries: the fade, then the flash over it; the overlay's as they are. */
        const uint32_t amount =
                fading && pal->fade < pal->fade_seconds ? (uint32_t)(256.0f * pal->fade / pal->fade_seconds) : 256u;
        if (amount >= 256u)
                memcpy(pal->shown, pal->base, PAL_PARTS * sizeof(uint32_t));
        else
                bmds_palette_mix(pal->shown, pal->from, pal->base, PAL_PARTS, amount);
        if (pal->flash > 0.0f)
                bmds_palette_mix_rgb(pal->shown, pal->shown, PAL_PARTS, pal->flash_rgb,
                                     (uint32_t)(pal->flash * 256.0f));
        if (pal->veil > 0.0f)
                bmds_palette_mix_rgb(pal->shown, pal->shown, PAL_PARTS, pal->veil_rgb, (uint32_t)(pal->veil * 256.0f));
        memcpy(pal->shown + PAL_PARTS, pal->base + PAL_PARTS, (256 - PAL_PARTS) * sizeof(uint32_t));
        pal->changed = 0;
        return pal->shown;
}
