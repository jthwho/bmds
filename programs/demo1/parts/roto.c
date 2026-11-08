/**
 * @file      roto.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 8 (devplan/demo.md §3): the rotozoomer and the cube. A 128×128 picture of the BMDS logo on
 * a checker of two colour ramps, turned and zoomed over the whole screen in one call
 * (bmds_game_affine_rotozoom(), each texel 2×2 pixels as the tunnel's); half way, it dims and a cube flies in, lit, its faces a plasma
 * made afresh every frame into a 64×64 canvas that is the cube's texture (bmds_canvas_image(),
 * libbmds3d's TEXTURED_FLAT through a light table, perspective-correct), a small cube of the logo
 * going round it. The kick zooms and pumps; the clap spins. A click turns the zoom about.
 */

#include <stdlib.h>
#include <string.h>
#include <bmds/3d.h>
#include <bmds/palette.h>
#include "../demo.h"

#define SIDE 128 /* 16 KB: the cores share one L1 data cache (docs/platforms/esp32p4.md) */
#define PLASMA 64
/* The palette: the logo's two ramps and its letters, the plasma's ramp, and dim copies of the
 * logo's for the background behind the cube (outside the light table's candidates). */
#define LOGO_FIRST 1 /* 2 × 32 */
#define LOGO_INK 65
#define PLASMA_FIRST 66 /* 64 */
#define LIT_COUNT 130   /* the light table's candidates: 0 to 129 */
#define DIM_FIRST 130   /* LOGO_FIRST's 65 entries, dimmed */

static struct {
                uint8_t          *logo, *dim, *plasma;
                bmds_canvas_t     logo_canvas, plasma_canvas;
                bmds_game_image_t logo_image, dim_image, plasma_image;
                uint8_t          *light;
                uint32_t          palette[PAL_PARTS];
                bmds_3d_mesh_t    cube;
                float             spin, zoom_sign;
} s;

static int init(demo_t *demo) {
        (void)demo;
        s.logo = malloc(SIDE * SIDE);
        s.dim = malloc(SIDE * SIDE);
        s.plasma = malloc(PLASMA * PLASMA);
        s.light = malloc(BMDS_3D_LIGHT_TABLE_SIZE);
        if (s.logo == NULL || s.dim == NULL || s.plasma == NULL || s.light == NULL) return -BMDS_ENOMEM;
        /* The picture: a checker of 32-texel squares, each an XOR pattern in one of two ramps. */
        for (uint32_t y = 0; y < SIDE; ++y)
                for (uint32_t x = 0; x < SIDE; ++x) {
                        const uint32_t ramp = ((x >> 5) ^ (y >> 5)) & 1u;
                        s.logo[y * SIDE + x] = (uint8_t)(LOGO_FIRST + ramp * 32 + (((x ^ y) >> 1) & 31u));
                }
        int err = bmds_canvas_init(&s.logo_canvas, s.logo, SIDE, SIDE, SIDE, BMDS_PIXEL_L8);
        if (err != 0) return err;
        const int32_t big = bmds_game_font_scaled(2);
        if (big < 0) return big;
        /* BMDS across the middle, outlined in black, "DEMO #1" under it. */
        const int32_t w = bmds_game_text_width(big, "BMDS");
        for (int dy = -2; dy <= 2; dy += 2)
                for (int dx = -2; dx <= 2; dx += 2)
                        (void)bmds_canvas_text(&s.logo_canvas, big, (SIDE - w) / 2 + dx, 30 + dy, "BMDS", 0);
        (void)bmds_canvas_text(&s.logo_canvas, big, (SIDE - w) / 2, 30, "BMDS", LOGO_INK);
        const int32_t w2 = bmds_game_text_width(BMDS_GAME_FONT_DEFAULT, "DEMO #1");
        (void)bmds_canvas_text(&s.logo_canvas, BMDS_GAME_FONT_DEFAULT, (SIDE - w2) / 2 + 1, 74 + 1, "DEMO #1", 0);
        (void)bmds_canvas_text(&s.logo_canvas, BMDS_GAME_FONT_DEFAULT, (SIDE - w2) / 2, 74, "DEMO #1", LOGO_INK);
        for (uint32_t i = 0; i < SIDE * SIDE; ++i)
                s.dim[i] = (uint8_t)(s.logo[i] == 0 ? 0 : s.logo[i] - LOGO_FIRST + DIM_FIRST);
        err = bmds_canvas_init(&s.plasma_canvas, s.plasma, PLASMA, PLASMA, PLASMA, BMDS_PIXEL_L8);
        if (err == 0) err = bmds_canvas_image(&s.logo_canvas, NULL, &s.logo_image);
        if (err == 0) err = bmds_canvas_image(&s.plasma_canvas, NULL, &s.plasma_image);
        if (err != 0) return err;
        s.dim_image = s.logo_image;
        s.dim_image.pixels = s.dim;

        /* The colours: magenta to gold, teal to blue, white letters; the plasma's fire-to-ice. */
        s.palette[0] = 0x000000;
        for (uint32_t i = 0; i < 32; ++i) {
                const float f = (float)i / 31.0f;
                s.palette[LOGO_FIRST + i] = bmds_palette_hsv(0.88f + 0.2f * f, 0.85f, 0.35f + 0.65f * f);
                s.palette[LOGO_FIRST + 32 + i] = bmds_palette_hsv(0.45f + 0.15f * f, 0.8f, 0.3f + 0.6f * f);
        }
        s.palette[LOGO_INK] = 0xfff8e0;
        for (uint32_t i = 0; i < PLASMA; ++i) {
                const float f = (float)i / (PLASMA - 1);
                s.palette[PLASMA_FIRST + i] = bmds_palette_hsv(0.02f + 0.6f * f, 0.9f - 0.3f * dsin(f * 3.14159f),
                                                               0.5f + 0.5f * dsin(f * 3.14159f));
        }
        for (uint32_t i = 0; i < 65; ++i) {
                const uint32_t c = s.palette[LOGO_FIRST + i];
                s.palette[DIM_FIRST + i] =
                        ((c >> 16 & 0xff) * 2 / 7) << 16 | ((c >> 8 & 0xff) * 2 / 7) << 8 | (c & 0xff) * 2 / 7;
        }
        /* The light table over the bright entries: the cubes' texels at 32 levels of light. */
        err = bmds_3d_light_table(s.light, s.palette, 0, LIT_COUNT, 0x000000);
        if (err != 0) return err;
        bmds_3d_shape_t shape;
        memset(&shape, 0, sizeof(shape));
        shape.size = sizeof(shape);
        shape.kind = BMDS_3D_SHAPE_CUBE;
        shape.radius = 1.0f;
        return bmds_3d_mesh_make(&shape, 0, &s.cube);
}

