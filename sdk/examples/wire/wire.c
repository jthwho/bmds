/**
 * @file      wire.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's wireframes (bmds/3d.h, devplan/3d.md step 1): a starfield rushing past, a cube, a
 * torus and an orbiting sphere, shaded by distance along palette ramps on a 640×360 L8 surface.
 * Dragging with the mouse turns the camera, the wheel moves it nearer or farther, Space shows or
 * hides the edges of back faces, Escape or closing the window ends it. The frame's time and
 * counters show at the top.
 *
 * With -e nothing is drawn but the clear and the counters: what presenting a whole frame costs.
 *
 *     wire [-F] [-e] [-n STARS]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/3d.h>
#include <bmds/game.h>

#define WIDTH 640
#define HEIGHT 360
#define MAX_STARS 2000
#define PI 3.14159265f

/* The palette: a background, text, and three ramps of 32, dark to bright. */
#define INK_BACKGROUND 0
#define INK_TEXT 1
#define RAMP_CYAN 32
#define RAMP_MAGENTA 64
#define RAMP_GOLD 96
#define RAMP_STARS 128
#define RAMP 32

#define KEY_ESCAPE 0x00070029u
#define KEY_SPACE 0x0007002cu

typedef struct {
                bmds_3d_t     *ctx;
                bmds_3d_mesh_t cube, torus, sphere;
                float          stars[MAX_STARS * 3];
                int32_t        star_count;
                float          yaw, pitch, distance; /* the camera, round the origin */
                float          time;
                int            dragging, hidden, empty;
                int32_t        last_x, last_y;
                uint32_t       palette[256];
                int            palette_set;
                char           status[96];
                int64_t        second_ns;
                uint32_t       frames, fps;
} state_t;

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

static float frand(float lo, float hi) {
        return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static void ramp(uint32_t *palette, uint32_t first, uint32_t rgb) {
        for (uint32_t i = 0; i < RAMP; ++i) {
                const uint32_t r = (rgb >> 16 & 0xffu) * (i + 1) / RAMP, g = (rgb >> 8 & 0xffu) * (i + 1) / RAMP;
                const uint32_t b = (rgb & 0xffu) * (i + 1) / RAMP;
                palette[first + i] = r << 16 | g << 8 | b;
        }
        return;
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        state_t *s = bmds_game_arg(game);
        if (e->type == BMDS_EVENT_CLOSE) {
                bmds_game_quit(game);
        } else if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0) {
                if (e->data[0] == KEY_ESCAPE) bmds_game_quit(game);
                if (e->data[0] == KEY_SPACE) s->hidden = !s->hidden;
        } else if (e->type == BMDS_EVENT_POINTER) {
                const int32_t x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                const int     down = (e->data[2] & 1u) != 0 && x != BMDS_POINTER_GONE;
                if (down && s->dragging) {
                        s->yaw += (float)(x - s->last_x) * 0.01f;
                        s->pitch += (float)(y - s->last_y) * 0.01f;
                        if (s->pitch > 1.4f) s->pitch = 1.4f;
                        if (s->pitch < -1.4f) s->pitch = -1.4f;
                }
                s->dragging = down;
                s->last_x = x, s->last_y = y;
        } else if (e->type == BMDS_EVENT_WHEEL) {
                s->distance -= (float)(int32_t)e->data[0] * 0.5f;
                if (s->distance < 3.0f) s->distance = 3.0f;
                if (s->distance > 30.0f) s->distance = 30.0f;
        }
        return;
}

static void on_tick(bmds_game_t *game) {
        state_t *s = bmds_game_arg(game);
        s->time += 1.0f / 60.0f;
        /* The stars come towards the camera and start again far away. */
        for (int32_t i = 0; i < s->star_count; ++i) {
                float *p = s->stars + 3 * i;
                p[2] += 0.25f;
                if (p[2] > 10.0f) {
                        p[0] = frand(-40.0f, 40.0f);
                        p[1] = frand(-25.0f, 25.0f);
                        p[2] = -60.0f;
                }
        }
        if (!s->dragging) s->yaw += 0.003f;
        return;
}

