/**
 * @file      model.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's models (devplan/3d.md §12d): `.bm3d` files (bmds/3dfile.h) read in one go and
 * checked as if hostile, then meshes pointed into their memory. Nothing is converted or copied.
 */

#include <string.h>
#include <bmds/3dfile.h>
#include "3d_internal.h"

struct bmds_3d_model_t {
                void           *owned;  /* the file's bytes when loaded from a path; NULL: the caller's */
                uint32_t        count;  /* meshes */
                bmds_3d_mesh_t *meshes; /* after the record, in its allocation */
                const char    **names;  /* after the meshes: one a mesh, NULL for none */
};

#define MAX_FILE_BYTES 0x4000000u /* 64 MB: bigger than any memory a program has */

/* @p count elements of @p elem bytes at @p offset in a section of @p bytes, after its fixed part
   of @p fixed bytes: inside it and 4-aligned. */
static int array_ok(uint32_t offset, uint32_t count, uint32_t elem, uint32_t fixed, uint32_t bytes) {
        if (offset < fixed || offset > bytes || (offset & 3u) != 0) return 0;
        return (uint64_t)count * elem <= bytes - offset;
}

/* Whether @p count floats at @p p are all finite (no NaN or infinity: an exponent of all ones). */
static int all_finite(const uint8_t *p, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
                uint32_t bits;
                memcpy(&bits, p + 4 * i, sizeof(bits));
                if ((bits & 0x7f800000u) == 0x7f800000u) return 0;
        }
        return 1;
}

/* Fills @p mesh from the mesh section at @p s (@p bytes long, 4-aligned), checking everything the
   drawing code trusts. */
static int read_mesh(const uint8_t *s, uint32_t bytes, bmds_3d_mesh_t *mesh) {
        bmds_3d_file_mesh_t h;
        if (bytes < sizeof(h)) return -BMDS_EILSEQ;
        memcpy(&h, s, sizeof(h));
        if (h.size < sizeof(h) || h.size > bytes || (h.size & 3u) != 0) return -BMDS_EILSEQ;
        if (h.flags != 0) return -BMDS_ENOTSUP;
        if (h.vertex_count == 0 || h.vertex_count > 65535u) return -BMDS_EILSEQ;
        const uint32_t v = h.vertex_count, t = h.triangle_count, e = h.edge_count;
        if (!array_ok(h.positions, v, 12, h.size, bytes)) return -BMDS_EILSEQ;
        if (h.normals != 0 && !array_ok(h.normals, v, 12, h.size, bytes)) return -BMDS_EILSEQ;
        if (h.uvs != 0 && !array_ok(h.uvs, v, 8, h.size, bytes)) return -BMDS_EILSEQ;
        if ((t != 0 || h.triangles != 0) && !array_ok(h.triangles, t, 6, h.size, bytes)) return -BMDS_EILSEQ;
        if ((e != 0 || h.edges != 0) && !array_ok(h.edges, e, 4, h.size, bytes)) return -BMDS_EILSEQ;
        if (h.edge_faces != 0 && !array_ok(h.edge_faces, e, 4, h.size, bytes)) return -BMDS_EILSEQ;
        /* Every number finite (normals and uvs feed texel addresses), the radius not below 0. */
        if (!all_finite((const uint8_t *)h.bounds, 4) || !(h.bounds[3] >= 0.0f)) return -BMDS_EILSEQ;
        if (!all_finite(s + h.positions, 3 * v)) return -BMDS_EILSEQ;
        if (h.normals != 0 && !all_finite(s + h.normals, 3 * v)) return -BMDS_EILSEQ;
        if (h.uvs != 0 && !all_finite(s + h.uvs, 2 * v)) return -BMDS_EILSEQ;

        const uint16_t *tri = (const uint16_t *)(const void *)(s + h.triangles);
        for (uint32_t i = 0; i < 3 * t; ++i)
                if (tri[i] >= v) return -BMDS_EILSEQ;
        const uint16_t *edges = (const uint16_t *)(const void *)(s + h.edges);
        for (uint32_t i = 0; i < 2 * e; ++i)
                if (edges[i] >= v) return -BMDS_EILSEQ;
        const uint16_t *faces = h.edge_faces != 0 ? (const uint16_t *)(const void *)(s + h.edge_faces) : NULL;
        for (uint32_t i = 0; faces != NULL && i < 2 * e; ++i)
                if (faces[i] >= t && faces[i] != BMDS_3D_NO_FACE) return -BMDS_EILSEQ;

        memset(mesh, 0, sizeof(*mesh));
        mesh->size = sizeof(*mesh);
        mesh->vertex_count = v;
        mesh->positions = (const float *)(const void *)(s + h.positions);
        mesh->normals = h.normals != 0 ? (const float *)(const void *)(s + h.normals) : NULL;
        mesh->uvs = h.uvs != 0 ? (const float *)(const void *)(s + h.uvs) : NULL;
        mesh->triangle_count = t;
        mesh->triangles = t != 0 ? tri : NULL;
        mesh->edge_count = e;
        mesh->edges = e != 0 ? edges : NULL;
        mesh->edge_faces = e != 0 ? faces : NULL;
        memcpy(mesh->bounds, h.bounds, sizeof(mesh->bounds));
        return 0;
}

