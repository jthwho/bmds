/**
 * @file      balls.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 6 (devplan/demo.md §3): vector balls over a checkered floor. 216 shaded balls take one
 * shape after another (a cube of 6³, a sphere, a torus, a double helix, a rippling sheet), easing
 * from each to the next on every second bar; libbmds3d projects them (bmds_3d_project()), sorts
 * them far to near (bmds_3d_sort_depth(), from last frame's order: nearly sorted), and each is
 * drawn as a sprite of its size: a shaded disc made at start in 16 sizes, its values added to its
 * colour's ramp, a span a row. The floor is 2D: each row below the horizon is one distance, its
 * checks stepped across in fixed point, four pixels a store; the sky is a still the 2D-DMA copies. On the beat
 * (sync_t): the kick bounces the shape, the hats ripple the sheet, each bar's change is on its
 * downbeat. The pointer turns the shape; a click blows the balls apart and they come back.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/3d.h>
#include <bmds/palette.h>
#include "../demo.h"

#define BALLS 216
#define SHAPES 5
#define SIZES 16 /* sprite radii 2..17 */
#define MIN_RADIUS 2
#define SHADE 24 /* a ramp's entries */
#define COLOURS 4
#define RAMP_FIRST 1 /* COLOURS ramps of SHADE */
#define FLOOR_FIRST (RAMP_FIRST + COLOURS * SHADE)
#define FLOOR_SHADES 32 /* two checks' colours each, near to far */
#define SKY_FIRST (FLOOR_FIRST + 2 * FLOOR_SHADES)
#define HORIZON (DEMO_HEIGHT / 2 + 20)
#define PI 3.14159265f
#define SKY_SLOT 1 /* the 2D-DMA's image slot (the copper's is 0) */

typedef struct {
                int8_t   size;   /* 2r + 1 */
                uint8_t *values; /* size², 0: none, else the shade + 1 */
                /* Each row's span (left > right: none). */
                int8_t left[2 * (MIN_RADIUS + SIZES) + 1], right[2 * (MIN_RADIUS + SIZES) + 1];
} sprite_t;

static struct {
                float               shapes[SHAPES][BALLS * 3];
                float               now[BALLS * 3];
                float               scatter[BALLS * 3]; /* each ball's way out when blown apart */
                bmds_3d_projected_t projected[BALLS];
                uint16_t            order[BALLS];
                sprite_t            sprites[SIZES];
                uint32_t            palette[PAL_PARTS];
                float               blow; /* 0..1, how far apart */
                float               yaw, pitch;
                int32_t             drag_x, drag_y;
                float               floor_z;
                bmds_canvas_t      *canvas; /* this frame's, for the floor's bands */
                bmds_game_image_t   sky;    /* the rows above the horizon, a still picture the 2D-DMA copies */
} s;

