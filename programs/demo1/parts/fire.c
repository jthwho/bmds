/**
 * @file      fire.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 7 (devplan/demo.md §3): fire, and a shape burning in it. The fire is the old way: a
 * half-size field of heat, each cell a frame the average of the four below it less a little,
 * the bottom rows fed at random; heat is a palette ramp, black through red and yellow to white,
 * shown as 2×2 pixels; two fields, each frame's made from the last's. The shape (a wire
 * icosahedron, the torus knot, then Newell's teapot) is drawn by libbmds3d into the heat field
 * itself as its hottest value, so it burns: flames rise off its edges. The teapot is a `.bm3d`
 * model in the program's folder (dist/demo1 makes it with bmds-mesh3d: its patches' grid lines,
 * no diagonals); without it, the knot burns on. On the beat (sync_t): the kick feeds the fire,
 * the clap and the bass flare the shape; the pointer turns it, a click throws a fireball where it
 * is.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/3d.h>
#include <bmds/app.h>
#include <bmds/palette.h>
#include "../demo.h"

#define CELLS_X (DEMO_WIDTH / 2)
#define CELLS_Y (DEMO_HEIGHT / 2)
#define ROWS (CELLS_Y + 2) /* two fed rows below the screen */
#define RAMP_FIRST 1
#define RAMP 128
#define SHAPES 2
#define NOISE 24
#define NOISE_CELLS 4096

static struct {
                uint8_t       *heat[2]; /* CELLS_X × ROWS each: the last frame's and this one's */
                int            now;     /* this frame's */
                bmds_canvas_t  field;
                bmds_3d_mesh_t   shapes[SHAPES];
                bmds_3d_model_t *teapot;             /* NULL: not there */
                uint8_t        look[256];          /* heat to palette index */
                uint8_t        noise[NOISE_CELLS]; /* a cell's cooling, 0..NOISE: the flames' flicker */
                uint32_t       frames;
                uint8_t        hot[2][ROWS]; /* each field's rows with some heat (as last shown) */
                bmds_canvas_t *canvas;       /* this frame's */
                uint32_t       palette[PAL_PARTS];
                uint32_t       seed;
                float          yaw, pitch;
                int32_t        drag_x, drag_y;
                int32_t        burst_x, burst_y; /* a fireball to throw (-1: none) */
} s;

static uint32_t next_random(void) {
        s.seed = s.seed * 1664525u + 1013904223u;
        return s.seed >> 8;
}

static int make(bmds_3d_mesh_t *mesh, uint32_t kind, uint32_t segments, uint32_t rings, float radius, float radius2) {
        bmds_3d_shape_t shape;
        memset(&shape, 0, sizeof(shape));
        shape.size = sizeof(shape);
        shape.kind = kind;
        shape.segments = segments;
        shape.rings = rings;
        shape.radius = radius;
        shape.radius2 = radius2;
        return bmds_3d_mesh_make(&shape, 0, mesh);
}

