/**
 * @file      build.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-mesh3d's meshes: vertices unique by value (a hash), triangles without area dropped,
 * seams welded, and the last touches (Z-up, fitting, scaling) before writing.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mesh3d.h"

#define MAX_VERTICES 65535u
#define PI 3.14159265358979

void m3d_options_init(m3d_options_t *options) {
        memset(options, 0, sizeof(*options));
        options->crease = 60.0f;
        options->detail = 4;
        options->scale = 1.0f;
        return;
}

m3d_mesh_t *m3d_scene_add(m3d_scene_t *scene, const char *name) {
        m3d_mesh_t *grown = (m3d_mesh_t *)realloc(scene->meshes, (scene->count + 1) * sizeof(m3d_mesh_t));
        if (grown == NULL) return NULL;
        scene->meshes = grown;
        m3d_mesh_t *m = &grown[scene->count];
        memset(m, 0, sizeof(*m));
        m->name = strdup(name != NULL ? name : "");
        if (m->name == NULL) return NULL;
        ++scene->count;
        return m;
}

m3d_mesh_t *m3d_scene_find(m3d_scene_t *scene, const char *name) {
        for (uint32_t i = 0; i < scene->count; ++i)
                if (strcmp(scene->meshes[i].name, name) == 0) return &scene->meshes[i];
        return NULL;
}

static void mesh_free(m3d_mesh_t *m) {
        free(m->name);
        free(m->positions);
        free(m->normals);
        free(m->uvs);
        free(m->triangles);
        free(m->inner);
        free(m->hash);
        memset(m, 0, sizeof(*m));
        return;
}

void m3d_scene_free(m3d_scene_t *scene) {
        for (uint32_t i = 0; i < scene->count; ++i) mesh_free(&scene->meshes[i]);
        free(scene->meshes);
        scene->meshes = NULL;
        scene->count = 0;
        return;
}

/* ---- Vertices and triangles ------------------------------------------------------------------- */

static uint32_t hash_vertex(const float *v) {
        uint32_t h = 2166136261u;
        for (int k = 0; k < 8; ++k) {
                const float f = v[k] == 0.0f ? 0.0f : v[k]; /* -0 and 0 alike */
                uint32_t    bits;
                memcpy(&bits, &f, sizeof(bits));
                h = (h ^ bits) * 16777619u;
        }
        return h ^ (h >> 15);
}

static void vertex_of(const m3d_mesh_t *m, uint32_t i, float *v) {
        memcpy(v, m->positions + 3 * i, 3 * sizeof(float));
        memcpy(v + 3, m->normals + 3 * i, 3 * sizeof(float));
        memcpy(v + 6, m->uvs + 2 * i, 2 * sizeof(float));
        return;
}

static int same_vertex(const float *a, const float *b) {
        for (int k = 0; k < 8; ++k)
                if (a[k] != b[k]) return 0;
        return 1;
}

static int rehash(m3d_mesh_t *m, uint32_t slots) {
        uint32_t *table = (uint32_t *)calloc(slots, sizeof(uint32_t));
        if (table == NULL) return -1;
        free(m->hash);
        m->hash = table;
        m->hash_mask = slots - 1;
        for (uint32_t i = 0; i < m->vertex_count; ++i) {
                float v[8];
                vertex_of(m, i, v);
                uint32_t s = hash_vertex(v) & m->hash_mask;
                while (table[s] != 0) s = (s + 1) & m->hash_mask;
                table[s] = i + 1;
        }
        return 0;
}

