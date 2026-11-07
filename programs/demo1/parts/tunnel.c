/**
 * @file      tunnel.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 4 (devplan/demo.md §3): the textured tunnel, by lookup tables. Each cell of a half-size
 * table holds where it looks into the tunnel: the angle round it (u) and the depth along it (v;
 * its shade, how far away, a table of v); a frame is one lookup into a 256×64 texture a cell,
 * moved on in u (the turn) and v (the flight), written as 2×2 pixels. The table is larger than the
 * screen, and the window onto it moves, so the tunnel's end swings about (the pointer steers it).
 * The texture is 32 values whose colours are seven rows of the palette, near to far, each row
 * cycled. On the beat (sync_t): the kick pushes the flight on, turns the colours and flashes blue;
 * the clap twists.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/palette.h>
#include "../demo.h"

#define CELLS_X (DEMO_WIDTH / 2)
#define CELLS_Y (DEMO_HEIGHT / 2)
#define MARGIN_X 64 /* cells the window moves each way */
#define MARGIN_Y 36
#define TABLE_X (CELLS_X + 2 * MARGIN_X)
#define TABLE_Y (CELLS_Y + 2 * MARGIN_Y)
#define TEX 32       /* the texture's values, a row of the palette each shade */
#define SHADES 7     /* rows, near (bright) to far */
#define FIRST 1      /* the first row's first entry */
#define TEX_DEEP 64u /* the texture's rows, along the tunnel */
#define DEEP_MASK ((TEX_DEEP - 1u) << 8)

static struct {
                uint16_t *table;      /* TABLE_X × TABLE_Y: u | v << 8 (16 bits: half the reading of 32) */
                uint8_t   shade[256]; /* v's shade: its row's first entry */
                /* 256 round × 64 deep, 0..TEX-1, indexed v << 8 | u: 16 KB, not 64 (the cores share one L1
                 * data cache, and a synth on the other core takes its share: docs/platforms/esp32p4.md). */
                uint8_t *texture;
                uint32_t palette[PAL_PARTS];
                float    u, v;           /* the texture's offset (turn, flight) */
                float    look_x, look_y; /* the window's offset (cells), eased towards the steer */
                /* This frame's, for the rows' bands. */
                bmds_canvas_t *canvas;
                int32_t        ox, oy;
                uint32_t       du, dv;
} s;

/* Cell rows [first, end) of this frame. */
static void rows(void *arg, int32_t first, int32_t end) {
        (void)arg;
        const bmds_canvas_t *c = s.canvas;
        const uint32_t       du = s.du, dv = s.dv;
        const uint8_t       *tex = s.texture, *shade = s.shade;
        for (int32_t row = first; row < end; ++row) {
                const uint16_t *in = s.table + (size_t)(row + s.oy) * TABLE_X + s.ox;
                uint32_t       *a = (uint32_t *)(void *)(c->pixels + (size_t)(2 * row) * c->stride);
                uint32_t       *b = (uint32_t *)(void *)(c->pixels + (size_t)(2 * row + 1) * c->stride);
                /* Two cells, four pixels across, two rows: two stores. */
                for (int32_t x = 0; x < CELLS_X; x += 2) {
                        const uint32_t c0 = in[x], c1 = in[x + 1];
                        const uint32_t p0 = shade[c0 >> 8] + tex[((c0 + du) & 0xffu) | ((c0 + dv) & DEEP_MASK)];
                        const uint32_t p1 = shade[c1 >> 8] + tex[((c1 + du) & 0xffu) | ((c1 + dv) & DEEP_MASK)];
                        const uint32_t w = p0 * 0x0101u | p1 * 0x01010000u;
                        *a++ = w;
                        *b++ = w;
                }
        }
        return;
}

