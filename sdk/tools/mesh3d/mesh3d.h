/**
 * @file      mesh3d.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-mesh3d's parts (devplan/3d.md §12d): meshes built from OBJ files and Bézier patches, then
 * written as a `.bm3d` model (bmds/3dfile.h). The host tests compile all but main.c.
 */

#ifndef BMDS_MESH3D_H
#define BMDS_MESH3D_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A mesh being built: vertices unique by (position, normal, uv). */
typedef struct m3d_mesh_t {
                char     *name;
                float    *positions, *normals, *uvs; /* 3, 3, 2 a vertex */
                uint32_t  vertex_count, vertex_cap;
                uint16_t *triangles;
                uint8_t  *inner; /* per triangle, a bit per side (s: corner s to s + 1): inside a polygon */
                uint32_t  triangle_count, triangle_cap;
                uint32_t *hash; /* vertex + 1 by slot (0: empty) */
                uint32_t  hash_mask;
                int       has_normals, has_uvs; /* some vertex was given one */
} m3d_mesh_t;

/** @brief The meshes of a model, in file order. */
typedef struct m3d_scene_t {
                m3d_mesh_t *meshes;
                uint32_t    count;
} m3d_scene_t;

/** @brief What the command line asked for. */
typedef struct m3d_options_t {
                float    crease;     /**< Degrees: smooth across faces nearer than this (0: faceted). */
                uint32_t detail;     /**< Patches: an N × N grid each. */
                float    scale;      /**< Every position times this (after `fit`). */
                int      up_z;       /**< The input is Z-up: turned to Y-up. */
                int      fit;        /**< Centred, radius 1 (all meshes together). */
                int      merge;      /**< OBJ: one mesh an input, not one a group. */
                int      no_normals; /**< None written. */
                int      no_uvs;     /**< None written. */
                int      no_edges;   /**< None written. */
} m3d_options_t;

/** @brief The defaults: crease 60°, detail 4, scale 1. */
void m3d_options_init(m3d_options_t *options);

/** @brief A new empty mesh named @p name at the scene's end; NULL with no memory. */
m3d_mesh_t *m3d_scene_add(m3d_scene_t *scene, const char *name);

/** @brief The scene's mesh named @p name, or NULL. */
m3d_mesh_t *m3d_scene_find(m3d_scene_t *scene, const char *name);

/** @brief Frees every mesh and empties @p scene. */
void m3d_scene_free(m3d_scene_t *scene);

/**
 * @brief The index of the vertex (@p p, @p n, @p uv) in @p mesh, added if new (@p n, @p uv may be
 *        NULL: zeros). @return The index, or -1 (65,535 already, or no memory).
 */
int32_t m3d_vertex(m3d_mesh_t *mesh, const float *p, const float *n, const float *uv);

/** @brief Adds a triangle; one with a repeated vertex or no area is dropped. @return 0, or -1 (no memory). */
int m3d_triangle(m3d_mesh_t *mesh, uint32_t a, uint32_t b, uint32_t c);

/**
 * @brief As m3d_triangle(), its sides in @p inner (bit s: from corner s to s + 1) inside a polygon (a
 *        quad's diagonal, a fan's spokes): no edge is written for them.
 */
int m3d_triangle_inner(m3d_mesh_t *mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t inner);

/**
 * @brief Merges @p mesh's vertices at one position (within a millionth of its size) whose uvs are
 *        equal and whose normals are within @p crease degrees, their normals averaged: a seam
 *        between patches made smooth. Drops triangles left without area. @return 0, or -1.
 */
int m3d_weld(m3d_mesh_t *mesh, float crease);

/**
 * @brief Reads Wavefront OBJ text into @p scene (@p name for faces before any `o` or `g`).
 * @return 0, or -1 with a message in @p err.
 */
int m3d_read_obj(const char *text, size_t length, const char *name, const m3d_options_t *options, m3d_scene_t *scene,
                 char *err, size_t err_size);

/**
 * @brief Reads Bézier patches (`.bpt`: a count, then each patch's `3 3` and 16 points, rows of u)
 *        into one mesh named @p name, tessellated `detail` × `detail` a patch.
 * @return 0, or -1 with a message in @p err.
 */
int m3d_read_bpt(const char *text, size_t length, const char *name, const m3d_options_t *options, m3d_scene_t *scene,
                 char *err, size_t err_size);

/** @brief Turns Z-up to Y-up, fits, scales, merges and drops what @p options say. @return 0, or -1. */
int m3d_finish(m3d_scene_t *scene, const m3d_options_t *options, char *err, size_t err_size);

/**
 * @brief The `.bm3d` file of @p scene (edges and bounds by libbmds3d), in a buffer from malloc().
 * @return 0, or -1 with a message in @p err.
 */
int m3d_write(const m3d_scene_t *scene, const m3d_options_t *options, uint8_t **out, size_t *out_size, char *err,
              size_t err_size);

#ifdef __cplusplus
}
#endif

#endif // BMDS_MESH3D_H
