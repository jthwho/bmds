/**
 * @file      stars.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 3 (devplan/demo.md §3): stars rush at the viewer, slow, and gather into the letters BMDS;
 * the letters become a wireframe that spins while the stars let go and rush on. The letters are
 * outlines extruded by libbmds3d (bmds_3d_mesh_extrude(); B's and D's holes extruded on their own:
 * in wire they are the letters' holes), shaded by distance along palette ramps; the stars are 3D
 * points. Dragging turns the letters; a click flashes.
 */

#include <stdlib.h>
#include <string.h>
#include <bmds/3d.h>
#include <bmds/palette.h>
#include "../demo.h"

#define STARS 900
#define RAMP 32
#define RAMP_STARS 1
#define RAMP_LOGO (RAMP_STARS + RAMP)
#define PIECES 7
#define GATHER_START 4.0f /* seconds into the part */
#define GATHER_END 8.0f
#define LET_GO 9.5f

/* The letters' outlines in their own units (7 high), y up; holes as pieces of their own. */
static const float b_outer[] = {0, 0, 4, 0, 5, 1, 5, 2.6f, 4.4f, 3.5f, 5, 4.4f, 5, 6, 4, 7, 0, 7};
static const float b_hole1[] = {1.3f, 1.2f, 3.6f, 1.2f, 3.8f, 1.5f, 3.8f, 2.6f, 3.6f, 2.9f, 1.3f, 2.9f};
static const float b_hole2[] = {1.3f, 4.1f, 3.6f, 4.1f, 3.8f, 4.4f, 3.8f, 5.5f, 3.6f, 5.8f, 1.3f, 5.8f};
static const float m_outer[] = {0, 0, 1.3f, 0, 1.3f, 4.6f, 2.5f, 2.8f, 3.7f, 4.6f, 3.7f, 0,
                                5, 0, 5, 7, 3.8f, 7, 2.5f, 5, 1.2f, 7, 0, 7};
static const float d_outer[] = {0, 0, 3.5f, 0, 5, 1.5f, 5, 5.5f, 3.5f, 7, 0, 7};
static const float d_hole[] = {1.3f, 1.3f, 3.0f, 1.3f, 3.7f, 2.0f, 3.7f, 5.0f, 3.0f, 5.7f, 1.3f, 5.7f};
static const float s_outer[] = {0, 0, 4, 0, 5, 1, 5, 3, 4, 4, 1.3f, 4, 1.3f, 5.7f, 5, 5.7f,
                                5, 7, 1, 7, 0, 6, 0, 4, 1, 3, 3.7f, 3, 3.7f, 1.3f, 0, 1.3f};

static const struct {
                const float *xy;
                uint32_t     points;
                float        x; /* the letter's place */
} outlines[PIECES] = {
        {b_outer, 9, 0.0f},  {b_hole1, 6, 0.0f}, {b_hole2, 6, 0.0f},  {m_outer, 12, 6.5f},
        {d_outer, 6, 13.0f}, {d_hole, 6, 13.0f}, {s_outer, 16, 19.5f},
};

static struct {
                bmds_3d_mesh_t pieces[PIECES];
                float          stars[STARS * 3];   /* camera space, rushing */
                float          targets[STARS * 3]; /* on the letters' edges, the letters' space */
                float          shown[STARS * 3];
                uint32_t       palette[PAL_PARTS];
                uint32_t       logo[RAMP];
                float          yaw, pitch; /* the pointer's turn */
                int32_t        drag_x, drag_y;
} s;

