/**
 * @file      hive.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The bestagons (devplan/demo.md §3): hexagons, three ways over the hive's sixteen bars.
 *
 * - Bars 0-4, a honeycomb all palette: a picture of hexagonal cells made once (faces, bevels lit
 *   from the top left and shaded, gaps), each cell one of COLOURS colours at random (each colour
 *   three entries: face, lit and shaded bevel), copied into the frame by the 2D-DMA. Only the
 *   palette moves: on each beat one colour, never the last one, flares bright and eases back over
 *   about a second. The colours themselves are picked at random each run.
 * - Bars 4-12, pillars: 37 hexagonal prisms (10 triangles each: the top and the three sides that
 *   can face the camera) seen from straight above, their
 *   tops the honeycomb again; the camera tilts and circles, the pillars rise in waves, the bass
 *   lifting the middle, each ring a beat's kick. Flat lit, a ramp a ring. Each prism's bounds are
 *   centred on its base, so the draws' order (by their centres) is by place on the ground: back to
 *   front for things standing on a plane, whatever their heights.
 * - Bars 12-16, a Goldberg ball: the dual of a twice-split icosahedron, 150 hexagons and 12
 *   pentagons over a dark core. Its tiles fly in and fit together, then pop out in a band that
 *   travels round it on the beat, and fly off at the end. Coloured by distance from the pentagons.
 *
 * The honeycomb stays behind the 3D, dimmed. The pointer turns the camera; a click flashes the
 * palette and jolts the pillars or bursts the tiles.
 */

#include <math.h>
#include <string.h>
#include <bmds/3d.h>
#include <bmds/palette.h>
#include "../demo.h"

#define HIVE_SLOT 3 /* the 2D-DMA's image slot (the copper's 0, the balls' 1, the boot's 2) */

/* The honeycomb's cells: pointy-topped, CELL pixels from the middle to a corner. */
#define CELL 22.0f
#define SQRT3 1.7320508f
#define COLOURS 8

/* The palette. */
#define INK_GAP 0                       /* between the cells */
#define INK_FACE 1                      /* COLOURS each: a cell's face, */
#define INK_LIGHT (INK_FACE + COLOURS)  /* its lit bevel */
#define INK_DARK (INK_LIGHT + COLOURS)  /* and its shaded one */
#define RAMP 30                         /* each 3D colour's lit ramp */
#define INK_RAMPS (INK_DARK + COLOURS)  /* three of RAMP */
#define INK_CORE (INK_RAMPS + 3 * RAMP) /* CORE: the ball's core */
#define CORE 8
_Static_assert(INK_CORE + CORE <= PAL_PARTS, "the palette");

/* The pillars: a hexagon of RINGS rings round one, a unit apart. */
#define RINGS 3 /* 37 pillars: four rings (61) cost 13-16 ms a frame in the show, three 9-11 */
#define PILLARS (1 + 3 * RINGS * (RINGS + 1))
#define PILLAR_RADIUS 0.9f /* to a corner; the cells' centres SQRT3 apart */

/* The ball. */
#define TILES 162
#define TILE_CORNERS 6

static const uint32_t ramp_rgb[3] = {0xffb020, 0x20c8d8, 0xe048c8}; /* honey, teal, magenta */

typedef struct {
                float    centre[3]; /* on the unit sphere: its normal too */
                float    corner[TILE_CORNERS][3];
                uint32_t corners; /* 5 or 6 */
                uint32_t group;   /* its colour: pentagons 0, then by distance from them */
                float    delay;   /* bars late flying in */
} tile_t;

