/**
 * @file      3dfile.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `.bm3d` model file (devplan/3d.md §12d): an on-disk format, frozen once shipped (fields
 * and section types are only ever appended). libbmds3d reads it (bmds_3d_model_load());
 * `bmds-mesh3d` (sdk/tools/mesh3d) writes it.
 *
 * Little-endian; floats are IEEE-754 binary32. A header, at `header_bytes` a table of
 * `section_count` sections, and the sections, each starting with its `uint32_t size` (the bytes
 * of its fixed part, which later versions may grow: read the fields you know, skip the rest).
 * Offsets inside a section count from the section's start. Writers align sections to 16 bytes and
 * arrays to 4; readers need 4 for both.
 *
 * Every struct here is made of 4-byte fields, so its layout is the same with any C compiler.
 */

#ifndef BMDS_3DFILE_H
#define BMDS_3DFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The header's magic, as the file's first 4 bytes. */
#define BMDS_3D_FILE_MAGIC "BM3D"
/** @brief The version written. A reader refuses a higher one: additions are new sections and
           appended fields, not versions. */
#define BMDS_3D_FILE_VERSION 1u
/** @brief An index that names nothing (a mesh without a material). */
#define BMDS_3D_FILE_NONE 0xffffffffu

/** @brief The file's first bytes. */
typedef struct bmds_3d_file_header_t {
                char     magic[4];      /**< `BMDS_3D_FILE_MAGIC`. */
                uint16_t version;       /**< `BMDS_3D_FILE_VERSION`. */
                uint16_t header_bytes;  /**< Where the section table starts: at least 24, a multiple of 4. */
                uint32_t file_bytes;    /**< The file's length. */
                uint32_t section_count; /**< Entries in the section table. */
                uint32_t flags;         /**< None yet: a reader refuses bits it doesn't know. */
                uint32_t reserved;      /**< 0. */
} bmds_3d_file_header_t;

/** @brief `bmds_3d_file_section_t::flags`: a reader that doesn't know the type refuses the file
           (without it, an unknown section is skipped). */
#define BMDS_3D_SECTION_REQUIRED 0x1u

/* Section types: numbers never reused. */
#define BMDS_3D_SECTION_MESH 1u     /**< `bmds_3d_file_mesh_t`: one mesh. */
#define BMDS_3D_SECTION_NAMES 2u    /**< `bmds_3d_file_names_t`: names for other sections. */
#define BMDS_3D_SECTION_TEXTURE 3u  /**< Reserved: an image (drafted in devplan/3d.md §12d). */
#define BMDS_3D_SECTION_MATERIAL 4u /**< Reserved: how a mesh is drawn (drafted). */
#define BMDS_3D_SECTION_NODE 5u     /**< Reserved: a transform in a hierarchy. */

/** @brief An entry in the section table. */
typedef struct bmds_3d_file_section_t {
                uint32_t type;   /**< `BMDS_3D_SECTION_...`. */
                uint32_t flags;  /**< `BMDS_3D_SECTION_REQUIRED`. */
                uint32_t offset; /**< From the file's start; a multiple of 4. */
                uint32_t bytes;  /**< The section's length. */
} bmds_3d_file_section_t;

/**
 * @brief A `BMDS_3D_SECTION_MESH`: `bmds_3d_mesh_t`'s arrays, at offsets from the section's start
 *        (4-aligned, inside the section). Triangles are counter-clockwise seen from their front.
 */
typedef struct bmds_3d_file_mesh_t {
                uint32_t size;           /**< `sizeof(bmds_3d_file_mesh_t)` (64). */
                uint32_t flags;          /**< None yet: a reader refuses bits it doesn't know. */
                uint32_t vertex_count;   /**< 1 to 65,535. */
                uint32_t triangle_count; /**< 0 (points only) or more. */
                uint32_t edge_count;     /**< 0: none stored (wire draws the triangles' sides). */
                uint32_t material;       /**< An index among the `MATERIAL` sections; `BMDS_3D_FILE_NONE`. */
                float    bounds[4];      /**< A sphere around it: centre x, y, z and radius (0: unknown). */
                uint32_t positions;      /**< x, y, z floats a vertex. Required. */
                uint32_t normals;        /**< x, y, z floats a vertex, unit length; 0: none. */
                uint32_t uvs;            /**< u, v floats a vertex; 0: none. */
                uint32_t triangles;      /**< Three uint16 vertex indices each; 0 only without triangles. */
                uint32_t edges;          /**< Two uint16 vertex indices each; 0 only without edges. */
                uint32_t edge_faces;     /**< Two uint16 an edge: the triangles beside it, 0xffff for none; 0: none. */
} bmds_3d_file_mesh_t;

/** @brief A `BMDS_3D_SECTION_NAMES`: `count` `bmds_3d_file_name_t` entries follow its fixed part. */
typedef struct bmds_3d_file_names_t {
                uint32_t size;  /**< `sizeof(bmds_3d_file_names_t)` (8). */
                uint32_t count; /**< Entries. */
} bmds_3d_file_names_t;

/** @brief A name: of the @p index'th section of @p type (in table order). */
typedef struct bmds_3d_file_name_t {
                uint32_t type;  /**< `BMDS_3D_SECTION_...`. */
                uint32_t index; /**< Which section of that type. */
                uint32_t name;  /**< The offset of a UTF-8 string, NUL-terminated inside the section. */
} bmds_3d_file_name_t;

#ifdef __cplusplus
}
#endif

#endif // BMDS_3DFILE_H
