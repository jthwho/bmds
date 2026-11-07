/**
 * @file      plasma.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 2 (devplan/demo.md §3): the classic plasma, its colour all palette. Each pixel is three
 * waves summed: one across (a table per frame), one down (a value per row), and one turning
 * slowly across the screen (a phase stepped along each row in fixed point); the sum, 0..765,
 * picks one of PAL_PARTS colours through a table, and the palette turns a step a frame. The
 * section's cue chooses the palette's mood (a loop of every colour; deep sea at the break), the
 * kick turns it faster, a clap flashes it. Moving the pointer stretches the turning wave; a click
 * flashes it.
 */

#include <math.h>
#include <string.h>
#include <bmds/palette.h>
#include "../demo.h"

#define PI 3.14159265f

static struct {
                uint8_t  sine[256];              /* 0..255 */
                uint8_t  index[768];             /* a sum of three to a colour */
                uint8_t  across[DEMO_WIDTH / 2]; /* this frame's wave across, a pair of pixels each */
                uint32_t palette[PAL_PARTS], deep[PAL_PARTS];
                /* This frame's, for the rows' bands. */
                bmds_canvas_t *canvas;
                int32_t        step_x, step_y, drift;
                uint32_t       down_shift;
} s;

static int init(demo_t *demo) {
        (void)demo;
        for (int i = 0; i < 256; ++i) s.sine[i] = (uint8_t)(127.5f + 127.5f * sinf(2.0f * PI * (float)i / 256.0f));
        for (int i = 0; i < 768; ++i) s.index[i] = (uint8_t)((uint32_t)i * PAL_PARTS / 768u);
        /* A loop of colours: each channel its own sine, so the ramp never ends where it repeats. */
        for (uint32_t i = 0; i < PAL_PARTS; ++i) {
                const float    a = 2.0f * PI * (float)i / PAL_PARTS;
                const uint32_t r = (uint32_t)(127.5f + 127.5f * sinf(a));
                const uint32_t g = (uint32_t)(127.5f + 127.5f * sinf(a * 2.0f + 2.1f));
                const uint32_t b = (uint32_t)(127.5f + 127.5f * sinf(a * 3.0f + 4.2f));
                s.palette[i] = r << 16 | g << 8 | b;
                /* The break's: blues and greens, dark to bright and back twice round. */
                const float v = 0.5f - 0.5f * cosf(2.0f * a);
                s.deep[i] = bmds_palette_hsv(0.5f + 0.12f * sinf(a), 0.8f, 0.15f + 0.8f * v);
        }
        return 0;
}

static void enter(demo_t *demo) {
        const sync_t *y = &demo->sync;
        const int     deep = y->known && y->pos.cue != NULL && strcmp(y->pos.cue, "break") == 0;
        pal_set_base(&demo->pal, deep ? s.deep : s.palette, 1.0f);
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER && (e->data[2] & 1u) != 0 && !demo->button)
                pal_flash(&demo->pal, 0xffffff, 0.8f, 2.0f);
        return;
}

/* Rows [first, end) of this frame. */
static void rows(void *arg, int32_t first, int32_t end) {
        (void)arg;
        const bmds_canvas_t *c = s.canvas;
        for (int32_t y = first; y < end; ++y) {
                const uint32_t down = s.sine[(uint32_t)(y * 5 / 4 + s.down_shift) & 255u];
                uint32_t      *out = (uint32_t *)(void *)(c->pixels + (size_t)y * c->stride);
                int32_t        phase = s.step_y * y + s.drift;
                /* Two values, four pixels, one store. */
                for (int32_t x = 0; x < DEMO_WIDTH / 2; x += 2) {
                        const uint32_t a = s.index[s.across[x] + down + s.sine[(uint32_t)(phase >> 16) & 255u]];
                        phase += s.step_x;
                        const uint32_t b = s.index[s.across[x + 1] + down + s.sine[(uint32_t)(phase >> 16) & 255u]];
                        phase += s.step_x;
                        *out++ = a * 0x0101u | b * 0x01010000u;
                }
        }
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        (void)dt;
        /* Across: a wave a frame, a value for each pair of pixels (the plasma is smooth). */
        const uint32_t shift = (uint32_t)(t * 60.0f);
        for (int32_t x = 0; x < DEMO_WIDTH / 2; ++x) s.across[x] = s.sine[(uint32_t)(x * 6 / 5 + shift) & 255u];
        /* The turning wave: its direction and spacing (a pair of pixels a step); the pointer stretches it. */
        const float angle = t * 0.25f;
        const float spacing = demo->pointer_x >= 0 ? 0.3f + 1.2f * (float)demo->pointer_x / DEMO_WIDTH : 0.8f;
        s.step_x = (int32_t)(dcos(angle) * spacing * 2.0f * 65536.0f);
        s.step_y = (int32_t)(dsin(angle) * spacing * 65536.0f);
        s.drift = (int32_t)(t * 40.0f * 65536.0f);
        s.down_shift = (uint32_t)(t * 35.0f);
        s.canvas = c;
        rows(NULL, 0, DEMO_HEIGHT);
        const sync_t *y = &demo->sync;
        pal_rotate(&demo->pal, 0, PAL_PARTS, 2 + (int32_t)(8.0f * y->kick));
        if (y->claps != 0) pal_flash(&demo->pal, 0xffffff, 0.3f, 4.0f);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_plasma = {"Plasma", init, enter, frame, event};
