/**
 * @file      solids.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's solids (bmds/3d.h, devplan/3d.md step 2): a flat-shaded cube, a Gouraud torus, a
 * faceted sphere and a smooth one orbiting, over a wire floor, drawn far to near. The light comes
 * from where the pointer is; Space switches every solid between flat and Gouraud; S changes the
 * middle shape (torus, torus knot, cylinder, cone, icosphere, an extruded letter B); Escape or
 * closing the window ends it. On L8 the shades are palette ramps (bmds/palette.h); with -f 565 or
 * 888 the same scene in RGB. The frame's time and counters show at the top.
 *
 *     solids [-F] [-f l8|565|888]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/3d.h>
#include <bmds/game.h>
#include <bmds/palette.h>

#define WIDTH 640
#define HEIGHT 360
#define PI 3.14159265f
#define RAMP 48

#define KEY_ESCAPE 0x00070029u
#define KEY_SPACE 0x0007002cu
#define KEY_S 0x00070016u
#define SHAPES 6

/* L8: a background, text, the floor, and four ramps of RAMP. */
#define INK_BACKGROUND 0
#define INK_TEXT 1
#define INK_FLOOR 2
#define RAMP_RED 16
#define RAMP_GREEN (RAMP_RED + RAMP)
#define RAMP_BLUE (RAMP_GREEN + RAMP)
#define RAMP_GOLD (RAMP_BLUE + RAMP)

typedef struct {
                bmds_3d_t     *ctx;
                bmds_3d_mesh_t cube, ball, facets, floor;
                bmds_3d_mesh_t middle[SHAPES]; /* the torus, then the others S steps through */
                int            shape;
                uint32_t       format;
                uint32_t       palette[256];
                int            palette_set, smooth;
                float          time, light_x, light_y;
                char           status[128];
                int64_t        second_ns, last_ns;
                float          period_ms; /* between frames, smoothed */
                float          draw_ms;   /* drawing a frame, smoothed */
                uint32_t       frames, fps;
} state_t;

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        state_t *s = bmds_game_arg(game);
        if (e->type == BMDS_EVENT_CLOSE) {
                bmds_game_quit(game);
        } else if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0) {
                if (e->data[0] == KEY_ESCAPE) bmds_game_quit(game);
                if (e->data[0] == KEY_SPACE) s->smooth = !s->smooth;
                if (e->data[0] == KEY_S) s->shape = (s->shape + 1) % SHAPES;
        } else if (e->type == BMDS_EVENT_POINTER && (int32_t)e->data[0] != BMDS_POINTER_GONE) {
                /* The pointer's place, -1..1 each way: where the light comes from. */
                s->light_x = 2.0f * (float)(int32_t)e->data[0] / WIDTH - 1.0f;
                s->light_y = 1.0f - 2.0f * (float)(int32_t)e->data[1] / HEIGHT;
        }
        return;
}

static void on_tick(bmds_game_t *game) {
        state_t *s = bmds_game_arg(game);
        s->time += 1.0f / 60.0f;
        return;
}

