/**
 * @file      palette.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's palettes (bmds/palette.h, devplan/3d.md §8).
 */

#include <math.h>
#include <bmds/palette.h>

static uint32_t channel(uint32_t rgb, int shift) {
        return rgb >> shift & 0xffu;
}

/* @p a towards @p b by @p num / @p den, rounded. */
static uint32_t lerp(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
        uint32_t out = 0;
        for (int shift = 0; shift < 24; shift += 8) {
                const int32_t ca = (int32_t)channel(a, shift), cb = (int32_t)channel(b, shift);
                const int32_t c =
                        ca + ((cb - ca) * (int32_t)num + (int32_t)den / 2 * (cb >= ca ? 1 : -1)) / (int32_t)den;
                out |= (uint32_t)c << shift;
        }
        return out;
}

void bmds_palette_ramp(uint32_t *palette, uint32_t first, uint32_t count, uint32_t from, uint32_t to) {
        if (count == 0 || first >= 256) return;
        if (count > 256 - first) count = 256 - first;
        if (count == 1) {
                palette[first] = to & 0xffffffu;
                return;
        }
        for (uint32_t i = 0; i < count; ++i) palette[first + i] = lerp(from, to, i, count - 1);
        return;
}

void bmds_palette_ramp3(uint32_t *palette, uint32_t first, uint32_t count, uint32_t from, uint32_t mid, uint32_t to) {
        if (count < 3) {
                bmds_palette_ramp(palette, first, count, from, to);
                return;
        }
        const uint32_t half = count / 2; /* entries from..mid; mid..to shares mid */
        bmds_palette_ramp(palette, first, half + 1, from, mid);
        bmds_palette_ramp(palette, first + half, count - half, mid, to);
        return;
}

void bmds_palette_mix(uint32_t *out, const uint32_t *a, const uint32_t *b, uint32_t count, uint32_t amount) {
        if (amount > 256) amount = 256;
        for (uint32_t i = 0; i < count; ++i) out[i] = lerp(a[i], b[i], amount, 256);
        return;
}

void bmds_palette_mix_rgb(uint32_t *out, const uint32_t *a, uint32_t count, uint32_t rgb, uint32_t amount) {
        if (amount > 256) amount = 256;
        for (uint32_t i = 0; i < count; ++i) out[i] = lerp(a[i], rgb, amount, 256);
        return;
}

void bmds_palette_rotate(uint32_t *palette, uint32_t first, uint32_t count, int32_t steps) {
        if (count < 2 || first >= 256) return;
        if (count > 256 - first) count = 256 - first;
        int32_t k = steps % (int32_t)count;
        if (k < 0) k += (int32_t)count;
        if (k == 0) return;
        /* Three reversals: rotates right by k in place. */
        uint32_t *p = palette + first;
        for (int pass = 0; pass < 3; ++pass) {
                uint32_t lo = pass == 1 ? 0 : pass == 2 ? (uint32_t)k : 0;
                uint32_t hi = pass == 0 ? count - 1 : pass == 1 ? (uint32_t)k - 1 : count - 1;
                for (; lo < hi; ++lo, --hi) {
                        const uint32_t t = p[lo];
                        p[lo] = p[hi];
                        p[hi] = t;
                }
        }
        return;
}

uint32_t bmds_palette_hsv(float h, float s, float v) {
        h -= floorf(h);
        s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        const float sector = h * 6.0f;
        const int   i = (int)sector % 6;
        const float f = sector - floorf(sector);
        const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
        float       r, g, b;
        switch (i) {
                case 0: r = v, g = t, b = p; break;
                case 1: r = q, g = v, b = p; break;
                case 2: r = p, g = v, b = t; break;
                case 3: r = p, g = q, b = v; break;
                case 4: r = t, g = p, b = v; break;
                default: r = v, g = p, b = q; break;
        }
        return (uint32_t)(r * 255.0f + 0.5f) << 16 | (uint32_t)(g * 255.0f + 0.5f) << 8 | (uint32_t)(b * 255.0f + 0.5f);
}