int32_t m3d_vertex(m3d_mesh_t *m, const float *p, const float *n, const float *uv) {
        float v[8] = {p[0], p[1], p[2], 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        if (n != NULL) memcpy(v + 3, n, 3 * sizeof(float));
        if (uv != NULL) memcpy(v + 6, uv, 2 * sizeof(float));
        if (n != NULL) m->has_normals = 1;
        if (uv != NULL) m->has_uvs = 1;
        if (m->hash == NULL && rehash(m, 1024) != 0) return -1;

        uint32_t s = hash_vertex(v) & m->hash_mask;
        for (; m->hash[s] != 0; s = (s + 1) & m->hash_mask) {
                float w[8];
                vertex_of(m, m->hash[s] - 1, w);
                if (same_vertex(v, w)) return (int32_t)(m->hash[s] - 1);
        }
        if (m->vertex_count >= MAX_VERTICES) return -1;
        if (m->vertex_count == m->vertex_cap) {
                const uint32_t cap = m->vertex_cap != 0 ? 2 * m->vertex_cap : 256;
                float         *pos = (float *)realloc(m->positions, cap * 3 * sizeof(float));
                if (pos == NULL) return -1;
                m->positions = pos;
                float *nrm = (float *)realloc(m->normals, cap * 3 * sizeof(float));
                if (nrm == NULL) return -1;
                m->normals = nrm;
                float *tex = (float *)realloc(m->uvs, cap * 2 * sizeof(float));
                if (tex == NULL) return -1;
                m->uvs = tex;
                m->vertex_cap = cap;
        }
        const uint32_t i = m->vertex_count++;
        memcpy(m->positions + 3 * i, v, 3 * sizeof(float));
        memcpy(m->normals + 3 * i, v + 3, 3 * sizeof(float));
        memcpy(m->uvs + 2 * i, v + 6, 2 * sizeof(float));
        m->hash[s] = i + 1;
        if (2 * m->vertex_count > m->hash_mask && rehash(m, 2 * (m->hash_mask + 1)) != 0) return -1;
        return (int32_t)i;
}

int m3d_triangle(m3d_mesh_t *m, uint32_t a, uint32_t b, uint32_t c) {
        return m3d_triangle_inner(m, a, b, c, 0);
}

int m3d_triangle_inner(m3d_mesh_t *m, uint32_t a, uint32_t b, uint32_t c, uint32_t inner) {
        if (a == b || b == c || a == c) return 0;
        const float *pa = m->positions + 3 * a, *pb = m->positions + 3 * b, *pc = m->positions + 3 * c;
        const double e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const double e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const double x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2],
                     z = e1[0] * e2[1] - e1[1] * e2[0];
        const double l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
        const double l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        if (x * x + y * y + z * z <= 1e-14 * l1 * l2) return 0; /* no area (or a sliver of 1e-7 rad) */
        if (m->triangle_count == m->triangle_cap) {
                const uint32_t cap = m->triangle_cap != 0 ? 2 * m->triangle_cap : 256;
                uint16_t      *t = (uint16_t *)realloc(m->triangles, cap * 3 * sizeof(uint16_t));
                if (t == NULL) return -1;
                m->triangles = t;
                uint8_t *in = (uint8_t *)realloc(m->inner, cap);
                if (in == NULL) return -1;
                m->inner = in;
                m->triangle_cap = cap;
        }
        m->inner[m->triangle_count] = (uint8_t)(inner & 7u);
        uint16_t *t = m->triangles + 3 * m->triangle_count++;
        t[0] = (uint16_t)a;
        t[1] = (uint16_t)b;
        t[2] = (uint16_t)c;
        return 0;
}

/* ---- Welding ---------------------------------------------------------------------------------- */

static const float *g_sort_positions; /* qsort has no context argument */

static int by_x(const void *a, const void *b) {
        const float x = g_sort_positions[3 * *(const uint32_t *)a], y = g_sort_positions[3 * *(const uint32_t *)b];
        return x < y ? -1 : x > y ? 1 : 0;
}

static uint32_t find(uint32_t *parent, uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
}

/* Rebuilds @p m from its own vertices mapped through @p to and given @p positions etc.: values
   made unique again, triangles re-added (dropping those left without area). */