/* Gives the meshes of @p model the names in the names section at @p s (@p bytes long). */
static int read_names(const uint8_t *s, uint32_t bytes, bmds_3d_model_t *model) {
        bmds_3d_file_names_t h;
        if (bytes < sizeof(h)) return -BMDS_EILSEQ;
        memcpy(&h, s, sizeof(h));
        if (h.size < sizeof(h) || h.size > bytes || (h.size & 3u) != 0) return -BMDS_EILSEQ;
        if (!array_ok(h.size, h.count, sizeof(bmds_3d_file_name_t), h.size, bytes)) return -BMDS_EILSEQ;
        for (uint32_t i = 0; i < h.count; ++i) {
                bmds_3d_file_name_t n;
                memcpy(&n, s + h.size + i * sizeof(n), sizeof(n));
                if (n.name < h.size || n.name >= bytes || memchr(s + n.name, 0, bytes - n.name) == NULL)
                        return -BMDS_EILSEQ;
                /* Names of sections this library doesn't keep (or that aren't there) are skipped. */
                if (n.type == BMDS_3D_SECTION_MESH && n.index < model->count)
                        model->names[n.index] = (const char *)(s + n.name);
        }
        return 0;
}

/* Reads the model in @p data (4-aligned, @p bytes long) into a new record. */
static int open_model(const uint8_t *data, uint32_t bytes, bmds_3d_model_t **out) {
        bmds_3d_file_header_t h;
        if (bytes < sizeof(h)) return -BMDS_EILSEQ;
        memcpy(&h, data, sizeof(h));
        if (memcmp(h.magic, BMDS_3D_FILE_MAGIC, 4) != 0) return -BMDS_EILSEQ;
        if (h.version == 0) return -BMDS_EILSEQ;
        if (h.version > BMDS_3D_FILE_VERSION || h.flags != 0) return -BMDS_ENOTSUP;
        if (h.header_bytes < sizeof(h) || (h.header_bytes & 3u) != 0 || h.file_bytes > bytes ||
            h.file_bytes < h.header_bytes)
                return -BMDS_EILSEQ;
        const uint32_t size = h.file_bytes;
        if (h.section_count > (size - h.header_bytes) / sizeof(bmds_3d_file_section_t)) return -BMDS_EILSEQ;

        /* The table: every section inside the file, required ones known; meshes counted. */
        const uint8_t *table = data + h.header_bytes;
        uint32_t       meshes = 0;
        for (uint32_t i = 0; i < h.section_count; ++i) {
                bmds_3d_file_section_t sec;
                memcpy(&sec, table + i * sizeof(sec), sizeof(sec));
                if ((sec.offset & 3u) != 0 || sec.offset > size || sec.bytes > size - sec.offset) return -BMDS_EILSEQ;
                if (sec.type == BMDS_3D_SECTION_MESH)
                        ++meshes;
                else if (sec.type != BMDS_3D_SECTION_NAMES && (sec.flags & BMDS_3D_SECTION_REQUIRED) != 0)
                        return -BMDS_ENOTSUP;
        }

        const size_t     record = sizeof(bmds_3d_model_t) + meshes * (sizeof(bmds_3d_mesh_t) + sizeof(const char *));
        bmds_3d_model_t *model = (bmds_3d_model_t *)bmds_mem_alloc((uint32_t)record, 0, 0);
        if (model == NULL) return -BMDS_ENOMEM;
        memset(model, 0, record);
        model->count = meshes;
        model->meshes = (bmds_3d_mesh_t *)(void *)(model + 1);
        model->names = (const char **)(void *)(model->meshes + meshes);

        int      err = 0;
        uint32_t m = 0;
        for (uint32_t i = 0; err == 0 && i < h.section_count; ++i) {
                bmds_3d_file_section_t sec;
                memcpy(&sec, table + i * sizeof(sec), sizeof(sec));
                if (sec.type == BMDS_3D_SECTION_MESH)
                        err = read_mesh(data + sec.offset, sec.bytes, &model->meshes[m++]);
        }
        /* Names after the meshes, so a names section anywhere in the table can name them. */
        for (uint32_t i = 0; err == 0 && i < h.section_count; ++i) {
                bmds_3d_file_section_t sec;
                memcpy(&sec, table + i * sizeof(sec), sizeof(sec));
                if (sec.type == BMDS_3D_SECTION_NAMES) err = read_names(data + sec.offset, sec.bytes, model);
        }
        if (err != 0) {
                bmds_mem_free(model);
                return err;
        }
        *out = model;
        return 0;
}