static float frand(float lo, float hi) {
        return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static void make_shapes(void) {
        for (int i = 0; i < BALLS; ++i) {
                float *p;
                /* A cube of 6 a side. */
                p = s.shapes[0] + 3 * i;
                p[0] = (float)(i % 6) - 2.5f, p[1] = (float)(i / 6 % 6) - 2.5f, p[2] = (float)(i / 36) - 2.5f;
                for (int k = 0; k < 3; ++k) p[k] *= 0.62f;
                /* A sphere: the golden spiral. */
                p = s.shapes[1] + 3 * i;
                const float z = 1.0f - 2.0f * ((float)i + 0.5f) / BALLS, r = sqrtf(1.0f - z * z);
                const float a = (float)i * 2.39996323f;
                p[0] = 2.6f * r * cosf(a), p[1] = 2.6f * z, p[2] = 2.6f * r * sinf(a);
                /* A torus: 18 round, 12 round the tube. */
                p = s.shapes[2] + 3 * i;
                const float u = 2.0f * PI * (float)(i % 18) / 18.0f, v = 2.0f * PI * (float)(i / 18) / 12.0f;
                p[0] = (2.2f + 0.9f * cosf(v)) * cosf(u), p[1] = 0.9f * sinf(v),
                p[2] = (2.2f + 0.9f * cosf(v)) * sinf(u);
                /* A double helix: two strands of 108. */
                p = s.shapes[3] + 3 * i;
                const float h = (float)(i / 2) / 108.0f, w = h * 4.0f * PI + (float)(i & 1) * PI;
                p[0] = 1.5f * cosf(w), p[1] = 6.0f * h - 3.0f, p[2] = 1.5f * sinf(w);
                /* A sheet of 18 by 12 (rippled in frame()). */
                p = s.shapes[4] + 3 * i;
                p[0] = 0.4f * ((float)(i % 18) - 8.5f), p[1] = 0.0f, p[2] = 0.45f * ((float)(i / 18) - 5.5f);
                /* Blown apart: any way out. */
                const bmds_vec3_t d = bmds_vec3_normalize(bmds_vec3(frand(-1, 1), frand(-1, 1), frand(-1, 1)));
                s.scatter[3 * i] = d.x * frand(4.0f, 9.0f);
                s.scatter[3 * i + 1] = d.y * frand(4.0f, 9.0f);
                s.scatter[3 * i + 2] = d.z * frand(4.0f, 9.0f);
        }
        return;
}

static int init(demo_t *demo) {
        (void)demo;
        make_shapes();
        /* The sprites: discs lit from the top left, a highlight. */
        for (int k = 0; k < SIZES; ++k) {
                const int r = MIN_RADIUS + k, n = 2 * r + 1;
                sprite_t *sp = &s.sprites[k];
                sp->size = (int8_t)n;
                sp->values = malloc((size_t)(n * n));
                if (sp->values == NULL) return -BMDS_ENOMEM;
                for (int y = 0; y < n; ++y)
                        for (int x = 0; x < n; ++x) {
                                const float dx = ((float)x - (float)r) / ((float)r + 0.5f);
                                const float dy = ((float)y - (float)r) / ((float)r + 0.5f);
                                const float d2 = dx * dx + dy * dy;
                                uint8_t     value = 0;
                                if (d2 <= 1.0f) {
                                        const float nz = sqrtf(1.0f - d2);
                                        float       lit = 0.15f + 0.85f * (-0.45f * dx - 0.55f * dy + 0.7f * nz);
                                        const float hl = -0.45f * dx - 0.55f * dy + 0.7f * nz;
                                        if (hl > 0.93f) lit = 1.0f; /* the highlight */
                                        if (lit < 0.0f) lit = 0.0f;
                                        if (lit > 1.0f) lit = 1.0f;
                                        value = (uint8_t)(1 + (int)(lit * (SHADE - 1)));
                                }
                                sp->values[y * n + x] = value;
                        }
        }
        /* Each row's span (a disc: one run a row), so drawing tests no pixel. */
        for (int k = 0; k < SIZES; ++k) {
                sprite_t *sp = &s.sprites[k];
                for (int y = 0; y < sp->size; ++y) {
                        sp->left[y] = sp->size, sp->right[y] = -1;
                        for (int x = 0; x < sp->size; ++x)
                                if (sp->values[y * sp->size + x] != 0) {
                                        if (sp->left[y] > x) sp->left[y] = (int8_t)x;
                                        sp->right[y] = (int8_t)x;
                                }
                }
        }
        for (int i = 0; i < BALLS; ++i) s.order[i] = (uint16_t)i; /* sorted from last frame's on */
        /* The ramps: dark, the colour, white. */
        static const uint32_t colours[COLOURS] = {0xff3070, 0x30c0ff, 0xffd030, 0x60ff80};
        s.palette[0] = 0x000000;
        for (int i = 0; i < COLOURS; ++i)
                bmds_palette_ramp3(s.palette, RAMP_FIRST + (uint32_t)i * SHADE, SHADE, 0x0c0410, colours[i], 0xffffff);
        /* The floor: each distance's two checks, fading to the horizon's haze. */
        for (uint32_t d = 0; d < FLOOR_SHADES; ++d) {
                const float    f = (float)d / (FLOOR_SHADES - 1);
                const uint32_t haze = 0x281040;
                uint32_t       mix[2] = {0x6030a0, 0x100820};
                bmds_palette_mix_rgb(mix, mix, 2, haze, (uint32_t)(256.0f * f));
                s.palette[FLOOR_FIRST + 2 * d] = mix[0];
                s.palette[FLOOR_FIRST + 2 * d + 1] = mix[1];
        }
        /* The sky: black at the top to the haze at the horizon (four rows an entry), a still picture. */
        bmds_palette_ramp(s.palette, SKY_FIRST, HORIZON / 4, 0x000000, 0x281040);
        uint8_t *sky = bmds_mem_alloc(DEMO_WIDTH * HORIZON, 64, 0); /* the 2D-DMA reads it */
        if (sky == NULL) return -BMDS_ENOMEM;
        for (int32_t y = 0; y < HORIZON; ++y) memset(sky + (size_t)y * DEMO_WIDTH, SKY_FIRST + y / 4, DEMO_WIDTH);
        s.sky = (bmds_game_image_t){sky, DEMO_WIDTH, DEMO_WIDTH, HORIZON, BMDS_PIXEL_L8, 0, 0};
        return 0;
}

static void enter(demo_t *demo) {
        (void)bmds_game_dma_image(demo->game, SKY_SLOT, &s.sky);
        pal_set_base(&demo->pal, s.palette, 0.5f);
        s.blow = 0.0f;
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type != BMDS_EVENT_POINTER) return;
        const int32_t x = (int32_t)e->data[0], y = (int32_t)e->data[1];
        const int     down = (e->data[2] & 1u) != 0 && x != BMDS_POINTER_GONE;
        if (down && !demo->button) {
                s.blow = 1.0f;
                pal_flash(&demo->pal, 0xffffff, 0.5f, 3.0f);
        }
        if (down && demo->button) {
                s.yaw += (float)(x - s.drag_x) * 0.01f;
                s.pitch += (float)(y - s.drag_y) * 0.01f;
        }
        s.drag_x = x, s.drag_y = y;
        return;
}

