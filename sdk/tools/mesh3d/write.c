/**
 * @file      write.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-mesh3d's writer: a `.bm3d` model (bmds/3dfile.h, devplan/3d.md §12d). Bounds and edges come
 * from libbmds3d's own code, so a file holds what the board would compute.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/3d.h>
#include <bmds/3dfile.h>
#include "mesh3d.h"

static uint32_t align(uint32_t x, uint32_t a) {
        return (x + a - 1) & ~(a - 1);
}

/* What one mesh's section holds. */
typedef struct {
                bmds_3d_mesh_t      view;  /* the mesh as libbmds3d sees it, with its edges */
                uint16_t           *edges; /* those kept (not inside a polygon), then their faces */
                bmds_3d_file_mesh_t head;
                uint32_t            offset; /* the section's, in the file */
} plan_t;

int m3d_write(const m3d_scene_t *scene, const m3d_options_t *options, uint8_t **out, size_t *out_size, char *err,
              size_t err_size) {
        *out = NULL;
        *out_size = 0;
        plan_t *plan = (plan_t *)calloc(scene->count != 0 ? scene->count : 1, sizeof(plan_t));
        if (plan == NULL) {
                snprintf(err, err_size, "out of memory");
                return -1;
        }
        uint32_t named = 0, strings = 0;
        for (uint32_t i = 0; i < scene->count; ++i)
                if (scene->meshes[i].name[0] != '\0') {
                        ++named;
                        strings += (uint32_t)strlen(scene->meshes[i].name) + 1;
                }
        const uint32_t sections = scene->count + (named != 0 ? 1 : 0);
        const uint32_t table = sizeof(bmds_3d_file_header_t);
        uint32_t       at = align(table + sections * (uint32_t)sizeof(bmds_3d_file_section_t), 16);

        int result = 0;
        for (uint32_t i = 0; result == 0 && i < scene->count; ++i) {
                const m3d_mesh_t *m = &scene->meshes[i];
                plan_t           *p = &plan[i];
                p->view.size = sizeof(p->view);
                p->view.vertex_count = m->vertex_count;
                p->view.positions = m->positions;
                p->view.normals = m->has_normals ? m->normals : NULL;
                p->view.uvs = m->has_uvs ? m->uvs : NULL;
                p->view.triangle_count = m->triangle_count;
                p->view.triangles = m->triangles;
                bmds_3d_mesh_bounds(&p->view);
                if (!options->no_edges && m->triangle_count != 0 && bmds_3d_mesh_edges(&p->view, 0, 0) != 0) {
                        snprintf(err, err_size, "mesh \"%s\": its edges: out of memory", m->name);
                        result = -1;
                        break;
                }
                /* Edges inside a polygon (a patch's quad's diagonal, an OBJ polygon's spokes) go: each
                   edge is its first triangle's side, found there. */
                const uint32_t found = p->view.edge_count;
                p->edges = (uint16_t *)malloc((found != 0 ? found : 1) * 4 * sizeof(uint16_t));
                if (p->edges == NULL) {
                        snprintf(err, err_size, "out of memory");
                        result = -1;
                        break;
                }
                uint32_t kept = 0;
                for (uint32_t e = 0; e < found; ++e) {
                        const uint16_t  a = p->view.edges[2 * e], b = p->view.edges[2 * e + 1];
                        const uint32_t  t = p->view.edge_faces[2 * e];
                        const uint16_t *tri = m->triangles + 3 * t;
                        int             inner = 0;
                        for (int k = 0; k < 3; ++k) {
                                const uint16_t u = tri[k], w = tri[(k + 1) % 3];
                                if (((u == a && w == b) || (u == b && w == a)) && (m->inner[t] >> k & 1u) != 0) inner = 1;
                        }
                        if (inner) continue;
                        p->edges[2 * kept] = a, p->edges[2 * kept + 1] = b;
                        p->edges[2 * found + 2 * kept] = p->view.edge_faces[2 * e];
                        p->edges[2 * found + 2 * kept + 1] = p->view.edge_faces[2 * e + 1];
                        ++kept;
                }
                memmove(p->edges + 2 * kept, p->edges + 2 * found, kept * 2 * sizeof(uint16_t));
                bmds_3d_file_mesh_t *h = &p->head;
                h->size = sizeof(*h);
                h->vertex_count = m->vertex_count;
                h->triangle_count = m->triangle_count;
                h->edge_count = kept;
                h->material = BMDS_3D_FILE_NONE;
                memcpy(h->bounds, p->view.bounds, sizeof(h->bounds));
                uint32_t o = sizeof(*h);
                h->positions = o, o += m->vertex_count * 12;
                if (p->view.normals != NULL) h->normals = o, o += m->vertex_count * 12;
                if (p->view.uvs != NULL) h->uvs = o, o += m->vertex_count * 8;
                if (m->triangle_count != 0) h->triangles = o, o = align(o + m->triangle_count * 6, 4);
                if (h->edge_count != 0) {
                        h->edges = o, o += h->edge_count * 4;
                        h->edge_faces = o, o += h->edge_count * 4;
                }
                p->offset = at;
                at = align(at + o, 16);
        }
        const uint32_t names_at = at;
        if (named != 0) at = align(at + (uint32_t)sizeof(bmds_3d_file_names_t) + named * 12 + strings, 16);

        uint8_t *file = result == 0 ? (uint8_t *)calloc(at, 1) : NULL;
        if (result == 0 && file == NULL) {
                snprintf(err, err_size, "out of memory");
                result = -1;
        }
        if (result == 0) {
                bmds_3d_file_header_t h;
                memset(&h, 0, sizeof(h));
                memcpy(h.magic, BMDS_3D_FILE_MAGIC, 4);
                h.version = BMDS_3D_FILE_VERSION;
                h.header_bytes = sizeof(h);
                h.file_bytes = at;
                h.section_count = sections;
                memcpy(file, &h, sizeof(h));
                for (uint32_t i = 0; i < scene->count; ++i) {
                        const plan_t          *p = &plan[i];
                        const uint32_t         end = i + 1 < scene->count ? plan[i + 1].offset : names_at;
                        bmds_3d_file_section_t s = {BMDS_3D_SECTION_MESH, 0, p->offset, end - p->offset};
                        memcpy(file + table + i * sizeof(s), &s, sizeof(s));
                        uint8_t *b = file + p->offset;
                        memcpy(b, &p->head, sizeof(p->head));
                        const uint32_t v = p->head.vertex_count;
                        memcpy(b + p->head.positions, p->view.positions, v * 12);
                        if (p->head.normals != 0) memcpy(b + p->head.normals, p->view.normals, v * 12);
                        if (p->head.uvs != 0) memcpy(b + p->head.uvs, p->view.uvs, v * 8);
                        if (p->head.triangles != 0)
                                memcpy(b + p->head.triangles, p->view.triangles, p->head.triangle_count * 6);
                        if (p->head.edges != 0) {
                                memcpy(b + p->head.edges, p->edges, p->head.edge_count * 4);
                                memcpy(b + p->head.edge_faces, p->edges + 2 * p->head.edge_count,
                                       p->head.edge_count * 4);
                        }
                }
                if (named != 0) {
                        bmds_3d_file_section_t s = {BMDS_3D_SECTION_NAMES, 0, names_at, at - names_at};
                        memcpy(file + table + scene->count * sizeof(s), &s, sizeof(s));
                        uint8_t             *b = file + names_at;
                        bmds_3d_file_names_t nh = {sizeof(nh), named};
                        memcpy(b, &nh, sizeof(nh));
                        uint32_t entry = sizeof(nh), text = sizeof(nh) + named * 12;
                        for (uint32_t i = 0; i < scene->count; ++i) {
                                const char *name = scene->meshes[i].name;
                                if (name[0] == '\0') continue;
                                bmds_3d_file_name_t e = {BMDS_3D_SECTION_MESH, i, text};
                                memcpy(b + entry, &e, sizeof(e));
                                entry += sizeof(e);
                                memcpy(b + text, name, strlen(name) + 1);
                                text += (uint32_t)strlen(name) + 1;
                        }
                }
                *out = file;
                *out_size = at;
        }
        for (uint32_t i = 0; i < scene->count; ++i) {
                bmds_3d_mesh_free(&plan[i].view);
                free(plan[i].edges);
        }
        free(plan);
        return result;
}