static float frand(float lo, float hi) {
        return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static void new_star(float *p, int far) {
        p[0] = frand(-30.0f, 30.0f);
        p[1] = frand(-18.0f, 18.0f);
        p[2] = far ? -80.0f : frand(-80.0f, -2.0f);
        return;
}

static int init(demo_t *demo) {
        (void)demo;
        /* The letters, centred: 24.5 units across, 7 high, 1.2 deep. */
        float xy[2 * 16];
        for (int i = 0; i < PIECES; ++i) {
                for (uint32_t k = 0; k < outlines[i].points; ++k) {
                        xy[2 * k] = outlines[i].xy[2 * k] + outlines[i].x - 12.25f;
                        xy[2 * k + 1] = outlines[i].xy[2 * k + 1] - 3.5f;
                }
                const int err = bmds_3d_mesh_extrude(xy, outlines[i].points, 1.2f, 0, &s.pieces[i]);
                if (err < 0) return err;
        }
        /* The stars' places on the letters: along the edges, as many a unit as there are to go. */
        float total = 0.0f;
        for (int i = 0; i < PIECES; ++i)
                for (uint32_t e = 0; e < s.pieces[i].edge_count; ++e) {
                        const float *a = s.pieces[i].positions + 3 * s.pieces[i].edges[2 * e];
                        const float *b = s.pieces[i].positions + 3 * s.pieces[i].edges[2 * e + 1];
                        total += bmds_vec3_length(bmds_vec3(b[0] - a[0], b[1] - a[1], b[2] - a[2]));
                }
        int n = 0;
        for (int i = 0; i < PIECES && n < STARS; ++i)
                for (uint32_t e = 0; e < s.pieces[i].edge_count && n < STARS; ++e) {
                        const float *a = s.pieces[i].positions + 3 * s.pieces[i].edges[2 * e];
                        const float *b = s.pieces[i].positions + 3 * s.pieces[i].edges[2 * e + 1];
                        const float  len = bmds_vec3_length(bmds_vec3(b[0] - a[0], b[1] - a[1], b[2] - a[2]));
                        const int    k = 1 + (int)(len / total * STARS);
                        for (int j = 0; j < k && n < STARS; ++j, ++n) {
                                const float f = ((float)j + 0.5f) / (float)k;
                                for (int c = 0; c < 3; ++c) s.targets[3 * n + c] = a[c] + (b[c] - a[c]) * f;
                        }
                }
        for (; n < STARS; ++n) memcpy(s.targets + 3 * n, s.targets + 3 * (n % 64), 3 * sizeof(float));
        for (int i = 0; i < STARS; ++i) new_star(s.stars + 3 * i, 0);

        s.palette[0] = 0x000000;
        bmds_palette_ramp(s.palette, RAMP_STARS, RAMP, 0x303848, 0xffffff);
        bmds_palette_ramp3(s.logo, 0, RAMP, 0x001018, 0x30c0ff, 0xe0ffff);
        bmds_palette_ramp(s.palette, RAMP_LOGO, RAMP, 0x000000, 0x000000); /* faded in by frame() */
        return 0;
}

static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.5f);
        s.yaw = s.pitch = 0.0f;
        for (int i = 0; i < STARS; ++i) new_star(s.stars + 3 * i, 0);
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type != BMDS_EVENT_POINTER) return;
        const int32_t x = (int32_t)e->data[0], y = (int32_t)e->data[1];
        const int     down = (e->data[2] & 1u) != 0 && x != BMDS_POINTER_GONE;
        if (down && !demo->button) pal_flash(&demo->pal, 0x80c0ff, 0.6f, 3.0f);
        if (down && demo->button) {
                s.yaw += (float)(x - s.drag_x) * 0.01f;
                s.pitch += (float)(y - s.drag_y) * 0.01f;
        }
        s.drag_x = x, s.drag_y = y;
        return;
}