static bmds_3d_material_t shaded(uint32_t mode, uint32_t ramp_first, uint32_t flags) {
        bmds_3d_material_t m;
        memset(&m, 0, sizeof(m));
        m.size = sizeof(m);
        m.mode = mode;
        m.flags = BMDS_3D_DEPTH_CUE | flags;
        m.ramp_first = ramp_first;
        m.ramp_count = RAMP;
        return m;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        state_t *s = bmds_game_arg(game);
        if (!s->palette_set) {
                bmds_surface_palette_t p = {sizeof(p), 0, 256, s->palette};
                (void)bmds_surface_control(bmds_game_fd(game), BMDS_SURFACE_PALETTE, &p, sizeof(p));
                s->palette_set = 1;
        }
        const int64_t start = now_ns();
        bmds_canvas_clear(c, INK_BACKGROUND);
        bmds_3d_t *ctx = s->ctx;
        bmds_3d_set_target(ctx, c);
        bmds_3d_begin(ctx);
        if (s->empty) goto counters;

        /* The stars, in the camera's own space: always rushing at the viewer. */
        bmds_mat4_t identity;
        bmds_mat4_identity(&identity);
        bmds_3d_set_view(ctx, &identity);
        bmds_3d_load(ctx, &identity);
        bmds_3d_set_fog(ctx, 2.0f, 60.0f);
        const bmds_3d_material_t stars = shaded(BMDS_3D_POINTS, RAMP_STARS, 0);
        (void)bmds_3d_points(ctx, s->stars, (uint32_t)s->star_count, &stars);

        /* The objects, round the origin. */
        const bmds_vec3_t eye = bmds_vec3(s->distance * cosf(s->pitch) * sinf(s->yaw), s->distance * sinf(s->pitch),
                                          s->distance * cosf(s->pitch) * cosf(s->yaw));
        bmds_mat4_t       view;
        bmds_mat4_look_at(&view, eye, bmds_vec3(0, 0, 0), bmds_vec3(0, 1, 0));
        bmds_3d_set_view(ctx, &view);
        bmds_3d_set_fog(ctx, s->distance - 3.0f, s->distance + 4.0f);
        const uint32_t cull = s->hidden ? BMDS_3D_CULL_BACK : 0;
        const float    t = s->time;

        bmds_3d_load(ctx, &identity);
        bmds_3d_rotate(ctx, bmds_vec3(1, 1, 0), t * 0.9f);
        const bmds_3d_material_t cyan = shaded(BMDS_3D_WIRE, RAMP_CYAN, cull);
        (void)bmds_3d_draw(ctx, &s->cube, &cyan);

        bmds_3d_load(ctx, &identity);
        bmds_3d_rotate(ctx, bmds_vec3(1, 0, 0), t * 0.7f);
        bmds_3d_rotate(ctx, bmds_vec3(0, 0, 1), t * 0.4f);
        const bmds_3d_material_t magenta = shaded(BMDS_3D_WIRE, RAMP_MAGENTA, cull);
        (void)bmds_3d_draw(ctx, &s->torus, &magenta);

        bmds_3d_load(ctx, &identity);
        bmds_3d_translate(ctx, 3.2f * cosf(t * 1.3f), 0.6f * sinf(t * 2.1f), 3.2f * sinf(t * 1.3f));
        bmds_3d_rotate(ctx, bmds_vec3(0, 1, 0), t * 2.0f);
        const bmds_3d_material_t gold = shaded(BMDS_3D_WIRE, RAMP_GOLD, cull);
        (void)bmds_3d_draw(ctx, &s->sphere, &gold);
counters:
        bmds_3d_end(ctx);

        bmds_3d_stats_t st;
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        bmds_3d_stats(ctx, &st);
        const int64_t now = now_ns();
        ++s->frames;
        if (now - s->second_ns >= 1000000000) {
                s->fps = s->frames;
                s->frames = 0;
                s->second_ns = now;
        }
        snprintf(s->status, sizeof(s->status), "%u fps  %.2f ms  %u lines  %u points  %u pixels%s", (unsigned)s->fps,
                 (double)(now - start) / 1e6, (unsigned)st.lines, (unsigned)st.points, (unsigned)st.pixels,
                 s->hidden ? "  (hidden lines)" : "");
        bmds_canvas_text(c, BMDS_GAME_FONT_DEFAULT, 4, 2, s->status, INK_TEXT);
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
        config.title = "Wire";
        s.star_count = 600;
        int opt;
        while ((opt = getopt(argc, argv, "Fen:")) != -1) {
                if (opt == 'e') {
                        s.empty = 1;
                } else if (opt == 'F') {
                        config.flags |= BMDS_SURFACE_FULL_SCREEN;
                } else if (opt == 'n') {
                        s.star_count = atoi(optarg);
                        if (s.star_count < 0 || s.star_count > MAX_STARS) return 2;
                } else {
                        fprintf(stderr, "usage: wire [-F] [-e] [-n STARS]\n");
                        return 2;
                }
        }

        s.palette[INK_BACKGROUND] = 0x000008;
        s.palette[INK_TEXT] = 0xc0c8d0;
        ramp(s.palette, RAMP_CYAN, 0x40e0ff);
        ramp(s.palette, RAMP_MAGENTA, 0xff40c0);
        ramp(s.palette, RAMP_GOLD, 0xffd040);
        ramp(s.palette, RAMP_STARS, 0xffffff);
        s.distance = 8.0f;
        s.pitch = 0.3f;
        s.hidden = 1;
        for (int32_t i = 0; i < s.star_count; ++i) {
                s.stars[3 * i] = frand(-40.0f, 40.0f);
                s.stars[3 * i + 1] = frand(-25.0f, 25.0f);
                s.stars[3 * i + 2] = frand(-60.0f, 10.0f);
        }

        int err = bmds_3d_create(NULL, &s.ctx);
        if (err == 0) err = make(&s.cube, BMDS_3D_SHAPE_CUBE, 0, 0, 1.0f, 0.0f);
        if (err == 0) err = make(&s.torus, BMDS_3D_SHAPE_TORUS, 32, 12, 2.2f, 0.5f);
        if (err == 0) err = make(&s.sphere, BMDS_3D_SHAPE_SPHERE, 12, 8, 0.6f, 0.0f);
        if (err < 0) {
                fprintf(stderr, "wire: %s\n", strerror(-err));
                return 1;
        }
        bmds_mat4_t projection;
        bmds_mat4_perspective(&projection, 60.0f * PI / 180.0f, (float)WIDTH / (float)HEIGHT, 0.5f, 100.0f);
        bmds_3d_set_projection(s.ctx, &projection);
        s.second_ns = now_ns();

        config.arg = &s;
        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        err = bmds_game_run(&config);
        bmds_3d_mesh_free(&s.sphere);
        bmds_3d_mesh_free(&s.torus);
        bmds_3d_mesh_free(&s.cube);
        bmds_3d_destroy(s.ctx);
        if (err < 0) {
                fprintf(stderr, "wire: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
