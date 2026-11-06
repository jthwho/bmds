/**
 * @file      mesh.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's meshes (devplan/3d.md §4c): shapes made with normals and uvs, edges found from
 * triangles (vertices at one position welded by a hash, sides sorted by a radix sort), bounds.
 */

#include <math.h>
#include <string.h>
#include "3d_internal.h"

#define PI 3.14159265358979f
#define MAX_VERTICES 65535u

/* ---- Bounds ----------------------------------------------------------------------------------- */

void bmds_3d_mesh_bounds(bmds_3d_mesh_t *mesh) {
        memset(mesh->bounds, 0, sizeof(mesh->bounds));
        if (mesh->vertex_count == 0 || mesh->positions == NULL) return;
        const float *p = mesh->positions;
        float        lo[3] = {p[0], p[1], p[2]}, hi[3] = {p[0], p[1], p[2]};
        for (uint32_t i = 1; i < mesh->vertex_count; ++i) {
                for (int k = 0; k < 3; ++k) {
                        const float v = p[3 * i + k];
                        if (v < lo[k]) lo[k] = v;
                        if (v > hi[k]) hi[k] = v;
                }
        }
        float r2 = 0.0f;
        for (int k = 0; k < 3; ++k) mesh->bounds[k] = 0.5f * (lo[k] + hi[k]);
        for (uint32_t i = 0; i < mesh->vertex_count; ++i) {
                const float dx = p[3 * i] - mesh->bounds[0], dy = p[3 * i + 1] - mesh->bounds[1],
                            dz = p[3 * i + 2] - mesh->bounds[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d > r2) r2 = d;
        }
        /* A hair more, so rounding never culls a mesh that touches the view. */
        mesh->bounds[3] = sqrtf(r2) * 1.0001f + 1e-6f;
        return;
}

/* ---- Edges ------------------------------------------------------------------------------------ */

static bmds_vec3_t position(const bmds_3d_mesh_t *m, uint32_t v) {
        return bmds_vec3(m->positions[3 * v], m->positions[3 * v + 1], m->positions[3 * v + 2]);
}

static uint32_t hash_position(const float *p) {
        uint32_t h = 2166136261u;
        for (int k = 0; k < 3; ++k) {
                float    f = p[k] == 0.0f ? 0.0f : p[k]; /* -0 and 0 alike */
                uint32_t bits;
                memcpy(&bits, &f, sizeof(bits));
                h = (h ^ bits) * 16777619u;
        }
        return h ^ (h >> 15);
}

/* @p canon[v]: the first vertex at v's position. @p table: @p mask + 1 entries of scratch. */
static void weld(const bmds_3d_mesh_t *m, uint16_t *canon, uint16_t *table, uint32_t mask) {
        for (uint32_t i = 0; i <= mask; ++i) table[i] = 0xffffu;
        for (uint32_t v = 0; v < m->vertex_count; ++v) {
                const float *p = m->positions + 3 * v;
                uint32_t     slot = hash_position(p) & mask;
                for (;;) {
                        const uint16_t t = table[slot];
                        if (t == 0xffffu) {
                                table[slot] = (uint16_t)v;
                                canon[v] = (uint16_t)v;
                                break;
                        }
                        const float *q = m->positions + 3 * t;
                        if (p[0] == q[0] && p[1] == q[1] && p[2] == q[2]) {
                                canon[v] = t;
                                break;
                        }
                        slot = (slot + 1) & mask;
                }
        }
        return;
}

/* Sorts @p n keys (the high 32 bits: the side; the low: triangle × 4 + side) by their high
   half, stably: four 8-bit passes of an LSD radix sort through @p tmp. */
