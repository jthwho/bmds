/**
 * @file      solids.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 5 (devplan/demo.md §3): lit solids. A Gouraud torus knot turns in the middle, four
 * flat-shaded polyhedra orbit it, over a floor of wire fading into the dark; each is a palette ramp
 * of its own (dark, its colour, white), and the knot's colour moves round the wheel a bar at a
 * time. Sized to the frame (devplan/3d.md step 3: ~1,100 cycles an input triangle): ~800
 * triangles. On the beat (sync_t): the kick pumps the knot and pulls the camera in, the clap lights
 * everything (the ambient), the orbit spins up on the crash. The light comes from where the
 * pointer is; a click jolts the orbit.
 */

#include <string.h>
#include <bmds/3d.h>
#include <bmds/palette.h>
#include "../demo.h"

#define RAMP 40
#define RAMP_KNOT 8
#define ORBITERS 4
#define RAMP_ORBIT (RAMP_KNOT + RAMP) /* four more, RAMP each */
#define FLOOR_FIRST (RAMP_ORBIT + ORBITERS * RAMP)
#define FLOOR_RAMP 24

static const uint32_t orbit_rgb[ORBITERS] = {0xff4838, 0x30d060, 0x3870ff, 0xffc030};

static struct {
                bmds_3d_mesh_t knot, orbiters[ORBITERS], floor;
                uint32_t       palette[PAL_PARTS];
                uint32_t       knot_ramp[RAMP];
                float          spin, spin_speed; /* the orbit's angle, and its jolt */
                float          hue;
} s;

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

/* Dark to the colour three quarters up, then towards white: a highlight. */
static void lit_ramp(uint32_t *out, uint32_t rgb) {
        const uint32_t lit = RAMP * 3 / 4;
        bmds_palette_ramp(out, 0, lit + 1, 0x06060a, rgb);
        bmds_palette_ramp(out, lit, RAMP - lit, rgb, 0xfff4e8);
        return;
}

static int init(demo_t *demo) {
        (void)demo;
        int err = make(&s.knot, BMDS_3D_SHAPE_TORUS_KNOT, 64, 5, 1.5f, 0.3f); /* 640 triangles, half facing away */
        if (err == 0) err = make(&s.orbiters[0], BMDS_3D_SHAPE_CUBE, 0, 0, 0.42f, 0.0f);
        if (err == 0) err = make(&s.orbiters[1], BMDS_3D_SHAPE_ICOSPHERE, 1, 0, 0.5f, 0.0f);
        if (err == 0) err = make(&s.orbiters[2], BMDS_3D_SHAPE_CONE, 8, 1, 0.5f, 0.5f);
        if (err == 0) err = make(&s.orbiters[3], BMDS_3D_SHAPE_SPHERE, 8, 5, 0.5f, 0.0f);
        if (err == 0) err = make(&s.floor, BMDS_3D_SHAPE_GRID, 6, 0, 7.0f, 0.0f);
        if (err < 0) return err;
        s.palette[0] = 0x04040c;
        for (int i = 0; i < ORBITERS; ++i) lit_ramp(s.palette + RAMP_ORBIT + i * RAMP, orbit_rgb[i]);
        lit_ramp(s.palette + RAMP_KNOT, bmds_palette_hsv(0.8f, 0.7f, 1.0f));
        bmds_palette_ramp(s.palette, FLOOR_FIRST, FLOOR_RAMP, 0x04040c, 0x3050a0);
        return 0;
}

static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.5f);
        s.spin_speed = 0.0f;
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER && (e->data[2] & 1u) != 0 && !demo->button) {
                s.spin_speed += 6.0f;
                pal_flash(&demo->pal, 0xffffff, 0.5f, 3.0f);
        }
        return;
}

