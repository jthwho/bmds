/**
 * @file      3d_internal.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's own declarations (devplan/3d.md).
 */

#ifndef BMDS_3D_INTERNAL_H
#define BMDS_3D_INTERNAL_H

#include <stddef.h>
#include <bmds/3d.h>

/** @brief The shades a shaded RGB draw steps through (an L8 one: its ramp's length). */
#define BMDS_3D_RGB_SHADES 32u

/** @brief A transformed vertex: clip space, view space (for culling, light and fog), its shade. */
typedef struct bmds_3d_vertex_t {
                float    cx, cy, cz, cw;
                float    vx, vy, vz;
                float    s;      /* solids: the shade, 0..the draw's top */
                float    sx, sy; /* on the screen (when in front: cw > 0) */
                int32_t  fx, fy; /* the same in 28.4 (when inside the guard band: oc 0) */
                uint32_t oc;     /* the planes it is outside (BMDS_3D_OUT_...) */
} bmds_3d_vertex_t;

/** @brief `bmds_3d_vertex_t::oc`'s planes: near, far, and the guard band's sides. */
#define BMDS_3D_OUT_NEAR 0x1u
#define BMDS_3D_OUT_FAR 0x2u
#define BMDS_3D_OUT_DEPTH (BMDS_3D_OUT_NEAR | BMDS_3D_OUT_FAR)

/** @brief The side planes widened: |x|, |y| up to this × w stay in 28.4 without overflow. */
#define BMDS_3D_GUARD 4.0f

/** @brief The viewport as floats, for projecting. */
typedef struct bmds_3d_viewport_t {
                float x, y, w, h;
} bmds_3d_viewport_t;

/** @brief A solid's triangle queued: on the screen in 28.4 fixed point, shades in 8.8. */
typedef struct bmds_3d_tri_t {
                int32_t  x[3], y[3];
                int16_t  s[3];
                uint16_t draw; /* its record in the context's draws */
} bmds_3d_tri_t;

/** @brief What a queued draw's triangles are drawn with. */
typedef struct bmds_3d_record_t {
                uint32_t first;                      /* L8: the value of shade 0 (the ramp's first, or the pixel) */
                uint32_t top;                        /* the highest shade (0: one, unshaded) */
                uint8_t  smooth;                     /* Gouraud: shades between the vertices' */
                uint32_t key;                        /* the draw's depth key (its centre), for BMDS_3D_ORDER_DRAWS */
                uint32_t rank; /* its place far to near among the queued draws (bmds_3d_flush()) */
                uint32_t shades[BMDS_3D_RGB_SHADES]; /* RGB565, BGR24: each shade's pixel */
                /* Textured modes (bmds_3d_triangle_textured()). */
                uint8_t        textured;    /* 1: drawn from the texture */
                uint8_t        perspective; /* BMDS_3D_PERSPECTIVE */
                const uint8_t *texels;      /* the texture's first texel */
                const uint8_t *table;       /* the light table (top 31), or NULL: texels straight */
                uint32_t       umask;       /* width - 1 */
                uint32_t       vmask;       /* (height - 1) << log2(stride) */
                uint32_t       vshift;      /* 16 - log2(stride): v (16.16) >> it, masked, is the row's offset */
                float          tw, th;      /* the texture's size, for the uvs' wrapping */
                float          itw, ith;    /* 1 / tw, 1 / th (exact: powers of two) */
} bmds_3d_record_t;

/** @brief A textured triangle's texture coordinates, in texels, and 1 / w (1: affine), as its `bmds_3d_tri_t`'s. */
typedef struct bmds_3d_tuv_t {
                float u[3], v[3], q[3];
} bmds_3d_tuv_t;

/** @brief The material's size before `texture` was appended: what older sources fill. */
#define BMDS_3D_MATERIAL_SIZE_1 ((uint32_t)offsetof(bmds_3d_material_t, texture))

/** @brief Whether @p mode is a textured one. */
static inline int bmds_3d_is_textured(uint32_t mode) {
        return mode >= BMDS_3D_TEXTURED && mode <= BMDS_3D_ENVMAP;
}

/** @brief A light in view space, for the draw at hand. */
typedef struct bmds_3d_view_light_t {
                uint32_t kind;
                float    x, y, z; /* a unit direction towards the light, or a position */
                float    intensity;
} bmds_3d_view_light_t;

/** @brief The rectangle drawing stays in: the viewport within the canvas's clip, as floats and ints. */
typedef struct bmds_3d_area_t {
                float   x0, y0, x1, y1; /* x1, y1 excluded */
                int32_t ix0, iy0, ix1, iy1;
} bmds_3d_area_t;

/** @brief What a draw call writes with: a pixel, or shades along a ramp by distance. */
typedef struct bmds_3d_ink_t {
                uint32_t shades[256]; /* the pixel of each shade, dark to bright */
                uint32_t count;       /* 1: unshaded (shades[0]) */
                float    fog_near, fog_scale;
} bmds_3d_ink_t;