static int init(demo_t *demo) {
        (void)demo;
        s.heat[0] = calloc(CELLS_X, ROWS);
        s.heat[1] = calloc(CELLS_X, ROWS);
        if (s.heat[0] == NULL || s.heat[1] == NULL) return -BMDS_ENOMEM;
        int err = make(&s.shapes[0], BMDS_3D_SHAPE_ICOSPHERE, 1, 0, 1.6f, 0.0f);
        if (err == 0) err = make(&s.shapes[1], BMDS_3D_SHAPE_TORUS_KNOT, 72, 5, 1.7f, 0.32f);
        if (err < 0) return err;
        const char *dir = bmds_app_dir();
        char        path[256];
        if (dir != NULL && (size_t)snprintf(path, sizeof(path), "%s/teapot.bm3d", dir) < sizeof(path)) {
                const int e = bmds_3d_model_load(path, 0, &s.teapot);
                if (e < 0) fprintf(stderr, "demo1: %s: %s\n", path, strerror(-e));
        }
        /* Black, deep red, orange, yellow, white: the ramp; the coolest eighth black. */
        s.palette[0] = 0;
        bmds_palette_ramp(s.palette, RAMP_FIRST, 24, 0x000000, 0x400000);
        bmds_palette_ramp(s.palette, RAMP_FIRST + 23, 40, 0x400000, 0xd02000);
        bmds_palette_ramp(s.palette, RAMP_FIRST + 62, 34, 0xd02000, 0xffb000);
        bmds_palette_ramp(s.palette, RAMP_FIRST + 95, 33, 0xffb000, 0xfffff0);
        for (uint32_t h = 0; h < 256; ++h) s.look[h] = (uint8_t)(h < 16 ? 0 : RAMP_FIRST + (h * RAMP) / 256u);
        s.seed = 12345;
        for (int i = 0; i < NOISE_CELLS; ++i) s.noise[i] = (uint8_t)(next_random() % (NOISE + 1));
        s.burst_x = -1;
        return 0;
}

static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.4f);
        for (int f = 0; f < 2; ++f) {
                memset(s.heat[f], 0, CELLS_X * ROWS);
                memset(s.hot[f], 0, CELLS_Y);
                s.hot[f][CELLS_Y] = s.hot[f][CELLS_Y + 1] = 1; /* the fed rows */
        }
        s.yaw = s.pitch = 0.0f;
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type != BMDS_EVENT_POINTER) return;
        const int32_t x = (int32_t)e->data[0], y = (int32_t)e->data[1];
        const int     down = (e->data[2] & 1u) != 0 && x != BMDS_POINTER_GONE;
        if (down && !demo->button) {
                s.burst_x = x / 2, s.burst_y = y / 2;
                pal_flash(&demo->pal, 0xfff0c0, 0.4f, 3.0f);
        }
        if (down && demo->button) {
                s.yaw += (float)(x - s.drag_x) * 0.01f;
                s.pitch += (float)(y - s.drag_y) * 0.01f;
        }
        s.drag_x = x, s.drag_y = y;
        return;
}

/* Rows [first, end) of this frame's field from the last's: each cell the average of the three below
 * it and the one below those, less a little that differs cell to cell and frame to frame (noise
 * read from a moving place): flicker. A row with nothing below it stays empty (the sky). */
static void rise(void *arg, int32_t first, int32_t end) {
        (void)arg;
        const uint8_t *last = s.heat[s.now ^ 1];
        uint8_t       *now = s.heat[s.now];
        const uint8_t *was = s.hot[s.now ^ 1];
        for (int32_t row = first; row < end; ++row) {
                uint8_t *out = now + (size_t)row * CELLS_X;
                if (!was[row + 1] && !was[row + 2]) {
                        if (s.hot[s.now][row]) memset(out, 0, CELLS_X); /* what it held two frames ago */
                        continue;
                }
                const uint8_t *b = last + (size_t)(row + 1) * CELLS_X, *bb = b + CELLS_X;
                const uint8_t *cool = s.noise + ((uint32_t)row * 97u + s.frames * 1031u) % (NOISE_CELLS - CELLS_X);
                for (int32_t x = 1; x < CELLS_X - 1; ++x) {
                        const uint32_t sum = b[x - 1] + b[x] + b[x + 1] + bb[x], less = cool[x];
                        out[x] = (uint8_t)(sum > less ? (sum - less) >> 2 : 0u);
                }
                out[0] = out[CELLS_X - 1] = 0;
        }
        return;
}