static struct {
                uint8_t          *picture; /* the honeycomb, DEMO_WIDTH × DEMO_HEIGHT */
                bmds_game_image_t image;
                uint32_t          palette[PAL_PARTS], shown[INK_RAMPS];
                uint32_t          seed; /* the cells' colours, this run */
                float             hue[COLOURS], sat[COLOURS], val[COLOURS];
                float             flare[COLOURS]; /* 1 on its beat, easing to 0 */
                int32_t           beat;           /* the last beat seen (-1: none) */
                uint32_t          lit;            /* the colour that flared last */
                /* The pillars: six meshes (by the sides facing the camera), placed PILLARS times. */
                float          prism_pos[6][18 * 3], prism_nrm[6][18 * 3];
                uint16_t       prism_tri[6][10 * 3];
                bmds_3d_mesh_t prism[6];          /* by the side facing the camera most */
                float          side_normal[6][2]; /* x, z */
                float          eye[3];
                float          pillar_xz[PILLARS][2];
                uint32_t       pillar_ring[PILLARS];
                float          jolt;
                /* The ball: its tiles, their three meshes rewritten each frame, and its core. */
                tile_t         tiles[TILES];
                float          ball_pos[3][TILES * TILE_CORNERS * 3], ball_nrm[3][TILES * TILE_CORNERS * 3];
                uint16_t       ball_tri[3][TILES * (TILE_CORNERS - 2) * 3];
                bmds_3d_mesh_t ball[3];
                bmds_3d_mesh_t core;
                float          burst;
} s;

/* ---- The honeycomb ---------------------------------------------------------------------------- */

/* The cell (axial q, r) at pixel offset (x, y) from the middle, and the offset from its centre. */
static void cell_at(float x, float y, int32_t *q, int32_t *r, float *dx, float *dy) {
        const float fq = (SQRT3 / 3.0f * x - y / 3.0f) / CELL, fr = (2.0f / 3.0f * y) / CELL, fs = -fq - fr;
        float       rq = roundf(fq), rr = roundf(fr), rs = roundf(fs);
        const float eq = fabsf(rq - fq), er = fabsf(rr - fr), es = fabsf(rs - fs);
        if (eq > er && eq > es)
                rq = -rr - rs;
        else if (er > es)
                rr = -rq - rs;
        *q = (int32_t)rq;
        *r = (int32_t)rr;
        *dx = x - CELL * SQRT3 * (rq + rr / 2.0f);
        *dy = y - CELL * 1.5f * rr;
        return;
}

static uint32_t ring_of(int32_t q, int32_t r) {
        const int32_t s3 = -q - r;
        return (uint32_t)((q < 0 ? -q : q) + (r < 0 ? -r : r) + (s3 < 0 ? -s3 : s3)) / 2u;
}

/* Cell (q, r)'s colour: a hash of it and this run's seed. */
static uint32_t colour_of(int32_t q, int32_t r) {
        uint32_t h = (uint32_t)q * 0x9e3779b1u ^ (uint32_t)r * 0x85ebca77u ^ s.seed;
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        return h % COLOURS;
}

static void make_picture(void) {
        const float inner = CELL * SQRT3 / 2.0f; /* the middle to an edge */
        for (int32_t py = 0; py < DEMO_HEIGHT; ++py) {
                uint8_t *row = s.picture + (size_t)py * DEMO_WIDTH;
                for (int32_t px = 0; px < DEMO_WIDTH; ++px) {
                        int32_t q, r;
                        float   dx, dy;
                        cell_at((float)px + 0.5f - DEMO_WIDTH / 2, (float)py + 0.5f - DEMO_HEIGHT / 2, &q, &r, &dx,
                                &dy);
                        /* How far to the edge (1 on it): pointy-topped, the edges' normals at 0°, ±60°, 180°... */
                        const float    ax = fabsf(dx), ay = fabsf(dy);
                        const float    e = fmaxf(ax, 0.5f * ax + SQRT3 / 2.0f * ay) / inner;
                        const uint32_t colour = colour_of(q, r);
                        uint8_t        ink;
                        if (e > 0.9f) {
                                ink = INK_GAP;
                        } else if (e > 0.72f) {
                                /* The bevel: the nearest edge's normal, lit from the top left. */
                                const float a = atan2f(dy, dx);
                                const float n = roundf(a / 1.0471976f) * 1.0471976f;
                                const float lit = -cosf(n) * 0.6f - sinf(n) * 0.8f;
                                ink = (uint8_t)((lit > 0.0f ? INK_LIGHT : INK_DARK) + colour);
                        } else {
                                ink = (uint8_t)(INK_FACE + colour);
                        }
                        row[px] = ink;
                }
        }
        return;
}