static void sort_sides(uint64_t *keys, uint64_t *tmp, uint32_t n) {
        for (int pass = 0; pass < 4; ++pass) {
                const int shift = 32 + 8 * pass;
                uint32_t  count[257] = {0};
                for (uint32_t i = 0; i < n; ++i) ++count[((keys[i] >> shift) & 0xffu) + 1];
                for (int b = 0; b < 256; ++b) count[b + 1] += count[b];
                for (uint32_t i = 0; i < n; ++i) tmp[count[(keys[i] >> shift) & 0xffu]++] = keys[i];
                memcpy(keys, tmp, (size_t)n * sizeof(*keys));
        }
        return;
}

static bmds_vec3_t face_normal(const bmds_3d_mesh_t *m, uint32_t t) {
        const uint16_t   *i = m->triangles + 3 * t;
        const bmds_vec3_t a = position(m, i[0]), b = position(m, i[1]), c = position(m, i[2]);
        return bmds_vec3_normalize(bmds_vec3_cross(bmds_vec3_sub(b, a), bmds_vec3_sub(c, a)));
}

static float length2(bmds_vec3_t a, bmds_vec3_t b) {
        const bmds_vec3_t d = bmds_vec3_sub(a, b);
        return bmds_vec3_dot(d, d);
}

/* Whether side @p side (0: v0-v1, 1: v1-v2, 2: v2-v0) is triangle @p t's longest (ties too). */
static int is_longest(const bmds_3d_mesh_t *m, uint32_t t, uint32_t side) {
        const uint16_t   *i = m->triangles + 3 * t;
        const bmds_vec3_t p[3] = {position(m, i[0]), position(m, i[1]), position(m, i[2])};
        float             l[3];
        for (int s = 0; s < 3; ++s) l[s] = length2(p[s], p[(s + 1) % 3]);
        const float mine = l[side];
        return mine >= l[(side + 1) % 3] * 0.9999f && mine >= l[(side + 2) % 3] * 0.9999f;
}

/* The edges of @p mesh; @p diagonals (a made shape's: per triangle, a bit per side) marks the
   sides that are a quad's diagonal, dropped for certain; NULL: found as the flags say. */