static int rebuild(m3d_mesh_t *m, const uint32_t *to, int keep_normals, int keep_uvs) {
        m3d_mesh_t fresh;
        memset(&fresh, 0, sizeof(fresh));
        fresh.name = m->name;
        uint32_t *index = (uint32_t *)malloc((m->vertex_count != 0 ? m->vertex_count : 1) * sizeof(uint32_t));
        if (index == NULL) return -1;
        int err = 0;
        for (uint32_t i = 0; err == 0 && i < m->vertex_count; ++i) {
                const uint32_t j = to != NULL ? to[i] : i;
                const int32_t  v = m3d_vertex(&fresh, m->positions + 3 * j, keep_normals ? m->normals + 3 * j : NULL,
                                             keep_uvs ? m->uvs + 2 * j : NULL);
                if (v < 0) err = -1;
                index[i] = (uint32_t)v;
        }
        for (uint32_t t = 0; err == 0 && t < m->triangle_count; ++t) {
                const uint16_t *tri = m->triangles + 3 * t;
                err = m3d_triangle_inner(&fresh, index[tri[0]], index[tri[1]], index[tri[2]], m->inner[t]);
        }
        free(index);
        if (err != 0) {
                fresh.name = NULL;
                mesh_free(&fresh);
                return -1;
        }
        m->name = NULL;
        mesh_free(m);
        *m = fresh;
        return 0;
}

int m3d_weld(m3d_mesh_t *m, float crease) {
        const uint32_t n = m->vertex_count;
        if (n < 2) return 0;
        float lo[3], hi[3];
        for (int k = 0; k < 3; ++k) lo[k] = hi[k] = m->positions[k];
        for (uint32_t i = 0; i < n; ++i)
                for (int k = 0; k < 3; ++k) {
                        const float v = m->positions[3 * i + k];
                        if (v < lo[k]) lo[k] = v;
                        if (v > hi[k]) hi[k] = v;
                }
        float extent = 0.0f;
        for (int k = 0; k < 3; ++k)
                if (hi[k] - lo[k] > extent) extent = hi[k] - lo[k];
        const float  eps = extent * 1e-6f;
        const double cos_crease = cos(crease * PI / 180.0) - 1e-6;

        uint32_t *order = (uint32_t *)malloc(n * sizeof(uint32_t));
        uint32_t *parent = (uint32_t *)malloc(n * sizeof(uint32_t));
        float    *sum = (float *)calloc(n * 3, sizeof(float));
        if (order == NULL || parent == NULL || sum == NULL) {
                free(order), free(parent), free(sum);
                return -1;
        }
        for (uint32_t i = 0; i < n; ++i) order[i] = parent[i] = i;
        g_sort_positions = m->positions;
        qsort(order, n, sizeof(uint32_t), by_x);

        /* Each vertex joins the group of the first earlier one it matches: same place, same uv,
           normal within the crease of that group's first vertex. */
        for (uint32_t a = 0; a < n; ++a) {
                const uint32_t i = order[a];
                const float   *p = m->positions + 3 * i, *ni = m->normals + 3 * i;
                for (uint32_t b = a; b-- > 0;) {
                        const uint32_t j = order[b];
                        const float   *q = m->positions + 3 * j;
                        if (p[0] - q[0] > eps) break;
                        if (fabsf(p[1] - q[1]) > eps || fabsf(p[2] - q[2]) > eps) continue;
                        if (m->uvs[2 * i] != m->uvs[2 * j] || m->uvs[2 * i + 1] != m->uvs[2 * j + 1]) continue;
                        const uint32_t r = find(parent, j);
                        const float   *nr = m->normals + 3 * r;
                        const double   d = (double)ni[0] * nr[0] + (double)ni[1] * nr[1] + (double)ni[2] * nr[2];
                        const int      flat = ni[0] == 0.0f && ni[1] == 0.0f && ni[2] == 0.0f && nr[0] == 0.0f &&
                                         nr[1] == 0.0f && nr[2] == 0.0f;
                        if (!flat && d < cos_crease) continue;
                        parent[i] = r;
                        break;
                }
        }
        /* Each group's normal: its members' averaged; its position: the first's. */
        for (uint32_t i = 0; i < n; ++i) {
                const uint32_t r = find(parent, i);
                for (int k = 0; k < 3; ++k) sum[3 * r + k] += m->normals[3 * i + k];
        }
        for (uint32_t i = 0; i < n; ++i) {
                if (parent[i] != i) continue;
                const float *s = sum + 3 * i;
                const float  l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
                if (l > 0.0f)
                        for (int k = 0; k < 3; ++k) m->normals[3 * i + k] = s[k] / l;
        }
        for (uint32_t i = 0; i < n; ++i) parent[i] = find(parent, i);
        const int err = rebuild(m, parent, m->has_normals, m->has_uvs);
        free(order), free(parent), free(sum);
        return err;
}

