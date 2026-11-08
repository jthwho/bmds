/**
 * @file      3d.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d (devplan/3d.md): software 3D drawn on libbmdsgame's canvases. A static library
 * (libbmds3d.a, linked by bmds_add_program and bmds-cc), as libbmdsgame is: no system calls of its
 * own and no ABI promise; a program keeps the version it was built with. Structs the caller fills
 * still begin with `size`, so source written for one version builds against the next.
 *
 * - Math (`bmds_vec3_t`, `bmds_mat4_t`): GL's conventions. Column-major matrices, a right-handed
 *   view space looking down -Z, counter-clockwise front faces, `perspective()` and `look_at()` as
 *   GLU's; angles in radians.
 * - A context (`bmds_3d_t`) draws on one canvas: a viewport, projection and view matrices, a
 *   model matrix stack, fog. Its memory is allocated once, at bmds_3d_create().
 * - Meshes (`bmds_3d_mesh_t`) are plain arrays (static data, or made by bmds_3d_mesh_make());
 *   materials (`bmds_3d_material_t`) say how they are drawn.
 *
 * Colours: an unshaded draw uses `pixel` (the canvas's value, bmds_canvas_pixel()). A shaded one
 * (lit solids, `BMDS_3D_DEPTH_CUE`) uses, on L8, a ramp of palette entries from dark (`ramp_first`)
 * to bright (`ramp_first + ramp_count - 1`); on RGB565 and BGR24, `rgb` in 32 levels from black.
 *
 * Textured solids (`BMDS_3D_TEXTURED`, `_TEXTURED_FLAT`, `_TEXTURED_GOURAUD`, `_ENVMAP`; L8 only for
 * now, `-BMDS_ENOSYS` on RGB565 and BGR24) fetch an L8 `texture`'s texels and light them through a
 * light table (bmds_3d_light_table(): `table[shade][texel]`, Doom's COLORMAP).
 *
 * Solids (`BMDS_3D_FLAT`, `BMDS_3D_GOURAUD`, the textured modes) are queued by bmds_3d_draw() and
 * drawn far to near by bmds_3d_end() (the painter's algorithm: by draw, then by triangle, unless
 * bmds_3d_set_order() says otherwise); points, lines and wire draw at once.
 *
 * Not thread-safe: a context is one thread's. Two contexts may draw on two bands of one canvas.
 */

#ifndef BMDS_3D_H
#define BMDS_3D_H

#include <math.h>
#include <stdint.h>
#include <bmds/game.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Math ------------------------------------------------------------------------------------- */

/** @brief A point or direction. */
typedef struct bmds_vec3_t {
                float x, y, z;
} bmds_vec3_t;

/** @brief A homogeneous point (clip space). */
typedef struct bmds_vec4_t {
                float x, y, z, w;
} bmds_vec4_t;

/** @brief A 4×4 matrix, column-major: `m[column * 4 + row]` (GL's layout). */
typedef struct bmds_mat4_t {
                float m[16];
} bmds_mat4_t;

/** @brief Returns (@p x, @p y, @p z). */
static inline bmds_vec3_t bmds_vec3(float x, float y, float z) {
        bmds_vec3_t v = {x, y, z};
        return v;
}