/** @brief The pixels a draw call touched, for the canvas's damage once at its end. */
typedef struct bmds_3d_bounds_t {
                int32_t x0, y0, x1, y1; /* empty while x1 <= x0 */
} bmds_3d_bounds_t;

/** @brief A context's helper on the other core (config `cores` 2): it draws a band of the queue. */
typedef struct bmds_3d_helper_t bmds_3d_helper_t;

struct bmds_3d_t {
                bmds_canvas_t    *target;
                int32_t           vx, vy, vw, vh;
                int               viewport_whole; /* the viewport follows the target's size */
                bmds_mat4_t       projection, view;
                bmds_mat4_t       model[BMDS_3D_STACK];
                uint32_t          depth; /* the model stack's top */
                float             fog_near, fog_far;
                uint32_t          max_vertices;
                bmds_3d_vertex_t *vertices;
                bmds_3d_stats_t   stats;
                /* Lights. */
                bmds_3d_light_t lights[BMDS_3D_LIGHTS];
                float           ambient;
                /* The solids' queue (bmds_3d_end() draws it). */
                bmds_3d_tri_t    *tris;
                uint32_t         *keys, *keys2; /* the sort's: depth keys, and its scratch */
                uint16_t         *order, *order2;
                uint32_t          max_tris, tri_count;
                bmds_3d_record_t *records;
                uint32_t          max_records, record_count;
                uint16_t         *ranked;     /* the records far to near, for the sort (max_records) */
                uint32_t          sort_order; /* BMDS_3D_ORDER_... */
                bmds_3d_tuv_t    *tuvs;       /* the textured triangles' coordinates (max_tris; the first
                                                 textured draw's) */
                float            *envuv;      /* ENVMAP with normals: each vertex's u, v (max_vertices × 2; the
                                                 first such draw's), worked out once a draw */
                bmds_3d_helper_t *helper;     /* NULL: one core */
                float             share;      /* with a helper: the top band's part of the rows, 0..1 */
};

/** @brief The rectangle @p ctx draws in; 0 when there is none (no target, an empty viewport). */
int bmds_3d_area(const bmds_3d_t *ctx, bmds_3d_area_t *area);

/** @brief Sets @p mvp and @p mv for the model matrix's current top. */
void bmds_3d_matrices(const bmds_3d_t *ctx, bmds_mat4_t *mvp, bmds_mat4_t *mv);

/** @brief @p ctx's viewport as floats (the whole target's when it follows it). */
bmds_3d_viewport_t bmds_3d_viewport(const bmds_3d_t *ctx);

/** @brief Sets @p v's outcode and, where it can, its screen position (after bmds_3d_transform_vertex()). */
void bmds_3d_screen(const bmds_3d_viewport_t *vp, bmds_3d_vertex_t *v);