/* ---- Finishing -------------------------------------------------------------------------------- */

int m3d_finish(m3d_scene_t *scene, const m3d_options_t *options, char *err, size_t err_size) {
        /* Meshes without triangles go (an OBJ's empty groups). */
        uint32_t kept = 0;
        for (uint32_t i = 0; i < scene->count; ++i) {
                if (scene->meshes[i].triangle_count == 0)
                        mesh_free(&scene->meshes[i]);
                else
                        scene->meshes[kept++] = scene->meshes[i];
        }
        scene->count = kept;
        if (kept == 0) {
                snprintf(err, err_size, "no triangles");
                return -1;
        }

        for (uint32_t i = 0; i < scene->count; ++i) {
                m3d_mesh_t *m = &scene->meshes[i];
                if (options->up_z) {
                        for (uint32_t v = 0; v < m->vertex_count; ++v) {
                                float      *p = m->positions + 3 * v, *n = m->normals + 3 * v;
                                const float py = p[1], ny = n[1];
                                p[1] = p[2], p[2] = -py;
                                n[1] = n[2], n[2] = -ny;
                        }
                }
        }

        double centre[3] = {0.0, 0.0, 0.0}, scale = options->scale;
        if (options->fit) {
                double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
                for (uint32_t i = 0; i < scene->count; ++i)
                        for (uint32_t v = 0; v < scene->meshes[i].vertex_count; ++v)
                                for (int k = 0; k < 3; ++k) {
                                        const double x = scene->meshes[i].positions[3 * v + k];
                                        if (x < lo[k]) lo[k] = x;
                                        if (x > hi[k]) hi[k] = x;
                                }
                for (int k = 0; k < 3; ++k) centre[k] = 0.5 * (lo[k] + hi[k]);
                double r2 = 0.0;
                for (uint32_t i = 0; i < scene->count; ++i)
                        for (uint32_t v = 0; v < scene->meshes[i].vertex_count; ++v) {
                                const float *p = scene->meshes[i].positions + 3 * v;
                                const double dx = p[0] - centre[0], dy = p[1] - centre[1], dz = p[2] - centre[2];
                                if (dx * dx + dy * dy + dz * dz > r2) r2 = dx * dx + dy * dy + dz * dz;
                        }
                if (r2 > 0.0) scale /= sqrt(r2);
        }
        for (uint32_t i = 0; i < scene->count; ++i) {
                m3d_mesh_t *m = &scene->meshes[i];
                for (uint32_t v = 0; v < m->vertex_count; ++v)
                        for (int k = 0; k < 3; ++k)
                                m->positions[3 * v + k] = (float)((m->positions[3 * v + k] - centre[k]) * scale);
                /* A negative scale mirrors: the triangles turn to keep their fronts. */
                if (scale < 0.0) {
                        for (uint32_t v = 0; v < m->vertex_count; ++v)
                                for (int k = 0; k < 3; ++k) m->normals[3 * v + k] = -m->normals[3 * v + k];
                        for (uint32_t t = 0; t < m->triangle_count; ++t) {
                                uint16_t *tri = m->triangles + 3 * t, x = tri[1];
                                tri[1] = tri[2], tri[2] = x;
                                /* Sides 0 and 2 change places; side 1 stays (reversed). */
                                const uint8_t in = m->inner[t];
                                m->inner[t] = (uint8_t)((in & 2u) | (in >> 2 & 1u) | (in << 2 & 4u));
                        }
                }
                const int normals = m->has_normals && !options->no_normals, uvs = m->has_uvs && !options->no_uvs;
                if ((normals != m->has_normals || uvs != m->has_uvs) && rebuild(m, NULL, normals, uvs) != 0) {
                        snprintf(err, err_size, "out of memory");
                        return -1;
                }
        }
        return 0;
}