static int init(demo_t *demo) {
        (void)demo;
        s.table = malloc(sizeof(uint16_t) * TABLE_X * TABLE_Y);
        s.texture = malloc(256 * TEX_DEEP);
        if (s.table == NULL || s.texture == NULL) return -BMDS_ENOMEM;
        for (int32_t y = 0; y < TABLE_Y; ++y)
                for (int32_t x = 0; x < TABLE_X; ++x) {
                        /* Square cells: the table's y in the same units as x. */
                        const float    dx = (float)(x - TABLE_X / 2), dy = (float)(y - TABLE_Y / 2);
                        const float    r = sqrtf(dx * dx + dy * dy) + 0.5f;
                        const uint32_t u = (uint32_t)(int32_t)(256.0f * atan2f(dy, dx) / 6.28318531f) & 255u;
                        const float    depth = 4096.0f / r;
                        const uint32_t v = depth > 255.0f ? 255u : (uint32_t)depth; /* the far end: one row */
                        s.table[y * TABLE_X + x] = (uint16_t)(u | v << 8);
                }
        /* Near (a small v: the edges) bright; the far end (the middle) dark. */
        for (uint32_t v = 0; v < 256; ++v) {
                uint32_t shade = v == 0 ? SHADES - 1 : (4096u / v) / 22u;
                if (shade > SHADES - 1) shade = SHADES - 1;
                s.shade[v] = (uint8_t)(FIRST + shade * TEX);
        }
        /* The texture: bricks of an XOR pattern, with mortar lines (value 0). */
        for (uint32_t v = 0; v < TEX_DEEP; ++v)
                for (uint32_t u = 0; u < 256; ++u) {
                        uint32_t value = ((u ^ v) >> 2) & (TEX - 1);
                        if ((v & 31u) < 2 || ((u + ((v >> 5) & 1u) * 16u) & 31u) < 2) value = 0;
                        s.texture[v << 8 | u] = (uint8_t)value;
                }
        /* Each row: the texture's colours (a loop of hues, the mortar dark) at the row's brightness. */
        s.palette[0] = 0;
        for (uint32_t r = 0; r < SHADES; ++r) {
                const float bright = 0.12f + 0.88f * (float)r / (SHADES - 1);
                for (uint32_t k = 0; k < TEX; ++k) {
                        const float h = 0.55f + 0.35f * dsin(6.28318531f * (float)k / TEX);
                        s.palette[FIRST + r * TEX + k] =
                                k == 0 ? bmds_palette_hsv(0.6f, 0.5f, 0.08f * bright)
                                       : bmds_palette_hsv(h, 0.75f, bright * (0.55f + 0.45f * (float)(k & 7u) / 7.0f));
                }
        }
        return 0;
}

static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.5f);
        s.look_x = s.look_y = 0.0f;
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER && (e->data[2] & 1u) != 0 && !demo->button)
                pal_flash(&demo->pal, 0xffe0c0, 0.7f, 2.5f);
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        const sync_t *y = &demo->sync;
        /* The flight: a texture's length a bar and a half, the kick pushing; the turn, slow, the clap twisting. */
        s.v += dt * (256.0f / (1.5f * y->bar_seconds)) * (1.0f + 1.2f * y->kick);
        s.u += dt * (18.0f + 90.0f * y->clap) * dsin(t * 0.21f + 1.0f);
        s.u -= 256.0f * (float)(int32_t)(s.u / 256.0f);
        s.v -= 256.0f * (float)(int32_t)(s.v / 256.0f);
        /* Where the end is: a slow figure, or the pointer's. */
        float want_x = MARGIN_X * 0.9f * dsin(t * 0.47f), want_y = MARGIN_Y * 0.9f * dsin(t * 0.31f + 0.7f);
        if (demo->pointer_x >= 0) {
                want_x = MARGIN_X * (2.0f * (float)demo->pointer_x / DEMO_WIDTH - 1.0f);
                want_y = MARGIN_Y * (2.0f * (float)demo->pointer_y / DEMO_HEIGHT - 1.0f);
        }
        const float ease = dt * 3.0f > 1.0f ? 1.0f : dt * 3.0f;
        s.look_x += (want_x - s.look_x) * ease;
        s.look_y += (want_y - s.look_y) * ease;
        s.ox = MARGIN_X - (int32_t)s.look_x, s.oy = MARGIN_Y - (int32_t)s.look_y;
        s.du = (uint32_t)(int32_t)s.u & 255u, s.dv = ((uint32_t)(int32_t)s.v & (TEX_DEEP - 1u)) << 8;
        s.canvas = c;
        rows(NULL, 0, CELLS_Y);
        /* Each row's colours cycle, faster on the kick. */
        const int32_t turn = 1 + (int32_t)y->kicks;
        for (uint32_t r = 0; r < SHADES; ++r) pal_rotate(&demo->pal, FIRST + r * TEX + 1, TEX - 1, turn);
        if (y->kicks != 0) pal_flash(&demo->pal, 0x203060, 0.25f, 5.0f);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_tunnel = {"Tunnel", init, enter, frame, event};