static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.3f);
        s.spin = 0.0f;
        s.zoom_sign = 1.0f;
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER && (e->data[2] & 1u) != 0 && !demo->button) s.zoom_sign = -s.zoom_sign;
        return;
}

/* This frame's plasma into its 64×64 canvas: four sines, the colours its ramp. */
static void plasma(float t) {
        for (int32_t y = 0; y < PLASMA; ++y) {
                uint8_t    *row = s.plasma + y * PLASMA;
                const float fy = (float)y * 0.098f;
                const float a = dsin(fy * 1.3f + t * 1.1f);
                for (int32_t x = 0; x < PLASMA; ++x) {
                        const float fx = (float)x * 0.098f;
                        const float v =
                                dsin(fx + t) + a + dsin((fx + fy) * 0.7f + t * 0.6f) + dsin(fx * 1.7f - t * 0.9f);
                        int32_t k = (int32_t)((v + 4.0f) * (PLASMA / 8.0f));
                        k = k < 0 ? 0 : k > PLASMA - 1 ? PLASMA - 1 : k;
                        row[x] = (uint8_t)(PLASMA_FIRST + k);
                }
        }
        return;
}

/* @p tex (SIDE × SIDE) by @p map over a half-size picture, each texel 2×2 pixels of @p c. */
static void zoom2x(bmds_canvas_t *c, const uint8_t *tex, const bmds_game_affine_t *map) {
        const uint32_t mask = SIDE - 1;
        uint32_t       u0 = (uint32_t)map->u, v0 = (uint32_t)map->v;
        const uint32_t dux = (uint32_t)map->du_dx, dvx = (uint32_t)map->dv_dx;
        for (int32_t row = 0; row < DEMO_HEIGHT / 2; ++row, u0 += (uint32_t)map->du_dy, v0 += (uint32_t)map->dv_dy) {
                uint32_t *a = (uint32_t *)(void *)(c->pixels + (size_t)(2 * row) * c->stride);
                uint32_t *b = (uint32_t *)(void *)(c->pixels + (size_t)(2 * row + 1) * c->stride);
                uint32_t  u = u0, v = v0;
                for (int32_t x = 0; x < DEMO_WIDTH / 2; x += 2) {
                        const uint32_t p0 = tex[((v >> 16) & mask) * SIDE + ((u >> 16) & mask)];
                        u += dux, v += dvx;
                        const uint32_t p1 = tex[((v >> 16) & mask) * SIDE + ((u >> 16) & mask)];
                        u += dux, v += dvx;
                        const uint32_t w = p0 * 0x0101u | p1 * 0x01010000u;
                        *a++ = w;
                        *b++ = w;
                }
        }
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        const sync_t *y = &demo->sync;
        /* Half way (four bars, or eight seconds without music): the cube. */
        const int cube = y->known ? y->pos.bar >= 4 : t >= 8.0f;
        s.spin += dt * (0.35f + 2.5f * y->clap);
        const float        zoom = (3.2f + 2.2f * dsin(t * 0.6f)) * (1.0f + 0.35f * y->kick);
        /* The map for a half-size picture, each texel then written as 2×2 pixels (the tunnel's way:
           a quarter of the lookups, two rows of word stores), chunky as the old ones were. */
        bmds_game_affine_t m;
        bmds_game_affine_rotozoom(&m, 0, 0, DEMO_WIDTH / 4.0f, DEMO_HEIGHT / 4.0f, 64.0f + 45.0f * dsin(t * 0.21f),
                                  64.0f + 35.0f * dsin(t * 0.17f), s.spin,
                                  0.5f * (s.zoom_sign > 0 ? zoom : 6.6f - zoom));
        zoom2x(c, cube ? s.dim : s.logo, &m);
        if (cube) {
                plasma(t);
                bmds_3d_t *ctx = demo->ctx;
                bmds_3d_set_target(ctx, c);
                bmds_3d_begin(ctx);
                bmds_mat4_t view;
                bmds_mat4_look_at(&view, bmds_vec3(0.0f, 0.4f, 5.2f - 0.5f * y->kick), bmds_vec3(0, 0, 0),
                                  bmds_vec3(0, 1, 0));
                bmds_3d_set_view(ctx, &view);
                const bmds_3d_light_t light = {
                        sizeof(light), BMDS_3D_LIGHT_DIRECTIONAL, BMDS_3D_LIGHT_VIEW, -0.5f, 0.7f, 1.0f, 0.8f};
                (void)bmds_3d_set_light(ctx, 0, &light);
                bmds_3d_set_ambient(ctx, 0.25f + 0.3f * y->clap);
                bmds_3d_material_t mat;
                memset(&mat, 0, sizeof(mat));
                mat.size = sizeof(mat);
                mat.mode = BMDS_3D_TEXTURED_FLAT;
                mat.flags = BMDS_3D_CULL_BACK | BMDS_3D_PERSPECTIVE;
                mat.texture = &s.plasma_image;
                mat.light_table = s.light;
                /* It flies in over the first beat of its half. */
                const float in = y->known ? (float)(y->pos.bar - 4) + y->pos.bar_phase : t - 8.0f;
                const float arrive = in >= 1.0f ? 0.0f : (1.0f - in) * (1.0f - in);
                const float pump = 1.0f + 0.18f * y->kick;
                bmds_mat4_t place;
                bmds_mat4_translation(&place, 0.0f, 0.0f, -14.0f * arrive);
                bmds_3d_load(ctx, &place);
                bmds_3d_rotate(ctx, bmds_vec3(1, 0, 0), t * 0.7f);
                bmds_3d_rotate(ctx, bmds_vec3(0, 1, 0), t * 0.53f + s.spin);
                bmds_3d_scale(ctx, pump, pump, pump);
                (void)bmds_3d_draw(ctx, &s.cube, &mat);
                /* The logo's cube, going round. */
                mat.texture = &s.logo_image;
                const float a = t * 1.3f;
                bmds_mat4_translation(&place, 2.4f * dcos(a), 0.7f * dsin(a * 0.7f), 2.4f * dsin(a) - 14.0f * arrive);
                bmds_3d_load(ctx, &place);
                bmds_3d_rotate(ctx, bmds_vec3(0.3f, 1, 0.2f), -t * 1.6f);
                bmds_3d_scale(ctx, 0.45f, 0.45f, 0.45f);
                (void)bmds_3d_draw(ctx, &s.cube, &mat);
                bmds_3d_end(ctx);
        }
        if (y->kicks != 0) pal_flash(&demo->pal, 0x302010, 0.2f, 5.0f);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_roto = {"Rotozoom", init, enter, frame, event};
