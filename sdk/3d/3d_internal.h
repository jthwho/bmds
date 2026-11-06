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

#include <bmds/3d.h>

/** @brief The shades a shaded RGB draw steps through (an L8 one: its ramp's length). */
#define BMDS_3D_RGB_SHADES 32u

/** @brief A transformed vertex: clip space, and view space (for culling and fog). */
typedef struct bmds_3d_vertex_t {
                float cx, cy, cz, cw;
                float vx, vy, vz;
} bmds_3d_vertex_t;

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
};

/** @brief The rectangle @p ctx draws in; 0 when there is none (no target, an empty viewport). */
int bmds_3d_area(const bmds_3d_t *ctx, bmds_3d_area_t *area);

/** @brief Sets @p mvp and @p mv for the model matrix's current top. */
void bmds_3d_matrices(const bmds_3d_t *ctx, bmds_mat4_t *mvp, bmds_mat4_t *mv);

/** @brief Transforms one point by @p mvp and @p mv into @p out. */
void bmds_3d_transform_vertex(const bmds_mat4_t *mvp, const bmds_mat4_t *mv, const float *p, bmds_3d_vertex_t *out);

/** @brief Sets @p ink for @p material on @p ctx's target. @return 0, or `-BMDS_EINVAL` (a bad ramp). */
int bmds_3d_ink(const bmds_3d_t *ctx, const bmds_3d_material_t *material, bmds_3d_ink_t *ink);

/** @brief Checks @p material's fields this version knows. @return 0, or `-BMDS_EINVAL`. */
int bmds_3d_check_material(const bmds_3d_material_t *material);

/** @brief Draws the segment @p a - @p b (clipped in 3D, then to @p area). @return 1 if any of it was drawn. */
int bmds_3d_segment(bmds_3d_t *ctx, const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, const bmds_3d_vertex_t *a,
                    const bmds_3d_vertex_t *b, bmds_3d_bounds_t *bounds);

/** @brief Draws the point @p v, @p size pixels square. @return 1 if drawn. */
int bmds_3d_point(bmds_3d_t *ctx, const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, const bmds_3d_vertex_t *v,
                  int32_t size, bmds_3d_bounds_t *bounds);

/** @brief Adds @p bounds to @p ctx's target's damage. */
void bmds_3d_damage(bmds_3d_t *ctx, const bmds_3d_bounds_t *bounds);

#endif // BMDS_3D_INTERNAL_H