/* Rows [first, end) shown: 2×2 pixels a cell, two cells a pair of stores; which have heat noted. */
static void show(void *arg, int32_t first, int32_t end) {
        (void)arg;
        const bmds_canvas_t *c = s.canvas;
        for (int32_t row = first; row < end; ++row) {
                const uint8_t  *in = s.heat[s.now] + (size_t)row * CELLS_X;
                const uint32_t *words = (const uint32_t *)(const void *)in;
                uint32_t        any = 0;
                for (int32_t k = 0; k < CELLS_X / 4; ++k) any |= words[k];
                s.hot[s.now][row] = any != 0;
                uint32_t *a = (uint32_t *)(void *)(c->pixels + (size_t)(2 * row) * c->stride);
                uint32_t *b = (uint32_t *)(void *)(c->pixels + (size_t)(2 * row + 1) * c->stride);
                for (int32_t x = 0; x < CELLS_X; x += 2) {
                        const uint32_t w = s.look[in[x]] * 0x0101u | s.look[in[x + 1]] * 0x01010000u;
                        *a++ = w;
                        *b++ = w;
                }
        }
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        (void)dt;
        const sync_t *y = &demo->sync;
        /* Fed: the two rows below the screen, hotter on the kick. */
        const uint32_t floor = 120u + (uint32_t)(120.0f * y->kick);
        uint8_t       *feed = s.heat[s.now] + (size_t)CELLS_Y * CELLS_X; /* the last frame's field */
        for (int32_t x = 0; x < 2 * CELLS_X; ++x) {
                const uint32_t r = next_random();
                feed[x] = (uint8_t)((r & 3u) == 0 ? 0 : floor + (r >> 4) % (256u - floor));
        }
        ++s.frames;
        s.now ^= 1;
        rise(NULL, 0, CELLS_Y);
        (void)bmds_canvas_init(&s.field, s.heat[s.now], CELLS_X, CELLS_X, CELLS_Y, BMDS_PIXEL_L8);
        /* The shape, burning: drawn into the heat at its hottest. */
        bmds_3d_t *ctx = demo->ctx;
        bmds_3d_set_target(ctx, &s.field);
        bmds_3d_begin(ctx);
        bmds_mat4_t view, model, turn;
        bmds_mat4_translation(&view, 0.0f, 0.4f, -7.0f);
        bmds_3d_set_view(ctx, &view);
        /* Bars 0-1 of each eight the icosahedron, 2-3 the knot, 4-7 the teapot (or by the part's time). */
        const float           bars = y->known ? (float)y->pos.bar : t / y->bar_seconds;
        const int32_t         bar = (int32_t)bars % 8;
        const bmds_3d_mesh_t *shape = &s.shapes[bar < 2 ? 0 : 1];
        const int             teapot = bar >= 4 && s.teapot != NULL;
        if (teapot) shape = bmds_3d_model_mesh(s.teapot, 0);
        /* The teapot upright, turning and rocking a little; the others tumbling. */
        bmds_mat4_rotation_y(&model, t * 0.7f + s.yaw);
        bmds_mat4_rotation_x(&turn, teapot ? 0.35f + 0.2f * dsin(t * 0.9f) + s.pitch : t * 0.43f + s.pitch);
        bmds_mat4_multiply(&turn, &turn, &model);
        model = turn;
        const float size = (teapot ? 2.0f : 1.0f) * (1.0f + 0.15f * y->kick);
        bmds_mat4_scaling(&turn, size, size, size);
        bmds_mat4_multiply(&model, &model, &turn);
        bmds_3d_load(ctx, &model);
        bmds_3d_material_t m;
        memset(&m, 0, sizeof(m));
        m.size = sizeof(m);
        m.mode = BMDS_3D_WIRE;
        m.flags = BMDS_3D_CULL_BACK;
        m.pixel = 150u + (uint32_t)(105.0f * (y->clap > y->bass ? y->clap : y->bass));
        (void)bmds_3d_draw(ctx, shape, &m);
        bmds_3d_end(ctx);
        if (s.burst_x >= 0) {
                bmds_canvas_fill_ellipse(&s.field, s.burst_x, s.burst_y, 14, 12, 255);
                s.burst_x = -1;
        }

        s.canvas = c;
        show(NULL, 0, CELLS_Y);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_fire = {"Fire", init, enter, frame, event};