static int edges_build(bmds_3d_mesh_t *mesh, uint32_t flags, uint32_t mem_flags, const uint8_t *diagonals) {
        if (mesh == NULL || (flags & ~BMDS_3D_EDGES_COPLANAR) != 0) return -BMDS_EINVAL;
        if (mesh->triangle_count == 0 || mesh->triangles == NULL || mesh->positions == NULL) return -BMDS_EINVAL;
        if (mesh->vertex_count == 0 || mesh->vertex_count > MAX_VERTICES || mesh->triangle_count > 0x3fffffffu / 3)
                return -BMDS_EINVAL;
        for (uint32_t i = 0; i < 3 * mesh->triangle_count; ++i)
                if (mesh->triangles[i] >= mesh->vertex_count) return -BMDS_EINVAL;

        const uint32_t n = 3 * mesh->triangle_count;
        uint32_t       mask = 1;
        while (mask < 2 * mesh->vertex_count) mask <<= 1;
        --mask;
        /* Scratch: the keys twice, then the canonical vertices and the hash table. */
        const size_t scratch_bytes = 2 * (size_t)n * sizeof(uint64_t) + (size_t)mesh->vertex_count * sizeof(uint16_t) +
                                     ((size_t)mask + 1) * sizeof(uint16_t);
        uint8_t *scratch = (uint8_t *)bmds_mem_alloc((uint32_t)scratch_bytes, 8, 0);
        if (scratch == NULL) return -BMDS_ENOMEM;
        uint64_t *keys = (uint64_t *)(void *)scratch;
        uint64_t *tmp = keys + n;
        uint16_t *canon = (uint16_t *)(void *)(tmp + n);
        uint16_t *table = canon + mesh->vertex_count;

        weld(mesh, canon, table, mask);
        for (uint32_t t = 0; t < mesh->triangle_count; ++t) {
                for (uint32_t s = 0; s < 3; ++s) {
                        uint32_t a = canon[mesh->triangles[3 * t + s]], b = canon[mesh->triangles[3 * t + (s + 1) % 3]];
                        if (a > b) {
                                const uint32_t x = a;
                                a = b;
                                b = x;
                        }
                        keys[3 * t + s] = (uint64_t)(a << 16 | b) << 32 | (uint64_t)(t * 4 + s);
                }
        }
        sort_sides(keys, tmp, n);

        /* Each run of one side is an edge (its first two triangles beside it), unless dropped. Kept
           ones are written over the keys' front, as (key, first, second). */
        uint32_t edges = 0;
        for (uint32_t i = 0; i < n;) {
                uint32_t j = i + 1;
                while (j < n && keys[j] >> 32 == keys[i] >> 32) ++j;
                const uint32_t first = (uint32_t)keys[i];
                const uint32_t second = j - i > 1 ? (uint32_t)keys[i + 1] : 0xffffffffu;
                const uint32_t a = (uint32_t)(keys[i] >> 48), b = (uint32_t)(keys[i] >> 32) & 0xffffu;
                int            keep = a != b; /* a degenerate side is no edge */
                if (keep && diagonals != NULL) {
                        keep = (diagonals[first >> 2] >> (first & 3u) & 1u) == 0;
                } else if (keep && second != 0xffffffffu) {
                        const uint32_t t1 = first >> 2, t2 = second >> 2;
                        const float    d = bmds_vec3_dot(face_normal(mesh, t1), face_normal(mesh, t2));
                        if (d > 0.9999f) {
                                if ((flags & BMDS_3D_EDGES_COPLANAR) != 0)
                                        keep = 0;
                                else if (is_longest(mesh, t1, first & 3u) && is_longest(mesh, t2, second & 3u))
                                        keep = 0;
                        }
                }
                if (keep) {
                        tmp[2 * edges] = first;
                        tmp[2 * edges + 1] = second;
                        ++edges;
                }
                i = j;
        }

        uint16_t *out = (uint16_t *)bmds_mem_alloc(edges * 4 * (uint32_t)sizeof(uint16_t), 0, mem_flags);
        if (out == NULL && edges != 0) {
                bmds_mem_free(scratch);
                return -BMDS_ENOMEM;
        }
        for (uint32_t e = 0; e < edges; ++e) {
                const uint32_t first = (uint32_t)tmp[2 * e], second = (uint32_t)tmp[2 * e + 1];
                const uint32_t t = first >> 2, s = first & 3u;
                out[2 * e] = mesh->triangles[3 * t + s];
                out[2 * e + 1] = mesh->triangles[3 * t + (s + 1) % 3];
                out[2 * edges + 2 * e] = (uint16_t)t;
                out[2 * edges + 2 * e + 1] = second == 0xffffffffu ? BMDS_3D_NO_FACE : (uint16_t)(second >> 2);
        }
        bmds_mem_free(scratch);
        bmds_mem_free(mesh->owned_edges);
        mesh->owned_edges = out;
        mesh->edges = out;
        mesh->edge_faces = out != NULL ? out + 2 * edges : NULL;
        mesh->edge_count = edges;
        return 0;
}

int bmds_3d_mesh_edges(bmds_3d_mesh_t *mesh, uint32_t flags, uint32_t mem_flags) {
        return edges_build(mesh, flags, mem_flags, NULL);
}

/* ---- Shapes ----------------------------------------------------------------------------------- */

/* A mesh being made: its arrays, and the triangles so far. */
typedef struct {
                float    *positions, *normals, *uvs;
                uint16_t *triangles;
                uint8_t  *diagonals; /* per triangle, a bit per side: a quad's diagonal */
                uint32_t  vertices, triangle_count;
} builder_t;