/* The honeycomb's colours: each its own, flared towards bright and pale, @p bright overall. */
static void honeycomb_colours(float bright) {
        for (uint32_t g = 0; g < COLOURS; ++g) {
                const float f = s.flare[g];
                float       v = bright * (s.val[g] + (1.0f - s.val[g]) * f);
                v = v > 1.0f ? 1.0f : v;
                const float sat = s.sat[g] * (1.0f - 0.55f * f);
                const float lv = v * 1.45f + 0.06f;
                s.shown[INK_FACE + g] = bmds_palette_hsv(s.hue[g], sat, v);
                s.shown[INK_LIGHT + g] = bmds_palette_hsv(s.hue[g], sat * 0.6f, lv > 1.0f ? 1.0f : lv);
                s.shown[INK_DARK + g] = bmds_palette_hsv(s.hue[g], sat, v * 0.4f);
        }
        return;
}

/* ---- The pillars ------------------------------------------------------------------------------ */

/* A hexagonal prism, Y from 0 to 1: its top (4 triangles) and the three sides a camera on the
   side of side k's normal can see (2 triangles each), one mesh for each k: nothing drawn faces
   away, and 18 vertices to place, not 30. Each face its own vertices (flat normals). Bounds round
   its base's centre. */
static void make_prisms(void) {
        float cx[6], cz[6];
        for (int k = 0; k < 6; ++k) {
                const float a = 0.5235988f + 1.0471976f * (float)k; /* pointy-topped, as the honeycomb */
                cx[k] = PILLAR_RADIUS * cosf(a);
                cz[k] = PILLAR_RADIUS * sinf(a);
        }
        for (int k = 0; k < 6; ++k) { /* side k: corners k and k + 1, its normal at 60° + 60°k */
                s.side_normal[k][0] = cosf(1.0471976f * (float)(k + 1));
                s.side_normal[k][1] = sinf(1.0471976f * (float)(k + 1));
        }
        for (int view = 0; view < 6; ++view) {
                float    *p = s.prism_pos[view], *n = s.prism_nrm[view];
                uint16_t *t = s.prism_tri[view];
                uint32_t  v = 0, tri = 0;
                for (int k = 0; k < 6; ++k) { /* the top, counter-clockwise from above */
                        p[3 * v] = cx[5 - k], p[3 * v + 1] = 1.0f, p[3 * v + 2] = cz[5 - k];
                        n[3 * v] = 0.0f, n[3 * v + 1] = 1.0f, n[3 * v + 2] = 0.0f;
                        ++v;
                }
                for (int k = 1; k + 1 < 6; ++k) {
                        t[3 * tri] = 0, t[3 * tri + 1] = (uint16_t)k, t[3 * tri + 2] = (uint16_t)(k + 1);
                        ++tri;
                }
                for (int d = -1; d <= 1; ++d) {
                        const int   k = (view + d + 6) % 6, j = (k + 1) % 6;
                        const float corners[4][3] = {
                                {cx[k], 0.0f, cz[k]}, {cx[j], 0.0f, cz[j]}, {cx[j], 1.0f, cz[j]}, {cx[k], 1.0f, cz[k]}};
                        for (int c = 0; c < 4; ++c) {
                                memcpy(p + 3 * (v + (uint32_t)c), corners[c], sizeof(corners[c]));
                                n[3 * (v + (uint32_t)c)] = s.side_normal[k][0], n[3 * (v + (uint32_t)c) + 1] = 0.0f;
                                n[3 * (v + (uint32_t)c) + 2] = s.side_normal[k][1];
                        }
                        /* Counter-clockwise seen from outside. */
                        const uint16_t a = (uint16_t)v;
                        t[3 * tri] = a, t[3 * tri + 1] = (uint16_t)(a + 2), t[3 * tri + 2] = (uint16_t)(a + 1), ++tri;
                        t[3 * tri] = a, t[3 * tri + 1] = (uint16_t)(a + 3), t[3 * tri + 2] = (uint16_t)(a + 2), ++tri;
                        v += 4;
                }
                bmds_3d_mesh_t *m = &s.prism[view];
                memset(m, 0, sizeof(*m));
                m->size = sizeof(*m);
                m->vertex_count = v;
                m->positions = p;
                m->normals = n;
                m->triangle_count = tri;
                m->triangles = t;
                m->bounds[3] = 1.45f; /* from the base's centre: the top's corners are 1.35 away */
        }
        return;
}