/* 0 → 1 smoothly over [a, b]. */
static float ease(float t, float a, float b) {
        if (t <= a) return 0.0f;
        if (t >= b) return 1.0f;
        const float u = (t - a) / (b - a);
        return u * u * (3.0f - 2.0f * u);
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        const bmds_rect_t whole = {0, 0, DEMO_WIDTH, DEMO_HEIGHT};
        bmds_game_dma_fill(demo->game, &whole, 1, 0);
        bmds_3d_t *ctx = demo->ctx;
        bmds_3d_set_target(ctx, c);
        bmds_3d_begin(ctx);

        /* The stars rush (slower while they gather). */
        const float gather = ease(t, GATHER_START, GATHER_END) * (1.0f - ease(t, LET_GO, LET_GO + 1.0f));
        const float speed = 25.0f * (1.0f - 0.85f * gather);
        for (int i = 0; i < STARS; ++i) {
                float *p = s.stars + 3 * i;
                p[2] += speed * dt;
                if (p[2] > -1.0f) new_star(p, 1);
        }
        /* The letters' turn: swinging once they are there (always readable), plus the pointer's. */
        const float spin = t > GATHER_END ? 0.8f * dsin((t - GATHER_END) * 0.9f) : 0.0f;
        bmds_mat4_t model, turn, place;
        bmds_mat4_rotation_y(&model, spin + s.yaw);
        bmds_mat4_rotation_x(&turn, 0.25f * dsin(t * 0.7f) + s.pitch);
        bmds_mat4_multiply(&model, &turn, &model);
        bmds_mat4_scaling(&turn, 0.42f, 0.42f, 0.42f);
        bmds_mat4_multiply(&model, &model, &turn);
        bmds_mat4_translation(&place, 0.0f, 0.0f, -9.0f);
        bmds_mat4_multiply(&model, &place, &model);

        /* Each star between where it is and its place on the letters. */
        for (int i = 0; i < STARS; ++i) {
                const float      *p = s.stars + 3 * i;
                const bmds_vec3_t q = bmds_mat4_transform_point(&model, bmds_vec3(s.targets[3 * i], s.targets[3 * i + 1],
                                                                                  s.targets[3 * i + 2]));
                s.shown[3 * i] = p[0] + (q.x - p[0]) * gather;
                s.shown[3 * i + 1] = p[1] + (q.y - p[1]) * gather;
                s.shown[3 * i + 2] = p[2] + (q.z - p[2]) * gather;
        }
        bmds_mat4_t identity;
        bmds_mat4_identity(&identity);
        bmds_3d_set_view(ctx, &identity);
        bmds_3d_load(ctx, &identity);
        bmds_3d_set_fog(ctx, 2.0f, 55.0f);
        bmds_3d_material_t m;
        memset(&m, 0, sizeof(m));
        m.size = sizeof(m);
        m.mode = BMDS_3D_POINTS;
        m.flags = BMDS_3D_DEPTH_CUE;
        m.ramp_first = RAMP_STARS;
        m.ramp_count = RAMP;
        m.point_size = gather > 0.3f ? 2 : 1; /* bigger while they draw the letters */
        (void)bmds_3d_points(ctx, s.shown, STARS, &m);
        m.point_size = 0;

        /* The letters in wire, faded in as the stars arrive (their ramp's colours scaled). */
        const float show = ease(t, GATHER_END - 1.0f, GATHER_END + 0.5f);
        if (show > 0.0f) {
                uint32_t ramp[RAMP];
                bmds_palette_mix_rgb(ramp, s.logo, RAMP, 0x000000, (uint32_t)(256.0f * (1.0f - show)));
                pal_set_entries(&demo->pal, RAMP_LOGO, RAMP, ramp);
                bmds_3d_load(ctx, &model);
                bmds_3d_set_fog(ctx, 6.0f, 13.0f);
                m.mode = BMDS_3D_WIRE;
                m.flags = BMDS_3D_DEPTH_CUE | BMDS_3D_CULL_BACK;
                m.ramp_first = RAMP_LOGO;
                for (int i = 0; i < PIECES; ++i) (void)bmds_3d_draw(ctx, &s.pieces[i], &m);
        }
        bmds_3d_end(ctx);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_stars = {"Stars into letters", init, enter, frame, event};