static void vertex(builder_t *b, uint32_t v, bmds_vec3_t p, bmds_vec3_t n, float u, float w) {
        b->positions[3 * v] = p.x;
        b->positions[3 * v + 1] = p.y;
        b->positions[3 * v + 2] = p.z;
        b->normals[3 * v] = n.x;
        b->normals[3 * v + 1] = n.y;
        b->normals[3 * v + 2] = n.z;
        b->uvs[2 * v] = u;
        b->uvs[2 * v + 1] = w;
        return;
}

static bmds_vec3_t at(const builder_t *b, uint32_t v) {
        return bmds_vec3(b->positions[3 * v], b->positions[3 * v + 1], b->positions[3 * v + 2]);
}

/* A triangle, turned to face the way its vertices' normals do; none if it has no area. Its side
   between @p d0 and @p d1 (a quad's diagonal; equal: none) is marked. */
static int triangle(builder_t *b, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t d0, uint32_t d1) {
        const bmds_vec3_t p0 = at(b, i0), p1 = at(b, i1), p2 = at(b, i2);
        const bmds_vec3_t n = bmds_vec3_cross(bmds_vec3_sub(p1, p0), bmds_vec3_sub(p2, p0));
        if (bmds_vec3_dot(n, n) < 1e-14f) return 0;
        bmds_vec3_t want = bmds_vec3(0.0f, 0.0f, 0.0f);
        for (int k = 0; k < 3; ++k) {
                const uint32_t v = k == 0 ? i0 : k == 1 ? i1 : i2;
                want = bmds_vec3_add(want, bmds_vec3(b->normals[3 * v], b->normals[3 * v + 1], b->normals[3 * v + 2]));
        }
        const uint32_t k = b->triangle_count++;
        uint16_t      *t = b->triangles + 3 * k;
        t[0] = (uint16_t)i0;
        if (bmds_vec3_dot(n, want) >= 0.0f) {
                t[1] = (uint16_t)i1;
                t[2] = (uint16_t)i2;
        } else {
                t[1] = (uint16_t)i2;
                t[2] = (uint16_t)i1;
        }
        b->diagonals[k] = 0;
        if (d0 == d1) return 1;
        for (uint32_t side = 0; side < 3; ++side) {
                const uint32_t p = t[side], q = t[(side + 1) % 3];
                if ((p == d0 && q == d1) || (p == d1 && q == d0)) b->diagonals[k] = (uint8_t)(1u << side);
        }
        return 1;
}

/* Two triangles of the quad a-b-c-d (in order around it), a-c their diagonal. If one has no area
   (a sphere's pole), the other is the quad, and a-c is a real edge. */
static void quad(builder_t *b, uint32_t a, uint32_t bb, uint32_t c, uint32_t d) {
        const uint32_t first = b->triangle_count;
        const int      both = triangle(b, a, bb, c, a, c) & triangle(b, a, c, d, a, c);
        if (!both)
                for (uint32_t k = first; k < b->triangle_count; ++k) b->diagonals[k] = 0;
        return;
}

static void make_cube(builder_t *b, float r) {
        /* Each face: its normal, and two axes across it. */
        static const float faces[6][9] = {
                {1, 0, 0, 0, 0, -1, 0, 1, 0}, {-1, 0, 0, 0, 0, 1, 0, 1, 0}, {0, 1, 0, 1, 0, 0, 0, 0, -1},
                {0, -1, 0, 1, 0, 0, 0, 0, 1}, {0, 0, 1, 1, 0, 0, 0, 1, 0},  {0, 0, -1, -1, 0, 0, 0, 1, 0},
        };
        static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (uint32_t f = 0; f < 6; ++f) {
                const bmds_vec3_t n = bmds_vec3(faces[f][0], faces[f][1], faces[f][2]);
                const bmds_vec3_t s = bmds_vec3(faces[f][3], faces[f][4], faces[f][5]);
                const bmds_vec3_t t = bmds_vec3(faces[f][6], faces[f][7], faces[f][8]);
                for (uint32_t c = 0; c < 4; ++c) {
                        const bmds_vec3_t p =
                                bmds_vec3_scale(bmds_vec3_add(n, bmds_vec3_add(bmds_vec3_scale(s, corners[c][0]),
                                                                               bmds_vec3_scale(t, corners[c][1]))),
                                                r);
                        vertex(b, 4 * f + c, p, n, 0.5f * (corners[c][0] + 1.0f), 0.5f * (1.0f - corners[c][1]));
                }
                quad(b, 4 * f, 4 * f + 1, 4 * f + 2, 4 * f + 3);
        }
        return;
}