static void place_pillars(void) {
        uint32_t i = 0;
        for (int32_t q = -RINGS; q <= RINGS; ++q)
                for (int32_t r = -RINGS; r <= RINGS; ++r) {
                        const uint32_t ring = ring_of(q, r);
                        if (ring > RINGS) continue;
                        s.pillar_xz[i][0] = SQRT3 * ((float)q + (float)r / 2.0f);
                        s.pillar_xz[i][1] = 1.5f * (float)r;
                        s.pillar_ring[i] = ring;
                        ++i;
                }
        return;
}

/* ---- The ball --------------------------------------------------------------------------------- */

static void normalise(float *v) {
        const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        v[0] /= l, v[1] /= l, v[2] /= l;
        return;
}

/* The tiles from an icosphere's vertices (each a tile) and triangles (each a tile's corner). */
static int make_ball(void) {
        bmds_3d_shape_t shape;
        memset(&shape, 0, sizeof(shape));
        shape.size = sizeof(shape);
        shape.kind = BMDS_3D_SHAPE_ICOSPHERE;
        shape.segments = 2;
        shape.radius = 1.0f;
        bmds_3d_mesh_t ico;
        int            err = bmds_3d_mesh_make(&shape, 0, &ico);
        if (err < 0) return err;
        if (ico.vertex_count != TILES) {
                bmds_3d_mesh_free(&ico);
                return -BMDS_EINVAL;
        }
        for (uint32_t v = 0; v < TILES; ++v) {
                tile_t *tl = &s.tiles[v];
                memcpy(tl->centre, ico.positions + 3 * v, sizeof(tl->centre));
                normalise(tl->centre);
                tl->corners = 0;
        }
        /* Each triangle's middle is a corner of its three vertices' tiles. */
        for (uint32_t t = 0; t < ico.triangle_count; ++t) {
                const uint16_t *ix = ico.triangles + 3 * t;
                float           m[3] = {0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 3; ++k)
                        for (int a = 0; a < 3; ++a) m[a] += ico.positions[3 * ix[k] + a];
                normalise(m);
                for (int k = 0; k < 3; ++k) {
                        tile_t *tl = &s.tiles[ix[k]];
                        if (tl->corners < TILE_CORNERS) memcpy(tl->corner[tl->corners++], m, sizeof(m));
                }
        }
        /* Corners round each tile counter-clockwise seen from outside, pulled in for the gaps, and onto
           the tile's plane (so its triangles shade as one). Neighbours: tiles sharing an edge. */
        static uint8_t near[TILES][TILES / 8 + 1];
        memset(near, 0, sizeof(near));
        for (uint32_t t = 0; t < ico.triangle_count; ++t)
                for (int k = 0; k < 3; ++k) {
                        const uint32_t a = ico.triangles[3 * t + k], b = ico.triangles[3 * t + (k + 1) % 3];
                        near[a][b / 8] |= (uint8_t)(1u << (b % 8));
                        near[b][a / 8] |= (uint8_t)(1u << (a % 8));
                }
        bmds_3d_mesh_free(&ico);
        for (uint32_t v = 0; v < TILES; ++v) {
                tile_t      *tl = &s.tiles[v];
                const float *n = tl->centre;
                /* A basis on the tile's plane. */
                float u[3] = {n[1], -n[0], 0.0f};
                if (fabsf(n[2]) > 0.9f) u[0] = 0.0f, u[1] = n[2], u[2] = -n[1];
                normalise(u);
                const float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
                float       angle[TILE_CORNERS];
                for (uint32_t c = 0; c < tl->corners; ++c) {
                        const float *p = tl->corner[c];
                        angle[c] = atan2f(p[0] * w[0] + p[1] * w[1] + p[2] * w[2],
                                          p[0] * u[0] + p[1] * u[1] + p[2] * u[2]);
                }
                for (uint32_t i = 1; i < tl->corners; ++i) /* by angle: few, insertion */
                        for (uint32_t j = i; j > 0 && angle[j - 1] > angle[j]; --j) {
                                const float a = angle[j];
                                angle[j] = angle[j - 1], angle[j - 1] = a;
                                float p[3];
                                memcpy(p, tl->corner[j], sizeof(p));
                                memcpy(tl->corner[j], tl->corner[j - 1], sizeof(p));
                                memcpy(tl->corner[j - 1], p, sizeof(p));
                        }
                for (uint32_t c = 0; c < tl->corners; ++c) {
                        float      *p = tl->corner[c];
                        const float d = (n[0] - p[0]) * n[0] + (n[1] - p[1]) * n[1] + (n[2] - p[2]) * n[2];
                        for (int a = 0; a < 3; ++a) p[a] = n[a] + 0.86f * (p[a] + d * n[a] - n[a]);
                }
                tl->delay = 0.6f * demo_frand();
        }
        /* Colours: the pentagons, then rings round them by distance (a breadth-first walk). */
        uint32_t queue[TILES], head = 0, tail = 0, dist[TILES];
        for (uint32_t v = 0; v < TILES; ++v) {
                dist[v] = s.tiles[v].corners == 5 ? 0u : 0xffu;
                if (dist[v] == 0) queue[tail++] = v;
        }
        while (head < tail) {
                const uint32_t v = queue[head++];
                for (uint32_t o = 0; o < TILES; ++o)
                        if ((near[v][o / 8] >> (o % 8) & 1u) != 0 && dist[o] == 0xffu) {
                                dist[o] = dist[v] + 1;
                                queue[tail++] = o;
                        }
        }
        for (uint32_t v = 0; v < TILES; ++v) s.tiles[v].group = dist[v] == 0 ? 0u : 1u + (dist[v] + 1) % 2u;

        /* The three meshes' triangles and normals, fixed; their positions written each frame. */
        uint32_t verts[3] = {0, 0, 0}, tris[3] = {0, 0, 0};
        for (uint32_t v = 0; v < TILES; ++v) {
                const tile_t  *tl = &s.tiles[v];
                const uint32_t g = tl->group, first = verts[g];
                for (uint32_t c = 0; c < tl->corners; ++c) memcpy(s.ball_nrm[g] + 3 * (first + c), tl->centre, 12);
                for (uint32_t c = 1; c + 1 < tl->corners; ++c) {
                        uint16_t *t = s.ball_tri[g] + 3 * tris[g]++;
                        t[0] = (uint16_t)first, t[1] = (uint16_t)(first + c), t[2] = (uint16_t)(first + c + 1);
                }
                verts[g] += tl->corners;
        }
        for (int g = 0; g < 3; ++g) {
                bmds_3d_mesh_t *m = &s.ball[g];
                memset(m, 0, sizeof(*m));
                m->size = sizeof(*m);
                m->vertex_count = verts[g];
                m->positions = s.ball_pos[g];
                m->normals = s.ball_nrm[g];
                m->triangle_count = tris[g];
                m->triangles = s.ball_tri[g];
                m->bounds[3] = 6.0f; /* round the middle, wide enough for tiles flying */
        }
        shape.segments = 1; /* 80 triangles: the tiles hide most of it */
        shape.radius = 0.9f;
        return bmds_3d_mesh_make(&shape, 0, &s.core);
}