/** @brief Returns @p a + @p b. */
static inline bmds_vec3_t bmds_vec3_add(bmds_vec3_t a, bmds_vec3_t b) {
        return bmds_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

/** @brief Returns @p a - @p b. */
static inline bmds_vec3_t bmds_vec3_sub(bmds_vec3_t a, bmds_vec3_t b) {
        return bmds_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

/** @brief Returns @p v × @p s. */
static inline bmds_vec3_t bmds_vec3_scale(bmds_vec3_t v, float s) {
        return bmds_vec3(v.x * s, v.y * s, v.z * s);
}

/** @brief Returns @p a · @p b. */
static inline float bmds_vec3_dot(bmds_vec3_t a, bmds_vec3_t b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
}

/** @brief Returns @p a × @p b. */
static inline bmds_vec3_t bmds_vec3_cross(bmds_vec3_t a, bmds_vec3_t b) {
        return bmds_vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/** @brief Returns @p v's length. */
static inline float bmds_vec3_length(bmds_vec3_t v) {
        return sqrtf(bmds_vec3_dot(v, v));
}

/** @brief Returns @p v at length 1 (a zero vector stays zero). */
static inline bmds_vec3_t bmds_vec3_normalize(bmds_vec3_t v) {
        const float l = bmds_vec3_length(v);
        return l > 0.0f ? bmds_vec3_scale(v, 1.0f / l) : v;
}

/** @brief Sets @p m to the identity. */
void bmds_mat4_identity(bmds_mat4_t *m);

/** @brief Sets @p out to @p a × @p b (@p b applied first). @p out may be @p a or @p b. */
void bmds_mat4_multiply(bmds_mat4_t *out, const bmds_mat4_t *a, const bmds_mat4_t *b);

/** @brief Sets @p m to a translation. */
void bmds_mat4_translation(bmds_mat4_t *m, float x, float y, float z);

/** @brief Sets @p m to a scale. */
void bmds_mat4_scaling(bmds_mat4_t *m, float x, float y, float z);

/** @brief Sets @p m to a rotation of @p angle radians about the X axis (Y towards Z). */
void bmds_mat4_rotation_x(bmds_mat4_t *m, float angle);

/** @brief Sets @p m to a rotation about the Y axis (Z towards X). */
void bmds_mat4_rotation_y(bmds_mat4_t *m, float angle);

/** @brief Sets @p m to a rotation about the Z axis (X towards Y). */
void bmds_mat4_rotation_z(bmds_mat4_t *m, float angle);

/** @brief Sets @p m to a rotation of @p angle radians about @p axis (normalized here; zero: identity). */
void bmds_mat4_rotation(bmds_mat4_t *m, bmds_vec3_t axis, float angle);

/**
 * @brief Sets @p m to a perspective projection (GLU's `gluPerspective`): @p fovy the vertical
 *        field of view in radians, @p aspect width over height, @p near and @p far positive
 *        distances (near < far).
 */
void bmds_mat4_perspective(bmds_mat4_t *m, float fovy, float aspect, float near_z, float far_z);

/** @brief Sets @p m to an orthographic projection (GL's `glOrtho`). */
void bmds_mat4_ortho(bmds_mat4_t *m, float left, float right, float bottom, float top, float near_z, float far_z);

/** @brief Sets @p m to a view from @p eye towards @p target, @p up roughly up (GLU's `gluLookAt`). */
void bmds_mat4_look_at(bmds_mat4_t *m, bmds_vec3_t eye, bmds_vec3_t target, bmds_vec3_t up);

/**
 * @brief Sets @p out to the inverse of @p m, an affine matrix (rotation, scale, translation; last
 *        row 0, 0, 0, 1). @p out may be @p m.
 * @return 0, or `-BMDS_EINVAL` (singular: @p out untouched).
 */
int bmds_mat4_invert_affine(bmds_mat4_t *out, const bmds_mat4_t *m);

/** @brief Returns @p m × (@p p, 1) as a homogeneous point. */
bmds_vec4_t bmds_mat4_transform(const bmds_mat4_t *m, bmds_vec3_t p);

/** @brief Returns @p m × (@p p, 1), divided by its w (a point through an affine or projective matrix). */
bmds_vec3_t bmds_mat4_transform_point(const bmds_mat4_t *m, bmds_vec3_t p);

/** @brief Returns @p m × (@p v, 0): a direction (no translation). */
bmds_vec3_t bmds_mat4_transform_vector(const bmds_mat4_t *m, bmds_vec3_t v);

/* ---- Meshes ----------------------------------------------------------------------------------- */

/** @brief `bmds_3d_mesh_t::edge_faces`: no triangle on that side (an open edge). */
#define BMDS_3D_NO_FACE 0xffffu

/**
 * @brief A mesh: plain arrays, read in place (static data works). Up to 65,535 vertices
 *        (uint16 indices). Triangles are counter-clockwise seen from their front.
 */
typedef struct bmds_3d_mesh_t {
                uint32_t        size;           /**< `sizeof(bmds_3d_mesh_t)`. */
                uint32_t        vertex_count;   /**< Vertices in `positions` (and `normals`, `uvs`). */
                const float    *positions;      /**< x, y, z per vertex. */
                const float    *normals;        /**< x, y, z per vertex, unit length; NULL: none. */
                const float    *uvs;            /**< u, v per vertex (0..1, wrapped); NULL: none. */
                uint32_t        triangle_count; /**< Triangles in `triangles`. */
                const uint16_t *triangles;      /**< Three vertex indices each. */
                uint32_t        edge_count;     /**< Edges in `edges` (wireframe); 0: the triangles' sides. */
                const uint16_t *edges;          /**< Two vertex indices each. */
                const uint16_t *edge_faces;     /**< Per edge, the two triangles beside it (`BMDS_3D_NO_FACE`
                                                     for none), for hiding edges of back faces; NULL: none. */
                float           bounds[4];      /**< A sphere around it: centre x, y, z and radius; radius 0:
                                                     unknown (never culled whole). */
                void           *owned;          /**< bmds_3d_mesh_make()'s memory (bmds_3d_mesh_free()); NULL. */
                void           *owned_edges;    /**< bmds_3d_mesh_edges()' memory (bmds_3d_mesh_free()); NULL. */
} bmds_3d_mesh_t;

/** @brief `bmds_3d_shape_t::kind`. */
#define BMDS_3D_SHAPE_CUBE 1u   /**< A cube, side `radius × 2`, centred: 24 vertices, faceted. */
#define BMDS_3D_SHAPE_GRID 2u   /**< A plane in X and Z (normal +Y), `radius × 2` across, `segments`² quads. */
#define BMDS_3D_SHAPE_SPHERE 3u /**< A UV sphere: `segments` around, `rings` from pole to pole. */
#define BMDS_3D_SHAPE_TORUS                                                                                            \
        4u /**< A torus about Y: `radius` to the tube's centre, `radius2` the tube's;
                                     `segments` around, `rings` around the tube. */

#define BMDS_3D_SHAPE_CYLINDER                                                                                         \
        5u /**< About Y: `radius`, `radius2` half its height (0: radius); `segments`
                                       around, `rings` along; capped. */
#define BMDS_3D_SHAPE_CONE                                                                                             \
        6u /**< About Y, its point up: `radius` at the base, `radius2` half its height
                                       (0: radius); capped. */
#define BMDS_3D_SHAPE_ICOSPHERE                                                                                        \
        7u /**< A sphere of even triangles: an icosahedron split `segments` times
                                        (0: 2; at most 5). */
#define BMDS_3D_SHAPE_TORUS_KNOT                                                                                       \
        8u /**< A (p, q) torus knot, `radius` across, its tube `radius2` (0: radius /
                                         8); `segments` along (0: 128), `rings` round the tube (0: 8). */

/** @brief A shape bmds_3d_mesh_make() makes. */
typedef struct bmds_3d_shape_t {
                uint32_t size;     /**< `sizeof(bmds_3d_shape_t)`. */
                uint32_t kind;     /**< `BMDS_3D_SHAPE_...`. */
                uint32_t segments; /**< Around (0: the shape's default). */
                uint32_t rings;    /**< Across (0: the shape's default). */
                float    radius;   /**< The size (0: 1). */
                float    radius2;  /**< The torus's tube (0: radius / 3); the others' as their kinds say. */
                uint32_t flags;    /**< 0. */
                uint32_t p, q;     /**< `BMDS_3D_SHAPE_TORUS_KNOT`'s windings (0, 0: 2, 3). */
} bmds_3d_shape_t;

/**
 * @brief Makes a mesh of @p shape, with normals, uvs, edges (no edges between coplanar faces)
 *        and bounds, in one allocation of `bmds_mem_alloc()` with @p mem_flags.
 * @return 0, or `-BMDS_EINVAL` (an unknown kind or flags, too many vertices), `-BMDS_ENOMEM`.
 */
int bmds_3d_mesh_make(const bmds_3d_shape_t *shape, uint32_t mem_flags, bmds_3d_mesh_t *mesh);

/**
 * @brief Makes a mesh of the simple polygon @p xy (@p count points x, y, either winding, no holes)
 *        extruded along Z from -@p depth / 2 to +@p depth / 2: capped, faceted, edges round the
 *        outline and its sides only. Logos and letters, a piece at a time.
 * @return 0, or `-BMDS_EINVAL` (fewer than 3 points, a polygon that crosses itself), `-BMDS_ENOMEM`.
 */
int bmds_3d_mesh_extrude(const float *xy, uint32_t count, float depth, uint32_t mem_flags, bmds_3d_mesh_t *mesh);

/** @brief Frees what bmds_3d_mesh_make() and bmds_3d_mesh_edges() allocated, and empties @p mesh. */
void bmds_3d_mesh_free(bmds_3d_mesh_t *mesh);

/** @brief bmds_3d_mesh_edges()' flags: no edge between two triangles in one plane at all (a
 *         polygon's triangulation, a cap), not only quads' diagonals. */
#define BMDS_3D_EDGES_COPLANAR 0x1u

/**
 * @brief Gives @p mesh its edges (`edges`, `edge_faces`) from its triangles: each side once
 *        (vertices at the same position are one), but none that is a quad's diagonal (two
 *        triangles in one plane, the edge the longest side of both). The arrays are one
 *        allocation, `owned_edges` (an earlier one freed); the rest of @p mesh stays as it was.
 * @return 0, or `-BMDS_EINVAL` (no triangles, unknown flags), `-BMDS_ENOMEM`.
 */
int bmds_3d_mesh_edges(bmds_3d_mesh_t *mesh, uint32_t flags, uint32_t mem_flags);

/** @brief Sets @p mesh's `bounds` to a sphere around its positions. */
void bmds_3d_mesh_bounds(bmds_3d_mesh_t *mesh);

/* ---- Materials -------------------------------------------------------------------------------- */

/** @brief `bmds_3d_material_t::mode`. */
#define BMDS_3D_POINTS 1u  /**< Each vertex a point, `point_size` pixels square. */
#define BMDS_3D_WIRE 2u    /**< The edges as lines. */
#define BMDS_3D_FLAT 3u    /**< Filled, one shade a triangle, lit by its own facing. */
#define BMDS_3D_GOURAUD 4u /**< Filled, shades between the vertices', lit by the mesh's normals. */
#define BMDS_3D_TEXTURED                                                                                               \
        5u /**< `texture` by the mesh's uvs, unlit: the texel itself, or through `light_table` at full
                light (a remap); with DEPTH_CUE, through `light_table` by distance. L8 only. */
#define BMDS_3D_TEXTURED_FLAT 6u /**< `texture` through `light_table`, lit per triangle (as FLAT). L8 only. */
#define BMDS_3D_TEXTURED_GOURAUD                                                                                       \
        7u /**< `texture` through `light_table`, lit per vertex by the mesh's normals (as GOURAUD).
                L8 only. */
#define BMDS_3D_ENVMAP                                                                                                 \
        8u /**< `texture` by uvs from the view-space normals (a sphere map, "matcap": chrome), unlit as
                TEXTURED is; without normals, each triangle's own. L8 only. */

/** @brief `bmds_3d_material_t::flags`. */
#define BMDS_3D_CULL_BACK 0x1u /**< Back faces not drawn (wire: only edges of a front face). */
#define BMDS_3D_DEPTH_CUE 0x2u /**< Shaded by distance: bright at the fog's near, dark at its far. */
#define BMDS_3D_UNLIT 0x4u     /**< Solids: no lights; `pixel`, or the ramp by distance with DEPTH_CUE. */
#define BMDS_3D_PERSPECTIVE                                                                                            \
        0x8u /**< Textured modes: the texture perspective-correct, exact every 16 pixels and affine
                  between (Quake's way). Without it, affine across each triangle (the PS1's swim). */

/** @brief Shades in a light table (bmds_3d_light_table()): 0 the darkest, 31 the texel's own colour. */
#define BMDS_3D_LIGHT_SHADES 32u

/** @brief A light table's bytes: `table[shade * 256 + texel]`. */
#define BMDS_3D_LIGHT_TABLE_SIZE (BMDS_3D_LIGHT_SHADES * 256u)

/**
 * @brief How a mesh is drawn. Fields were appended (`texture`, `light_table`, step 4): a material
 *        whose `size` ends before them is accepted and drawn as before (they read as NULL).
 */
typedef struct bmds_3d_material_t {
                uint32_t size;       /**< `sizeof(bmds_3d_material_t)`. */
                uint32_t mode;       /**< `BMDS_3D_POINTS`... */
                uint32_t flags;      /**< `BMDS_3D_CULL_BACK`... */
                uint32_t pixel;      /**< Unshaded: the canvas's pixel value. */
                uint32_t rgb;        /**< Shaded, RGB565 and BGR24: the brightest colour, 0xRRGGBB. */
                uint32_t ramp_first; /**< Shaded, L8: the ramp's darkest palette index, */
                uint32_t ramp_count; /**< and its length (1-256; first + count at most 256). */
                uint32_t point_size; /**< `BMDS_3D_POINTS`: pixels square (0: 1). */
                /** Textured modes: an L8 image whose width, height and stride are powers of two (stride
                    at most 65536), wrapped (u, v 0..1 across it, by mask); a canvas wrapped by
                    bmds_canvas_image() works. Read at bmds_3d_end(): keep it until then. Internal
                    RAM (`BMDS_MEM_FAST`) makes its fetches cheaper when it fits. */
                const bmds_game_image_t *texture;
                /** Textured modes: bmds_3d_light_table()'s `BMDS_3D_LIGHT_TABLE_SIZE` bytes, which lit
                    modes and DEPTH_CUE need; NULL: texels straight. Read at bmds_3d_end(). */
                const uint8_t *light_table;
} bmds_3d_material_t;

/**
 * @brief Fills @p table (`BMDS_3D_LIGHT_TABLE_SIZE` bytes) for textured L8 drawing, as Doom's
 *        COLORMAP: `table[shade * 256 + i]` is the palette index nearest (weighted 3, 4, 2 for red,
 *        green, blue, as bmds_game_image_load()) to @p palette[i]'s colour at that shade: @p dark at
 *        shade 0, the colour itself at `BMDS_3D_LIGHT_SHADES - 1`, in even steps between. Only
 *        entries @p first to @p first + @p count - 1 are chosen (the rest reserved: text, a
 *        background). Exact nearest colours, searched in the candidates sorted by brightness;
 *        about 36 ms for 256 entries on the board: build tables at a scene's start, not a frame.
 * @param table   [out] The table.
 * @param palette 256 colours, 0xRRGGBB.
 * @param first   The first candidate entry.
 * @param count   Candidates (1 to 256 - @p first).
 * @param dark    0xRRGGBB at shade 0: black (0), or a fog's colour.
 * @return 0, or `-BMDS_EINVAL` (NULL, an empty or too long range).
 * @nonblocking Thread-safe (no state of its own); allocates nothing (~4 KB of stack).
 */
int bmds_3d_light_table(uint8_t *table, const uint32_t *palette, uint32_t first, uint32_t count, uint32_t dark);

/* ---- Contexts --------------------------------------------------------------------------------- */

/** @brief A drawing context (bmds_3d_create()). */
typedef struct bmds_3d_t bmds_3d_t;

/** @brief bmds_3d_create()'s settings. */
typedef struct bmds_3d_config_t {
                uint32_t size;         /**< `sizeof(bmds_3d_config_t)`. */
                uint32_t max_vertices; /**< The most vertices a mesh drawn has (0: 4096). */
                uint32_t mem_flags;    /**< The vertices' scratch (`BMDS_MEM_FAST`: internal RAM, if it fits; 44
                                            bytes a vertex: ~10% faster solids, but internal RAM is the
                                            kernel's too: 45 KB of it kept a program from going full
                                            screen). The queue is in PSRAM. */
                uint32_t max_triangles; /**< Solids' triangles queued before they are drawn (0: 2048). */
                uint32_t max_draws;     /**< Solids' draws queued before they are drawn (0: 64). */
                uint32_t cores;         /**< 2: a helper thread on the other core draws a band of the solids
                                             (the split follows how busy each core is); 0 or 1: the caller's
                                             thread only. Points, lines and wire stay on the caller's. */
} bmds_3d_config_t;

/**
 * @brief Makes a context: the identity matrices, no target, fog from 1 to 100.
 * @return 0, or `-BMDS_EINVAL`, `-BMDS_ENOMEM`.
 */
int bmds_3d_create(const bmds_3d_config_t *config, bmds_3d_t **out);

/** @brief Frees @p ctx (NULL is ignored). */
void bmds_3d_destroy(bmds_3d_t *ctx);

/**
 * @brief Draws on @p canvas (NULL: nothing) from now on, the viewport the whole canvas. Drawing
 *        stays inside the canvas's clip (a band, for two contexts on two cores).
 */
void bmds_3d_set_target(bmds_3d_t *ctx, bmds_canvas_t *canvas);

/** @brief Where normalized device coordinates (-1..1) land, in the canvas's pixels. */
void bmds_3d_set_viewport(bmds_3d_t *ctx, int32_t x, int32_t y, int32_t width, int32_t height);

/** @brief Sets the projection matrix. */
void bmds_3d_set_projection(bmds_3d_t *ctx, const bmds_mat4_t *m);

/** @brief Sets the view (camera) matrix. */
void bmds_3d_set_view(bmds_3d_t *ctx, const bmds_mat4_t *m);

/** @brief Sets the fog's range for `BMDS_3D_DEPTH_CUE`: view distances, full brightness to dark. */
void bmds_3d_set_fog(bmds_3d_t *ctx, float near_z, float far_z);

/** @brief bmds_3d_set_order()'s orders for the solids queued. */
#define BMDS_3D_ORDER_DRAWS                                                                                            \
        0u                         /**< Each draw's triangles together, the draws far to near by their centres
                                        (separate objects never show through each other): the default. */
#define BMDS_3D_ORDER_TRIANGLES 1u /**< Every triangle by its own depth (objects that pass through each other). */

/**
 * @brief Sets how the solids queued from now on are ordered (`BMDS_3D_ORDER_...`); a change draws
 *        what is queued already first, in its own order. @return 0, or `-BMDS_EINVAL`.
 */
int bmds_3d_set_order(bmds_3d_t *ctx, uint32_t order);

/** @brief Lights a context has. */
#define BMDS_3D_LIGHTS 4u

/** @brief `bmds_3d_light_t::kind`. */
#define BMDS_3D_LIGHT_OFF 0u
#define BMDS_3D_LIGHT_DIRECTIONAL 1u /**< From a direction: (x, y, z) points towards the light. */
#define BMDS_3D_LIGHT_POINT 2u       /**< From a point: (x, y, z) is where it is (no falloff). */

/** @brief `bmds_3d_light_t::flags`: (x, y, z) in view space, moving with the camera (else world space). */
#define BMDS_3D_LIGHT_VIEW 0x1u

/** @brief A light (bmds_3d_set_light()). */
typedef struct bmds_3d_light_t {
                uint32_t size;      /**< `sizeof(bmds_3d_light_t)`. */
                uint32_t kind;      /**< `BMDS_3D_LIGHT_...`. */
                uint32_t flags;     /**< `BMDS_3D_LIGHT_VIEW`, or 0. */
                float    x, y, z;   /**< A direction or a position, as the kind says. */
                float    intensity; /**< 0..1 usually; what a surface facing it gains. */
} bmds_3d_light_t;

/**
 * @brief Sets light @p index (below `BMDS_3D_LIGHTS`); NULL turns it off. A new context has light 0
 *        directional in view space, over the viewer's left shoulder (-0.4, 0.6, 1), at 0.8.
 * @return 0, or `-BMDS_EINVAL`.
 */
int bmds_3d_set_light(bmds_3d_t *ctx, uint32_t index, const bmds_3d_light_t *light);

/** @brief Sets the light every surface has, 0..1 (a new context's: 0.25). */
void bmds_3d_set_ambient(bmds_3d_t *ctx, float ambient);

/** @brief The model matrix stack's depth. */
#define BMDS_3D_STACK 16u

/** @brief Saves the model matrix. @return 0, or `-BMDS_ENOSPC` (`BMDS_3D_STACK` deep already). */
int bmds_3d_push(bmds_3d_t *ctx);

/** @brief Restores the last saved model matrix. @return 0, or `-BMDS_EINVAL` (none saved). */
int bmds_3d_pop(bmds_3d_t *ctx);

/** @brief Sets the model matrix. */
void bmds_3d_load(bmds_3d_t *ctx, const bmds_mat4_t *m);

/** @brief Sets the model matrix to itself × @p m (@p m applied to the mesh first). */
void bmds_3d_mult(bmds_3d_t *ctx, const bmds_mat4_t *m);

/** @brief bmds_3d_mult() by a translation. */
void bmds_3d_translate(bmds_3d_t *ctx, float x, float y, float z);

/** @brief bmds_3d_mult() by a rotation about @p axis. */
void bmds_3d_rotate(bmds_3d_t *ctx, bmds_vec3_t axis, float angle);

/** @brief bmds_3d_mult() by a scale. */
void bmds_3d_scale(bmds_3d_t *ctx, float x, float y, float z);

/** @brief Returns the model matrix. */
const bmds_mat4_t *bmds_3d_model(const bmds_3d_t *ctx);

/* ---- Drawing ---------------------------------------------------------------------------------- */

/** @brief Starts a frame: the counters (bmds_3d_stats()) start again. */
void bmds_3d_begin(bmds_3d_t *ctx);

/** @brief Ends a frame: the solids queued are drawn, far to near, on the target. */
void bmds_3d_end(bmds_3d_t *ctx);

/**
 * @brief Draws @p mesh through the model, view and projection matrices with @p material: points,
 *        lines and wire at once; solids queued for bmds_3d_end() (with the lights as they are now).
 *        A textured solid's texture and light table are read at bmds_3d_end(): keep them till then.
 * @return 0, or `-BMDS_EINVAL` (a bad mesh or material, no target; a textured mode without the
 *         mesh's uvs, a usable texture, or the light table it needs), `-BMDS_ENOSPC` (more vertices
 *         than `max_vertices`), `-BMDS_ENOSYS` (a textured mode on RGB565 or BGR24),
 *         `-BMDS_ENOMEM` (the first textured draw's texture coordinates: `max_triangles` × 36 bytes,
 *         allocated once).
 */
int bmds_3d_draw(bmds_3d_t *ctx, const bmds_3d_mesh_t *mesh, const bmds_3d_material_t *material);

/**
 * @brief Draws lines between @p count points (x, y, z each) through the matrices: pairs (0-1,
 *        2-3...), or a strip (0-1, 1-2...) with @p strip. The material's mode is ignored.
 * @return 0, or `-BMDS_EINVAL`.
 */
int bmds_3d_lines(bmds_3d_t *ctx, const float *points, uint32_t count, int strip, const bmds_3d_material_t *material);

/** @brief Draws @p count points (x, y, z each) as `BMDS_3D_POINTS`. @return 0, or `-BMDS_EINVAL`. */
int bmds_3d_points(bmds_3d_t *ctx, const float *points, uint32_t count, const bmds_3d_material_t *material);

/** @brief A point on the screen (bmds_3d_project()). */
typedef struct bmds_3d_projected_t {
                float    x, y;    /**< The canvas's pixels (sub-pixel). */
                float    depth;   /**< The view distance (larger: farther). */
                float    scale;   /**< Pixels per unit at that distance (a sprite's size). */
                uint32_t visible; /**< 1 inside the view (in front of near, before far, on the viewport). */
} bmds_3d_projected_t;

/** @brief Projects @p count points (x, y, z each) through the matrices into @p out. */
void bmds_3d_project(bmds_3d_t *ctx, const float *points, uint32_t count, bmds_3d_projected_t *out);

/**
 * @brief Sorts @p order (@p count indices into @p points) far to near by `depth`: the order to
 *        draw sprites in. Stable.
 */
void bmds_3d_sort_depth(const bmds_3d_projected_t *points, uint16_t *order, uint32_t count);

/** @brief A context's counters since bmds_3d_begin(). */
typedef struct bmds_3d_stats_t {
                uint32_t size;     /**< Set by the caller: `sizeof(bmds_3d_stats_t)`. */
                uint32_t meshes;   /**< Drawn (not culled whole). */
                uint32_t culled;   /**< Meshes outside the view, by their bounds. */
                uint32_t vertices; /**< Transformed. */
                uint32_t lines;    /**< Drawn (some part on the screen). */
                uint32_t points;   /**< Drawn. */
                uint32_t pixels;   /**< Written. */
                uint32_t triangles; /**< Solids' triangles queued (after clipping). */
                uint32_t backfaces; /**< Solids' triangles not drawn, facing away. */
                uint32_t clipped;   /**< Solids' triangles the view's planes cut. */
                uint32_t drawn;     /**< Solids' triangles drawn (some pixel on the screen). */
                uint32_t flushes;   /**< Times a full queue (or a new target) was drawn before bmds_3d_end(). */
} bmds_3d_stats_t;

/** @brief Copies @p ctx's counters into @p stats (as much as its `size` holds). */
void bmds_3d_stats(const bmds_3d_t *ctx, bmds_3d_stats_t *stats);

/* ---- Models (.bm3d files) --------------------------------------------------------------------- */

/**
 * @brief A loaded model (devplan/3d.md §12d): the meshes of a `.bm3d` file (bmds/3dfile.h), made
 *        from OBJ files or Bézier patches by `bmds-mesh3d` on the PC.
 */
typedef struct bmds_3d_model_t bmds_3d_model_t;

/**
 * @brief Loads the model in the file at @p path: one read into one allocation, then every offset
 *        and index checked before a mesh points into it (a bad file is an error, never a crash).
 * @param path      The file.
 * @param mem_flags Where its arrays go (`BMDS_MEM_FAST`: internal RAM, cheaper to transform from).
 * @param model     [out] The model; bmds_3d_model_free() frees it.
 * @return 0; `-BMDS_EILSEQ` (not a well-formed `.bm3d`), `-BMDS_ENOTSUP` (a later version, or a
 *         section or flag it requires that this library doesn't know), `-BMDS_ENOMEM`, the file's
 *         errors (`-BMDS_ENOENT`...), `-BMDS_EINVAL` (NULL).
 * @blocking Thread-safe (no state of its own).
 */
int bmds_3d_model_load(const char *path, uint32_t mem_flags, bmds_3d_model_t **model);

/**
 * @brief As bmds_3d_model_load(), from @p bytes at @p data used in place (no copy): 4-aligned,
 *        kept unchanged until the model is freed (a model built into the program's read-only data).
 * @return As bmds_3d_model_load(); `-BMDS_EINVAL` also for @p data not 4-aligned.
 * @nonblocking Thread-safe (no state of its own); allocates the model's small record.
 */
int bmds_3d_model_load_memory(const void *data, uint32_t bytes, bmds_3d_model_t **model);

/** @brief The meshes in @p model (0 for NULL). @nonblocking */
uint32_t bmds_3d_model_mesh_count(const bmds_3d_model_t *model);

/**
 * @brief @p model's mesh @p index (in the file's order), to draw; it points into the model's memory
 *        and lives as long as the model. A copy may be given edges (bmds_3d_mesh_edges()) and freed
 *        with bmds_3d_mesh_free(), which frees only those. NULL past the last.
 * @nonblocking
 */
const bmds_3d_mesh_t *bmds_3d_model_mesh(const bmds_3d_model_t *model, uint32_t index);

/** @brief Mesh @p index's name (UTF-8), or NULL (none, or past the last). @nonblocking */
const char *bmds_3d_model_mesh_name(const bmds_3d_model_t *model, uint32_t index);

/** @brief The index of the first mesh named @p name, or `-BMDS_ENOENT`. @nonblocking */
int32_t bmds_3d_model_find(const bmds_3d_model_t *model, const char *name);

/** @brief Frees @p model (NULL is ignored); its meshes go with it. @nonblocking */
void bmds_3d_model_free(bmds_3d_model_t *model);

#ifdef __cplusplus
}
#endif

#endif // BMDS_3D_H