static void make_grid(builder_t *b, float r, uint32_t n) {
        for (uint32_t j = 0; j <= n; ++j) {
                for (uint32_t i = 0; i <= n; ++i) {
                        const float u = (float)i / (float)n, w = (float)j / (float)n;
                        vertex(b, j * (n + 1) + i, bmds_vec3((2.0f * u - 1.0f) * r, 0.0f, (2.0f * w - 1.0f) * r),
                               bmds_vec3(0.0f, 1.0f, 0.0f), u, w);
                }
        }
        for (uint32_t j = 0; j < n; ++j) {
                for (uint32_t i = 0; i < n; ++i) {
                        const uint32_t v = j * (n + 1) + i;
                        quad(b, v, v + 1, v + n + 2, v + n + 1);
                }
        }
        return;
}

static void make_sphere(builder_t *b, float r, uint32_t segments, uint32_t rings) {
        for (uint32_t j = 0; j <= rings; ++j) {
                const float theta = PI * (float)j / (float)rings;
                const float st = sinf(theta), ct = cosf(theta);
                for (uint32_t i = 0; i <= segments; ++i) {
                        /* The seam's column exactly the first's, so its vertices weld (edges). */
                        const float       phi = 2.0f * PI * (float)i / (float)segments;
                        const float       cp = i == segments ? 1.0f : cosf(phi), sp = i == segments ? 0.0f : sinf(phi);
                        const bmds_vec3_t n = j == 0       ? bmds_vec3(0, 1, 0)
                                              : j == rings ? bmds_vec3(0, -1, 0)
                                                           : bmds_vec3(st * cp, ct, -st * sp);
                        vertex(b, j * (segments + 1) + i, bmds_vec3_scale(n, r), n, (float)i / (float)segments,
                               (float)j / (float)rings);
                }
        }
        for (uint32_t j = 0; j < rings; ++j) {
                for (uint32_t i = 0; i < segments; ++i) {
                        const uint32_t v = j * (segments + 1) + i;
                        quad(b, v, v + segments + 1, v + segments + 2, v + 1);
                }
        }
        return;
}

static void make_torus(builder_t *b, float r, float tube, uint32_t segments, uint32_t rings) {
        for (uint32_t j = 0; j <= rings; ++j) {
                const float v = 2.0f * PI * (float)j / (float)rings;
                for (uint32_t i = 0; i <= segments; ++i) {
                        const float       u = 2.0f * PI * (float)i / (float)segments;
                        const float       cu = i == segments ? 1.0f : cosf(u), su = i == segments ? 0.0f : sinf(u);
                        const float       cv = j == rings ? 1.0f : cosf(v), sv = j == rings ? 0.0f : sinf(v);
                        const bmds_vec3_t n = bmds_vec3(cv * cu, sv, -cv * su);
                        const bmds_vec3_t c = bmds_vec3(r * cu, 0.0f, -r * su);
                        vertex(b, j * (segments + 1) + i, bmds_vec3_add(c, bmds_vec3_scale(n, tube)), n,
                               (float)i / (float)segments, (float)j / (float)rings);
                }
        }
        for (uint32_t j = 0; j < rings; ++j) {
                for (uint32_t i = 0; i < segments; ++i) {
                        const uint32_t k = j * (segments + 1) + i;
                        quad(b, k, k + 1, k + segments + 2, k + segments + 1);
                }
        }
        return;
}