/* Each tile @p push[] out along its normal. */
static void place_tiles(const float *push) {
        uint32_t at[3] = {0, 0, 0};
        for (uint32_t v = 0; v < TILES; ++v) {
                const tile_t *tl = &s.tiles[v];
                float        *p = s.ball_pos[tl->group] + 3 * at[tl->group];
                for (uint32_t c = 0; c < tl->corners; ++c)
                        for (int a = 0; a < 3; ++a) p[3 * c + a] = tl->corner[c][a] + push[v] * tl->centre[a];
                at[tl->group] += tl->corners;
        }
        return;
}

/* ---- The part --------------------------------------------------------------------------------- */

/* Dark to the colour three quarters up, then towards white. */
static void lit_ramp(uint32_t *out, uint32_t n, uint32_t rgb) {
        const uint32_t lit = n * 3 / 4;
        bmds_palette_ramp(out, 0, lit + 1, 0x05050a, rgb);
        bmds_palette_ramp(out, lit, n - lit, rgb, 0xfff4e0);
        return;
}

static int init(demo_t *demo) {
        (void)demo;
        s.picture = (uint8_t *)bmds_mem_alloc(DEMO_WIDTH * DEMO_HEIGHT, 0, 0);
        if (s.picture == NULL) return -BMDS_ENOMEM;
        /* This run's colours: spread round the wheel in a shuffled order, each a little off. */
        s.seed = demo_rand();
        uint32_t order[COLOURS];
        demo_shuffle(order, COLOURS);
        for (uint32_t g = 0; g < COLOURS; ++g) {
                s.hue[g] = ((float)order[g] + 0.4f * demo_frand()) / COLOURS;
                s.sat[g] = 0.6f + 0.3f * demo_frand();
                s.val[g] = 0.38f + 0.2f * demo_frand();
        }
        make_picture();
        s.image = (bmds_game_image_t){s.picture, DEMO_WIDTH, DEMO_WIDTH, DEMO_HEIGHT, BMDS_PIXEL_L8, 0, 0};
        make_prisms();
        place_pillars();
        const int err = make_ball();
        if (err < 0) return err;
        s.palette[INK_GAP] = 0x040408;
        honeycomb_colours(1.0f);
        memcpy(s.palette + INK_FACE, s.shown + INK_FACE, 3 * COLOURS * sizeof(uint32_t));
        for (int i = 0; i < 3; ++i) lit_ramp(s.palette + INK_RAMPS + i * RAMP, RAMP, ramp_rgb[i]);
        bmds_palette_ramp(s.palette, INK_CORE, CORE, 0x020206, 0x302440);
        return 0;
}