int bmds_3d_model_load_memory(const void *data, uint32_t bytes, bmds_3d_model_t **model) {
        if (model == NULL) return -BMDS_EINVAL;
        *model = NULL;
        if (data == NULL || ((uintptr_t)data & 3u) != 0) return -BMDS_EINVAL;
        return open_model((const uint8_t *)data, bytes, model);
}

int bmds_3d_model_load(const char *path, uint32_t mem_flags, bmds_3d_model_t **model) {
        if (model == NULL) return -BMDS_EINVAL;
        *model = NULL;
        if (path == NULL) return -BMDS_EINVAL;
        const int32_t fd = bmds_open(path, BMDS_O_READ);
        if (fd < 0) return fd;
        bmds_stat_t st;
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        int32_t  err = bmds_fstat(fd, &st);
        uint8_t *file = NULL;
        if (err == 0 && (st.file_size < sizeof(bmds_3d_file_header_t) || st.file_size > MAX_FILE_BYTES))
                err = -BMDS_EILSEQ;
        if (err == 0 && (file = (uint8_t *)bmds_mem_alloc((uint32_t)st.file_size, 0, mem_flags)) == NULL)
                err = -BMDS_ENOMEM;
        uint32_t got = 0;
        while (err == 0 && got < st.file_size) {
                const int32_t n = bmds_read(fd, file + got, (uint32_t)st.file_size - got);
                if (n <= 0)
                        err = n < 0 ? n : -BMDS_EIO;
                else
                        got += (uint32_t)n;
        }
        bmds_close(fd);
        if (err == 0) err = open_model(file, got, model);
        if (err != 0) {
                bmds_mem_free(file);
                return err;
        }
        (*model)->owned = file;
        return 0;
}

uint32_t bmds_3d_model_mesh_count(const bmds_3d_model_t *model) {
        return model != NULL ? model->count : 0;
}

const bmds_3d_mesh_t *bmds_3d_model_mesh(const bmds_3d_model_t *model, uint32_t index) {
        return model != NULL && index < model->count ? &model->meshes[index] : NULL;
}

const char *bmds_3d_model_mesh_name(const bmds_3d_model_t *model, uint32_t index) {
        return model != NULL && index < model->count ? model->names[index] : NULL;
}

int32_t bmds_3d_model_find(const bmds_3d_model_t *model, const char *name) {
        if (model == NULL || name == NULL) return -BMDS_ENOENT;
        for (uint32_t i = 0; i < model->count; ++i)
                if (model->names[i] != NULL && strcmp(model->names[i], name) == 0) return (int32_t)i;
        return -BMDS_ENOENT;
}

void bmds_3d_model_free(bmds_3d_model_t *model) {
        if (model == NULL) return;
        bmds_mem_free(model->owned);
        bmds_mem_free(model);
        return;
}