static float smooth(float u) {
        return u <= 0.0f ? 0.0f : u >= 1.0f ? 1.0f : u * u * (3.0f - 2.0f * u);
}

/* The floor's rows [HORIZON + first, HORIZON + end): checks, four pixels a store. */
static void floor_rows(void *arg, int32_t first, int32_t end) {
        (void)arg;
        const bmds_canvas_t *c = s.canvas;
        for (int32_t y = HORIZON + first; y < HORIZON + end; ++y) {
                /* The row's distance: the camera 1.5 up, focal 300 pixels. */
                const float   z = 1.5f * 300.0f / (float)(y - HORIZON + 1);
                const int32_t zc = (int32_t)((z + s.floor_z) * 1.0f);
                int32_t       d = (int32_t)(z * 0.9f);
                if (d > FLOOR_SHADES - 1) d = FLOOR_SHADES - 1;
                const uint8_t base = (uint8_t)(FLOOR_FIRST + 2 * d);
                /* Across: a check a unit, in 16.16 from the left edge. */
                const int32_t  step = (int32_t)(z / 300.0f * 65536.0f);
                int32_t        u = -step * (DEMO_WIDTH / 2) + (64 << 16);
                uint32_t      *out = (uint32_t *)(void *)(c->pixels + (size_t)y * c->stride);
                const uint32_t row_bit = (uint32_t)zc & 1u;
                for (int32_t x = 0; x < DEMO_WIDTH; x += 4) {
                        uint32_t w = 0;
                        for (int32_t k = 0; k < 4; ++k, u += step)
                                w |= (base + ((((uint32_t)u >> 16) & 1u) ^ row_bit)) << (8 * k);
                        *out++ = w;
                }
        }
        return;
}

/* The sky by the 2D-DMA, the floor by checks: every pixel. */
static void draw_back(demo_t *demo, bmds_canvas_t *c, float dt, float speed) {
        const bmds_rect_t sky = {0, 0, DEMO_WIDTH, HORIZON};
        bmds_game_dma_copy(demo->game, SKY_SLOT, &sky, 1);
        s.floor_z += dt * speed;
        s.floor_z -= 2.0f * (float)(int32_t)(s.floor_z / 2.0f);
        s.canvas = c;
        floor_rows(NULL, 0, DEMO_HEIGHT - HORIZON);
        return;
}