static bmds_3d_material_t solid(uint32_t mode, uint32_t ramp) {
        bmds_3d_material_t m;
        memset(&m, 0, sizeof(m));
        m.size = sizeof(m);
        m.mode = mode;
        m.flags = BMDS_3D_CULL_BACK;
        m.ramp_first = ramp;
        m.ramp_count = RAMP;
        return m;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        const sync_t     *y = &demo->sync;
        const bmds_rect_t whole = {0, 0, DEMO_WIDTH, DEMO_HEIGHT};
        bmds_game_dma_fill(demo->game, &whole, 1, 0);
        bmds_3d_t *ctx = demo->ctx;
        bmds_3d_set_target(ctx, c);
        bmds_3d_begin(ctx);

        /* The camera: round slowly, pulled in by the kick. */
        const float distance = 7.0f - 0.6f * y->kick;
        const float a = t * 0.15f;
        bmds_mat4_t view;
        bmds_mat4_look_at(&view, bmds_vec3(distance * dsin(a), 2.2f + 0.6f * dsin(t * 0.23f), distance * dcos(a)),
                          bmds_vec3(0, 0.2f, 0), bmds_vec3(0, 1, 0));
        bmds_3d_set_view(ctx, &view);
        /* The light from the pointer (or swinging), the clap lifting the ambient. */
        float lx = -0.4f + 0.5f * dsin(t * 0.5f), ly = 0.6f;
        if (demo->pointer_x >= 0) {
                lx = 2.0f * (float)demo->pointer_x / DEMO_WIDTH - 1.0f;
                ly = 1.0f - 2.0f * (float)demo->pointer_y / DEMO_HEIGHT;
        }
        const bmds_3d_light_t light = {sizeof(light), BMDS_3D_LIGHT_DIRECTIONAL, BMDS_3D_LIGHT_VIEW, lx, ly, 0.8f,
                                       0.75f};
        (void)bmds_3d_set_light(ctx, 0, &light);
        bmds_3d_set_ambient(ctx, 0.2f + 0.35f * y->clap);

        /* The floor: lines first, the solids queued over them. */
        bmds_3d_set_fog(ctx, 3.0f, 15.0f);
        bmds_mat4_t place;
        bmds_mat4_translation(&place, 0.0f, -1.7f, 0.0f);
        bmds_3d_load(ctx, &place);
        bmds_3d_material_t floor;
        memset(&floor, 0, sizeof(floor));
        floor.size = sizeof(floor);
        floor.mode = BMDS_3D_WIRE;
        floor.flags = BMDS_3D_DEPTH_CUE;
        floor.ramp_first = FLOOR_FIRST;
        floor.ramp_count = FLOOR_RAMP;
        (void)bmds_3d_draw(ctx, &s.floor, &floor);

        /* The knot, pumping. */
        const float pump = 1.0f + 0.12f * y->kick;
        bmds_mat4_identity(&place);
        bmds_3d_load(ctx, &place);
        bmds_3d_rotate(ctx, bmds_vec3(1, 0, 0), t * 0.5f);
        bmds_3d_rotate(ctx, bmds_vec3(0, 1, 0), t * 0.31f);
        bmds_3d_scale(ctx, pump, pump, pump);
        const bmds_3d_material_t knot = solid(BMDS_3D_GOURAUD, RAMP_KNOT);
        (void)bmds_3d_draw(ctx, &s.knot, &knot);

        /* The orbit: spun up by a click (or the crash: a new phrase), settling back. */
        s.spin_speed -= s.spin_speed * (dt * 1.5f > 1.0f ? 1.0f : dt * 1.5f);
        s.spin += dt * (0.8f + s.spin_speed);
        for (int i = 0; i < ORBITERS; ++i) {
                const float o = s.spin + (float)i * 1.5707963f;
                bmds_mat4_translation(&place, 3.0f * dcos(o), 0.5f * dsin(o * 2.0f + (float)i), 3.0f * dsin(o));
                bmds_3d_load(ctx, &place);
                bmds_3d_rotate(ctx, bmds_vec3(1, 1, 0.3f), t * (1.0f + 0.3f * (float)i));
                const bmds_3d_material_t m = solid(BMDS_3D_FLAT, RAMP_ORBIT + (uint32_t)i * RAMP);
                (void)bmds_3d_draw(ctx, &s.orbiters[i], &m);
        }
        bmds_3d_end(ctx);

        /* The knot's colour: round the wheel a bar at a time (the music's bar, or the part's time). */
        const float bars = y->known ? (float)y->pos.bar + y->pos.bar_phase : t / y->bar_seconds;
        const float hue = 0.8f + 0.125f * bars;
        if (hue - s.hue > 0.004f || hue < s.hue) {
                s.hue = hue;
                lit_ramp(s.knot_ramp, bmds_palette_hsv(hue - (float)(int32_t)hue, 0.7f, 1.0f));
                pal_set_entries(&demo->pal, RAMP_KNOT, RAMP, s.knot_ramp);
        }
        if (y->known && y->pos.bar % 8 == 0 && y->pos.beat == 0 && y->pos.step == 0) s.spin_speed = 5.0f;
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_solids = {"Solids", init, enter, frame, event};