static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.4f);
        (void)bmds_game_dma_image(demo->game, HIVE_SLOT, &s.image);
        memset(s.flare, 0, sizeof(s.flare));
        s.beat = -1;
        s.jolt = s.burst = 0.0f;
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER && (e->data[2] & 1u) != 0 && !demo->button) {
                s.jolt = 1.0f;
                s.burst = 1.0f;
                pal_flash(&demo->pal, 0xfff0c0, 0.5f, 3.0f);
        }
        return;
}

/* The camera at @p elevation (radians above the ground) and @p azimuth, @p distance from @p at. */
static void camera(bmds_3d_t *ctx, float elevation, float azimuth, float distance, float at_y) {
        const float ce = dcos(elevation);
        s.eye[0] = distance * ce * dsin(azimuth);
        s.eye[1] = at_y + distance * dsin(elevation);
        s.eye[2] = distance * ce * dcos(azimuth);
        bmds_mat4_t view;
        bmds_mat4_look_at(&view, bmds_vec3(s.eye[0], s.eye[1], s.eye[2]), bmds_vec3(0.0f, at_y, 0.0f),
                          /* Straight down, "up" is along the ground. */
                          elevation > 1.5f ? bmds_vec3(-dsin(azimuth), 0.0f, -dcos(azimuth)) : bmds_vec3(0, 1, 0));
        bmds_3d_set_view(ctx, &view);
        return;
}

static bmds_3d_material_t lit(uint32_t ramp, uint32_t count) {
        bmds_3d_material_t m;
        memset(&m, 0, sizeof(m));
        m.size = sizeof(m);
        m.mode = BMDS_3D_FLAT;
        m.flags = BMDS_3D_CULL_BACK;
        m.ramp_first = ramp;
        m.ramp_count = count;
        return m;
}

static float smooth01(float x) {
        x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        return x * x * (3.0f - 2.0f * x);
}