/** @brief Rounds @p pixels to 28.4 fixed point. */
static inline int32_t bmds_3d_fixed(float pixels) {
        const float v = pixels * 16.0f;
        return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/** @brief Transforms one point by @p mvp and @p mv into @p out. */
void bmds_3d_transform_vertex(const bmds_mat4_t *mvp, const bmds_mat4_t *mv, const float *p, bmds_3d_vertex_t *out);

/** @brief Sets @p ink for @p material on @p ctx's target. @return 0, or `-BMDS_EINVAL` (a bad ramp). */
int bmds_3d_ink(const bmds_3d_t *ctx, const bmds_3d_material_t *material, bmds_3d_ink_t *ink);

/** @brief Checks @p material's fields this version knows. @return 0, or `-BMDS_EINVAL`. */
int bmds_3d_check_material(const bmds_3d_material_t *material);

/**
 * @brief Checks a textured @p material's texture (L8, powers of two) and that it has the light
 *        table its lighting needs. @return 0, or `-BMDS_EINVAL`.
 */
int bmds_3d_check_texture(const bmds_3d_material_t *material);

/** @brief Copies @p material (checked) into @p out, the fields its `size` leaves out zero. */
void bmds_3d_full_material(const bmds_3d_material_t *material, bmds_3d_material_t *out);

/** @brief Draws the segment @p a - @p b (clipped in 3D, then to @p area). @return 1 if any of it was drawn. */
int bmds_3d_segment(bmds_3d_t *ctx, const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, const bmds_3d_vertex_t *a,
                    const bmds_3d_vertex_t *b, bmds_3d_bounds_t *bounds);

/** @brief Draws the point @p v, @p size pixels square. @return 1 if drawn. */
int bmds_3d_point(bmds_3d_t *ctx, const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, const bmds_3d_vertex_t *v,
                  int32_t size, bmds_3d_bounds_t *bounds);

/** @brief Queues @p mesh's triangles as a solid (its vertices transformed already). @return 0 or an error. */
int bmds_3d_queue_solid(bmds_3d_t *ctx, const bmds_3d_mesh_t *mesh, const bmds_3d_material_t *material,
                        const bmds_mat4_t *mv);

/** @brief Draws the sorted queue's @p n triangles (@p order) inside @p area; @return the pixels. */
uint32_t bmds_3d_raster_queue(bmds_3d_t *ctx, const bmds_3d_area_t *area, const uint16_t *order, uint32_t n,
                              bmds_3d_bounds_t *bounds, uint32_t *drawn);

/** @brief Starts @p ctx's helper. @return 0, or `-BMDS_ENOMEM` (then one core). */
int bmds_3d_helper_start(bmds_3d_t *ctx);

/** @brief Stops @p ctx's helper (waits for it). */
void bmds_3d_helper_stop(bmds_3d_t *ctx);

/**
 * @brief Draws the sorted queue on two cores: the helper the rows from the split down, the caller
 *        the rest; the split moved towards equal times. @return the pixels.
 */
uint32_t bmds_3d_helper_raster(bmds_3d_t *ctx, const bmds_3d_area_t *area, const uint16_t *order, uint32_t n,
                               bmds_3d_bounds_t *bounds, uint32_t *drawn);

/** @brief Draws what is queued, far to near, and empties the queue. */
void bmds_3d_flush(bmds_3d_t *ctx);

/** @brief Draws @p tri with @p record inside @p area of @p canvas. @return the pixels written. */
uint32_t bmds_3d_triangle(bmds_canvas_t *canvas, const bmds_3d_area_t *area, const bmds_3d_record_t *record,
                          const bmds_3d_tri_t *tri, bmds_3d_bounds_t *bounds);

/**
 * @brief Draws the textured @p tri (coordinates @p uv) with @p record inside @p area of @p canvas
 *        (L8): the same pixels bmds_3d_triangle() fills. @return the pixels written.
 */
uint32_t bmds_3d_triangle_textured(bmds_canvas_t *canvas, const bmds_3d_area_t *area, const bmds_3d_record_t *record,
                                   const bmds_3d_tri_t *tri, const bmds_3d_tuv_t *uv, bmds_3d_bounds_t *bounds);

/* ---- Triangles' edges (raster.c, texture.c) ---------------------------------------------------- */

/* An edge on successive rows: its x at the row's centre in 28.4 scaled by 4096 (so 1 is 1/65536 of
   a pixel), stepped by a whole number a row. The slope comes from one float division; stepping it
   adds integers only, so the x a row has is the same whichever row the stepping began on, and two
   triangles sharing an edge (both stepping it top to bottom, from the same two ends) get the same
   columns: no gap, no overlap.

   32 bits: inside the guard band (BMDS_3D_GUARD) an edge's x on any row it covers is within
   ±32768 pixels (2^31 here) for viewports up to 13,000 pixels wide; only a step past its last row
   (an edge under a pixel tall, its step up to 16 × its width) may wrap, unsigned, and isn't used. */
typedef struct bmds_3d_edge_t {
                uint32_t x, step;
} bmds_3d_edge_t;

/* The edge (x0, y0)-(x1, y1), y1 > y0, at row @p row (its centre at row × 16 + 8). Inside the guard
   band |dx| / dy × 4096 fits 32 bits. */
static inline void bmds_3d_edge_init(bmds_3d_edge_t *e, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t row) {
        const float   f = (float)(x1 - x0) * 4096.0f / (float)(y1 - y0);
        const int32_t slope = f > 2.0e9f ? 2000000000 : f < -2.0e9f ? -2000000000 : (int32_t)f;
        e->x = (uint32_t)((int64_t)x0 * 4096 + (int64_t)(row * 16 + 8 - y0) * slope);
        e->step = (uint32_t)slope * 16u;
        return;
}

static inline void bmds_3d_edge_step(bmds_3d_edge_t *e) {
        e->x += e->step;
        return;
}

/* The first pixel column whose centre is at or right of the edge: ceil((x − 8) / 16) in 28.4. */
static inline int32_t bmds_3d_edge_column(const bmds_3d_edge_t *e) {
        return (int32_t)(e->x + 32767u) >> 16; /* an arithmetic shift: floor */
}

/* The first row whose centre is at or below @p y (28.4). */
static inline int32_t bmds_3d_first_row(int32_t y) {
        return (y - 8 + 15) >> 4; /* an arithmetic shift: floor */
}

/** @brief Adds the rectangle (x0, y0)-(x1, y1) to @p bounds. */
void bmds_3d_include(bmds_3d_bounds_t *bounds, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

/** @brief Adds @p bounds to @p ctx's target's damage. */
void bmds_3d_damage(bmds_3d_t *ctx, const bmds_3d_bounds_t *bounds);

#endif // BMDS_3D_INTERNAL_H