static void draw_ball(bmds_canvas_t *c, int32_t cx, int32_t cy, int k, uint32_t base) {
        const sprite_t *sp = &s.sprites[k];
        const int32_t   n = sp->size, x0 = cx - n / 2, y0 = cy - n / 2;
        for (int32_t y = 0; y < n; ++y) {
                const int32_t py = y0 + y;
                if (py < 0 || py >= DEMO_HEIGHT) continue;
                int32_t from = sp->left[y], to = sp->right[y];
                if (from < -x0) from = -x0;
                if (to > DEMO_WIDTH - 1 - x0) to = DEMO_WIDTH - 1 - x0;
                const uint8_t *in = sp->values + y * n;
                uint8_t       *out = c->pixels + (size_t)py * c->stride + x0;
                for (int32_t x = from; x <= to; ++x) out[x] = (uint8_t)(base + in[x]);
        }
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        const sync_t *y = &demo->sync;
        draw_back(demo, c, dt, 4.0f + 6.0f * y->kick);

        /* Which shape, and how far to the next: a change every two bars, eased over the first half bar. */
        const float  bars = y->known ? (float)y->pos.bar + y->pos.bar_phase : t / y->bar_seconds;
        const int    at = (int)(bars / 2.0f);
        const float  into = bars - 2.0f * (float)at;
        const float *from = s.shapes[(at + SHAPES - 1) % SHAPES], *to = s.shapes[at % SHAPES];
        const float  mix = smooth(into * 2.0f);
        s.blow -= s.blow * (dt * 1.2f > 1.0f ? 1.0f : dt * 1.2f);
        const float blow = smooth(s.blow);
        const float bounce = 1.0f + 0.1f * y->kick;
        for (int i = 0; i < BALLS; ++i)
                for (int k = 0; k < 3; ++k) {
                        float v = from[3 * i + k] + (to[3 * i + k] - from[3 * i + k]) * mix;
                        s.now[3 * i + k] = (v + s.scatter[3 * i + k] * blow) * bounce;
                }
        /* The sheet ripples (with the hats). */
        if ((at % SHAPES) == SHAPES - 1 || ((at + SHAPES - 1) % SHAPES) == SHAPES - 1) {
                const float amount = (at % SHAPES) == SHAPES - 1 ? mix : 1.0f - mix;
                for (int i = 0; i < BALLS; ++i) {
                        const float *p = s.now + 3 * i;
                        s.now[3 * i + 1] +=
                                amount * (0.5f + 0.3f * y->hat) * dsin(p[0] * 1.3f + p[2] * 0.9f - t * 4.0f);
                }
        }

        /* Turned, placed, projected, sorted, drawn far to near. */
        bmds_3d_t  *ctx = demo->ctx;
        bmds_mat4_t view, model, turn;
        bmds_mat4_translation(&view, 0.0f, -0.3f, -9.5f);
        bmds_3d_set_view(ctx, &view);
        bmds_mat4_rotation_y(&model, t * 0.6f + s.yaw);
        bmds_mat4_rotation_x(&turn, 0.35f + 0.25f * dsin(t * 0.4f) + s.pitch);
        bmds_mat4_multiply(&model, &turn, &model);
        bmds_3d_set_target(ctx, c);
        bmds_3d_load(ctx, &model);
        bmds_3d_project(ctx, s.now, BALLS, s.projected);
        bmds_3d_sort_depth(s.projected, s.order, BALLS);
        for (int n = 0; n < BALLS; ++n) {
                const int                  i = s.order[n];
                const bmds_3d_projected_t *p = &s.projected[i];
                if (!p->visible) continue;
                int k = (int)(p->scale * 0.28f) - MIN_RADIUS;
                if (k < 0) k = 0;
                if (k > SIZES - 1) k = SIZES - 1;
                const uint32_t colour = (uint32_t)(i * COLOURS / BALLS + at) % COLOURS;
                draw_ball(c, (int32_t)p->x, (int32_t)p->y, k, RAMP_FIRST + colour * SHADE - 1);
        }
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_balls = {"Vector balls", init, enter, frame, event};