static void pillars(demo_t *demo, float bars, float t, float dt) {
        const sync_t *y = &demo->sync;
        bmds_3d_t    *ctx = demo->ctx;
        const float   u = bars - 4.0f; /* 0..8 */
        const float   tilt = smooth01(u / 2.0f);
        float         azimuth = 0.3f * u + 0.2f;
        if (demo->pointer_x >= 0) azimuth += 2.0f * (float)demo->pointer_x / DEMO_WIDTH - 1.0f;
        camera(ctx, 1.5707f - tilt * 0.65f, azimuth, 19.0f - 2.0f * tilt - 0.5f * y->kick, 0.5f);
        const bmds_3d_light_t light = {sizeof(light), BMDS_3D_LIGHT_DIRECTIONAL, 0, 0.35f, 1.0f, 0.55f, 0.6f};
        (void)bmds_3d_set_light(ctx, 0, &light);
        bmds_3d_set_ambient(ctx, 0.18f + 0.25f * y->clap);

        /* Rising out of the honeycomb, sinking back in the last bar. */
        const float rise = smooth01((u - 0.5f) / 1.5f) * smooth01(8.0f - u);
        s.jolt -= s.jolt * (dt * 2.5f > 1.0f ? 1.0f : dt * 2.5f);
        const uint32_t kick_ring = y->known ? y->pos.beat % (RINGS + 1) : (uint32_t)(t * 2.0f) % (RINGS + 1);
        for (uint32_t i = 0; i < PILLARS; ++i) {
                const float    x = s.pillar_xz[i][0], z = s.pillar_xz[i][1];
                const float    d = sqrtf(x * x + z * z);
                const float    wave = 0.5f + 0.5f * dsin(d * 0.75f - t * 2.6f + 0.6f * dsin(x * 0.3f + t));
                const uint32_t ring = s.pillar_ring[i];
                float          h = 0.06f + rise * (0.25f + 1.3f * wave * wave + 0.8f * y->bass * (ring < 2) +
                                          0.6f * y->kick * (ring == kick_ring) + 1.0f * s.jolt * wave);
                bmds_mat4_t    place;
                bmds_mat4_translation(&place, x, 0.0f, z);
                bmds_3d_load(ctx, &place);
                bmds_3d_scale(ctx, 1.0f, h, 1.0f);
                /* The sides towards the camera. */
                const float ex = s.eye[0] - x, ez = s.eye[2] - z;
                int         view = 0;
                float       best = -1e30f;
                for (int k = 0; k < 6; ++k) {
                        const float dot = ex * s.side_normal[k][0] + ez * s.side_normal[k][1];
                        if (dot > best) best = dot, view = k;
                }
                const bmds_3d_material_t m = lit(INK_RAMPS + (ring % 3) * RAMP, RAMP);
                (void)bmds_3d_draw(ctx, &s.prism[view], &m);
        }

        return;
}