int bmds_3d_mesh_make(const bmds_3d_shape_t *shape, uint32_t mem_flags, bmds_3d_mesh_t *mesh) {
        if (shape == NULL || mesh == NULL || shape->size < sizeof(bmds_3d_shape_t) || shape->flags != 0)
                return -BMDS_EINVAL;
        const float r = shape->radius > 0.0f ? shape->radius : 1.0f;
        uint32_t    segments = shape->segments, rings = shape->rings, vertices, triangles;
        switch (shape->kind) {
                case BMDS_3D_SHAPE_CUBE:
                        vertices = 24;
                        triangles = 12;
                        break;
                case BMDS_3D_SHAPE_GRID:
                        if (segments == 0) segments = 8;
                        if (segments > 254) return -BMDS_EINVAL;
                        vertices = (segments + 1) * (segments + 1);
                        triangles = 2 * segments * segments;
                        break;
                case BMDS_3D_SHAPE_SPHERE:
                case BMDS_3D_SHAPE_TORUS:
                        if (segments == 0) segments = shape->kind == BMDS_3D_SHAPE_SPHERE ? 16 : 24;
                        if (rings == 0) rings = 12;
                        if (segments < 3 || rings < (shape->kind == BMDS_3D_SHAPE_SPHERE ? 2u : 3u) ||
                            segments > 1024 || rings > 1024)
                                return -BMDS_EINVAL;
                        vertices = (segments + 1) * (rings + 1);
                        triangles = 2 * segments * rings;
                        break;
                default: return -BMDS_EINVAL;
        }
        if (vertices > MAX_VERTICES) return -BMDS_EINVAL;

        const size_t floats = (size_t)vertices * 8; /* positions, normals, uvs */
        const size_t bytes = floats * sizeof(float) + (size_t)triangles * 3 * sizeof(uint16_t);
        float       *block = (float *)bmds_mem_alloc((uint32_t)bytes, 0, mem_flags);
        uint8_t     *diagonals = (uint8_t *)bmds_mem_alloc(triangles, 0, 0);
        if (block == NULL || diagonals == NULL) {
                bmds_mem_free(block);
                bmds_mem_free(diagonals);
                return -BMDS_ENOMEM;
        }
        builder_t b = {block,
                       block + 3 * vertices,
                       block + 6 * vertices,
                       (uint16_t *)(void *)(block + floats),
                       diagonals,
                       vertices,
                       0};
        switch (shape->kind) {
                case BMDS_3D_SHAPE_CUBE: make_cube(&b, r); break;
                case BMDS_3D_SHAPE_GRID: make_grid(&b, r, segments); break;
                case BMDS_3D_SHAPE_SPHERE: make_sphere(&b, r, segments, rings); break;
                default: make_torus(&b, r, shape->radius2 > 0.0f ? shape->radius2 : r / 3.0f, segments, rings); break;
        }

        memset(mesh, 0, sizeof(*mesh));
        mesh->size = sizeof(*mesh);
        mesh->vertex_count = vertices;
        mesh->positions = b.positions;
        mesh->normals = b.normals;
        mesh->uvs = b.uvs;
        mesh->triangle_count = b.triangle_count;
        mesh->triangles = b.triangles;
        mesh->owned = block;
        bmds_3d_mesh_bounds(mesh);
        const int e = edges_build(mesh, 0, mem_flags, diagonals);
        bmds_mem_free(diagonals);
        if (e < 0) {
                bmds_3d_mesh_free(mesh);
                return e;
        }
        return 0;
}

void bmds_3d_mesh_free(bmds_3d_mesh_t *mesh) {
        if (mesh == NULL) return;
        bmds_mem_free(mesh->owned);
        bmds_mem_free(mesh->owned_edges);
        memset(mesh, 0, sizeof(*mesh));
        mesh->size = sizeof(*mesh);
        return;
}