static bmds_3d_material_t solid(uint32_t mode, uint32_t ramp, uint32_t rgb) {
        bmds_3d_material_t m;
        memset(&m, 0, sizeof(m));
        m.size = sizeof(m);
        m.mode = mode;
        m.flags = BMDS_3D_CULL_BACK;
        m.ramp_first = ramp;
        m.ramp_count = RAMP;
        m.rgb = rgb;
        return m;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        state_t *s = bmds_game_arg(game);
        if (s->format == BMDS_PIXEL_L8 && !s->palette_set) {
                (void)bmds_game_palette(game, 0, 256, s->palette);
                s->palette_set = 1;
        }
        const int64_t start = now_ns();
        const int     l8 = s->format == BMDS_PIXEL_L8;
        /* Every pixel repainted (the loop copies nothing forward), the clear by the 2D-DMA. */
        const bmds_rect_t whole = {0, 0, (uint32_t)c->width, (uint32_t)c->height};
        bmds_game_repainted(game, &whole);
        bmds_game_dma_fill(game, &whole, 1, l8 ? INK_BACKGROUND : bmds_canvas_pixel(c, 0x080810));
        bmds_3d_t *ctx = s->ctx;
        bmds_3d_set_target(ctx, c);
        bmds_3d_begin(ctx);

        const float t = s->time;
        bmds_mat4_t view;
        bmds_mat4_look_at(&view, bmds_vec3(6.0f * sinf(t * 0.1f), 2.5f, 6.0f * cosf(t * 0.1f)), bmds_vec3(0, 0, 0),
                          bmds_vec3(0, 1, 0));
        bmds_3d_set_view(ctx, &view);
        const bmds_3d_light_t light = {
                sizeof(light), BMDS_3D_LIGHT_DIRECTIONAL, BMDS_3D_LIGHT_VIEW, s->light_x, s->light_y, 0.8f, 0.7f};
        (void)bmds_3d_set_light(ctx, 0, &light);
        const uint32_t smooth = s->smooth ? BMDS_3D_GOURAUD : BMDS_3D_FLAT;

        /* The floor: lines, drawn at once (before the solids, which go over it). */
        bmds_3d_material_t floor;
        memset(&floor, 0, sizeof(floor));
        floor.size = sizeof(floor);
        floor.mode = BMDS_3D_WIRE;
        floor.pixel = l8 ? INK_FLOOR : bmds_canvas_pixel(c, 0x203040);
        bmds_mat4_t place;
        bmds_mat4_translation(&place, 0.0f, -1.6f, 0.0f);
        bmds_3d_load(ctx, &place);
        (void)bmds_3d_draw(ctx, &s->floor, &floor);

        bmds_mat4_translation(&place, -2.4f, 0.0f, 0.0f);
        bmds_3d_load(ctx, &place);
        bmds_3d_rotate(ctx, bmds_vec3(1, 1, 0.3f), t * 0.8f);
        const bmds_3d_material_t red = solid(BMDS_3D_FLAT, RAMP_RED, 0xff5040);
        (void)bmds_3d_draw(ctx, &s->cube, &red);

        bmds_mat4_identity(&place);
        bmds_3d_load(ctx, &place);
        bmds_3d_rotate(ctx, bmds_vec3(1, 0, 0), t * 0.6f);
        bmds_3d_rotate(ctx, bmds_vec3(0, 1, 0), t * 0.35f);
        const bmds_3d_material_t green = solid(smooth, RAMP_GREEN, 0x50ff70);
        (void)bmds_3d_draw(ctx, &s->middle[s->shape], &green);

        bmds_mat4_translation(&place, 2.4f, 0.0f, 0.0f);
        bmds_3d_load(ctx, &place);
        bmds_3d_rotate(ctx, bmds_vec3(0, 1, 0), t);
        const bmds_3d_material_t blue = solid(BMDS_3D_FLAT, RAMP_BLUE, 0x5080ff);
        (void)bmds_3d_draw(ctx, &s->facets, &blue);

        bmds_mat4_translation(&place, 3.0f * cosf(t * 0.9f), 1.2f + 0.3f * sinf(t * 2.3f), 3.0f * sinf(t * 0.9f));
        bmds_3d_load(ctx, &place);
        const bmds_3d_material_t gold = solid(smooth, RAMP_GOLD, 0xffd040);
        (void)bmds_3d_draw(ctx, &s->ball, &gold);
        bmds_3d_end(ctx);

        bmds_3d_stats_t st;
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        bmds_3d_stats(ctx, &st);
        const int64_t now = now_ns();
        if (s->last_ns != 0) s->period_ms += ((float)(start - s->last_ns) / 1e6f - s->period_ms) * 0.05f;
        s->draw_ms += ((float)(now - start) / 1e6f - s->draw_ms) * 0.05f;
        s->last_ns = start;
        ++s->frames;
        if (now - s->second_ns >= 1000000000) {
                s->fps = s->frames;
                s->frames = 0;
                s->second_ns = now;
        }
        snprintf(s->status, sizeof(s->status), "%u fps  draw %.2f ms  frame %.1f ms  %u triangles  %u pixels  %s",
                 (unsigned)s->fps, (double)s->draw_ms, (double)s->period_ms, (unsigned)st.triangles,
                 (unsigned)st.pixels, s->smooth ? "Gouraud" : "flat");
        bmds_canvas_text(c, BMDS_GAME_FONT_DEFAULT, 4, 2, s->status, l8 ? INK_TEXT : bmds_canvas_pixel(c, 0xc0c8d0));
        bmds_canvas_damage_all(c);
        return;
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

int main(int argc, char **argv) {
        static state_t     s;
        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = WIDTH;
        config.height = HEIGHT;
        config.format = BMDS_PIXEL_L8;
        config.flags = BMDS_SURFACE_WHOLE;
        config.title = "Solids";
        int opt;
        while ((opt = getopt(argc, argv, "Ff:")) != -1) {
                if (opt == 'F') {
                        config.flags |= BMDS_SURFACE_FULL_SCREEN;
                } else if (opt == 'f') {
                        config.format = strcmp(optarg, "565") == 0   ? BMDS_PIXEL_RGB565
                                        : strcmp(optarg, "888") == 0 ? BMDS_PIXEL_BGR24
                                                                     : BMDS_PIXEL_L8;
                } else {
                        fprintf(stderr, "usage: solids [-F] [-f l8|565|888]\n");
                        return 2;
                }
        }
        s.format = config.format;
        s.smooth = 1;
        s.light_x = -0.4f;
        s.light_y = 0.6f;

        s.palette[INK_BACKGROUND] = 0x080810;
        s.palette[INK_TEXT] = 0xc0c8d0;
        s.palette[INK_FLOOR] = 0x203040;
        /* Each ramp: dark to the colour three quarters up, then towards white (a highlight). */
        static const uint32_t colours[4] = {0xd03828, 0x30c050, 0x3060e0, 0xe0b020};
        for (int i = 0; i < 4; ++i) {
                const uint32_t first = RAMP_RED + (uint32_t)i * RAMP, lit = RAMP * 3 / 4;
                bmds_palette_ramp(s.palette, first, lit + 1, 0x080808, colours[i]);
                bmds_palette_ramp(s.palette, first + lit, RAMP - lit, colours[i], 0xfff0e0);
        }

        /* The scratch in PSRAM: internal RAM for it (BMDS_MEM_FAST, ~10% faster) left the kernel too
           little to go full screen (3d.md §13, step 3). */
        bmds_3d_config_t c3;
        memset(&c3, 0, sizeof(c3));
        c3.size = sizeof(c3);
        c3.cores = 2; /* a helper on the other core draws a band */
        int err = bmds_3d_create(&c3, &s.ctx);
        if (err == 0) err = make(&s.cube, BMDS_3D_SHAPE_CUBE, 0, 0, 0.9f, 0.0f);
        if (err == 0) err = make(&s.middle[0], BMDS_3D_SHAPE_TORUS, 40, 20, 1.2f, 0.45f);
        if (err == 0) err = make(&s.middle[1], BMDS_3D_SHAPE_TORUS_KNOT, 160, 10, 1.5f, 0.22f);
        if (err == 0) err = make(&s.middle[2], BMDS_3D_SHAPE_CYLINDER, 32, 1, 0.8f, 1.1f);
        if (err == 0) err = make(&s.middle[3], BMDS_3D_SHAPE_CONE, 32, 4, 1.0f, 1.1f);
        if (err == 0) err = make(&s.middle[4], BMDS_3D_SHAPE_ICOSPHERE, 3, 0, 1.2f, 0.0f);
        /* A letter B's outer outline (its holes left out: one simple polygon), 2 units high. */
        static const float letter_b[] = {-0.8f,  -1.0f, 0.4f,  -1.0f, 0.8f, -0.75f, 0.85f, -0.35f, 0.55f,
                                         -0.05f, 0.75f, 0.25f, 0.75f, 0.7f, 0.4f,   1.0f,  -0.8f,  1.0f};
        if (err == 0) err = bmds_3d_mesh_extrude(letter_b, 9, 0.5f, 0, &s.middle[5]);
        if (err == 0) err = make(&s.ball, BMDS_3D_SHAPE_SPHERE, 24, 16, 0.5f, 0.0f);
        if (err == 0) err = make(&s.facets, BMDS_3D_SHAPE_SPHERE, 10, 6, 0.9f, 0.0f);
        if (err == 0) err = make(&s.floor, BMDS_3D_SHAPE_GRID, 16, 0, 6.0f, 0.0f);
        if (err < 0) {
                fprintf(stderr, "solids: %s\n", strerror(-err));
                return 1;
        }
        bmds_mat4_t projection;
        bmds_mat4_perspective(&projection, 55.0f * PI / 180.0f, (float)WIDTH / (float)HEIGHT, 0.5f, 100.0f);
        bmds_3d_set_projection(s.ctx, &projection);
        s.second_ns = now_ns();

        config.arg = &s;
        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        config.options = BMDS_GAME_REPAINTS;
        err = bmds_game_run(&config);
        bmds_3d_mesh_free(&s.floor);
        bmds_3d_mesh_free(&s.facets);
        bmds_3d_mesh_free(&s.ball);
        for (int i = 0; i < SHAPES; ++i) bmds_3d_mesh_free(&s.middle[i]);
        bmds_3d_mesh_free(&s.cube);
        bmds_3d_destroy(s.ctx);
        if (err < 0) {
                fprintf(stderr, "solids: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