static void ball(demo_t *demo, float bars, float t, float dt) {
        const sync_t *y = &demo->sync;
        bmds_3d_t    *ctx = demo->ctx;
        const float   u = bars - 12.0f; /* 0..4 */
        float         azimuth = 0.4f * t;
        if (demo->pointer_x >= 0) azimuth += 2.0f * (float)demo->pointer_x / DEMO_WIDTH - 1.0f;
        camera(ctx, 0.35f, azimuth, 4.6f - 0.25f * y->kick, 0.0f);
        const bmds_3d_light_t light = {sizeof(light), BMDS_3D_LIGHT_DIRECTIONAL, BMDS_3D_LIGHT_VIEW, -0.5f, 0.7f, 0.8f,
                                       0.6f};
        (void)bmds_3d_set_light(ctx, 0, &light);
        bmds_3d_set_ambient(ctx, 0.18f + 0.25f * y->clap);

        /* Each tile: flying in (late by its delay), out in the band going round on the beat, a
           click's burst, and off in the last half bar. */
        s.burst -= s.burst * (dt * 2.0f > 1.0f ? 1.0f : dt * 2.0f);
        const float band = (y->known ? (float)y->pos.beat + y->pos.beat_phase : t * 2.33f) * 0.9f;
        const float pulse = y->known ? y->pulse : 0.0f;
        const float off = smooth01((u - 3.5f) * 2.0f);
        float       push[TILES];
        for (uint32_t v = 0; v < TILES; ++v) {
                const tile_t *tl = &s.tiles[v];
                const float   in = 1.0f - smooth01((u - tl->delay) / 0.8f);
                /* The band: a latitude round an axis that turns. */
                const float lat = tl->centre[1] * dcos(t * 0.3f) + tl->centre[0] * dsin(t * 0.3f);
                float       b = lat - dsin(band);
                b = b < 0.0f ? -b : b;
                const float pop = b < 0.25f ? (0.25f - b) * 4.0f : 0.0f;
                push[v] = 5.0f * in * in + 0.35f * pop * (0.4f + pulse) + 0.8f * s.burst * (float)(v % 3) / 2.0f +
                          6.0f * off * (0.5f + tl->delay);
        }
        place_tiles(push);

        bmds_mat4_t place;
        bmds_mat4_identity(&place);
        bmds_3d_load(ctx, &place);
        bmds_3d_rotate(ctx, bmds_vec3(0.2f, 1.0f, 0.1f), t * 0.5f);
        const float grow = smooth01(u * 2.0f) * (1.0f - off);
        bmds_3d_push(ctx);
        bmds_3d_scale(ctx, grow, grow, grow);
        const bmds_3d_material_t core = lit(INK_CORE, CORE);
        (void)bmds_3d_draw(ctx, &s.core, &core);
        (void)bmds_3d_pop(ctx);
        for (uint32_t g = 0; g < 3; ++g) {
                const bmds_3d_material_t m = lit(INK_RAMPS + g * RAMP, RAMP);
                (void)bmds_3d_draw(ctx, &s.ball[g], &m);
        }
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        const sync_t *y = &demo->sync;
        /* Where in the hive's sixteen bars: the music's, or the part's own time round them. */
        float bars = y->known && y->pos.cue != NULL && strcmp(y->pos.cue, "hive") == 0
                             ? (float)y->pos.bar + y->pos.bar_phase
                             : t / y->bar_seconds;
        bars -= 16.0f * floorf(bars / 16.0f);

        /* The honeycomb, behind everything: bright alone, dimmed under the 3D. */
        const bmds_rect_t whole = {0, 0, DEMO_WIDTH, DEMO_HEIGHT};
        bmds_game_dma_copy(demo->game, HIVE_SLOT, &whole, 1);
        /* Each beat a colour flares, never the last one; all ease back (about a second). */
        const int32_t beat = y->known ? (int32_t)(y->pos.bar * 4 + y->pos.beat) : (int32_t)(t * 4.0f / y->bar_seconds);
        if (beat != s.beat) {
                s.beat = beat;
                s.lit = (s.lit + 1 + demo_rand_below(COLOURS - 1)) % COLOURS;
                s.flare[s.lit] = 1.0f;
        }
        const float ease = dt * 2.2f > 1.0f ? 1.0f : dt * 2.2f;
        for (uint32_t g = 0; g < COLOURS; ++g) s.flare[g] -= s.flare[g] * ease;
        const float alone = 1.0f - smooth01((bars - 3.5f) * 2.0f);
        honeycomb_colours(0.3f + 0.7f * alone);
        pal_set_entries(&demo->pal, INK_FACE, 3 * COLOURS, s.shown + INK_FACE);

        if (bars >= 4.0f) {
                bmds_3d_t *ctx = demo->ctx;
                bmds_3d_set_target(ctx, c);
                bmds_3d_begin(ctx);
                if (bars < 12.0f)
                        pillars(demo, bars, t, dt);
                else
                        ball(demo, bars, t, dt);
                bmds_3d_end(ctx);
        }
        if (y->claps != 0) pal_flash(&demo->pal, 0xfff0d0, 0.2f, 5.0f);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_hive = {"Bestagons", init, enter, frame, event};
