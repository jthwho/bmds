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

/* Marks triangle @p t's side between @p d0 and @p d1 (if it has one) as no edge. */
static void mark(builder_t *b, uint32_t t, uint32_t d0, uint32_t d1) {
        const uint16_t *v = b->triangles + 3 * t;
        for (uint32_t side = 0; side < 3; ++side) {
                const uint32_t p = v[side], q = v[(side + 1) % 3];
                if ((p == d0 && q == d1) || (p == d1 && q == d0)) b->diagonals[t] |= (uint8_t)(1u << side);
        }
        return;
}

/* A cap: triangles from @p centre to the rim @p rim .. @p rim + @p n (n + 1 vertices, the last the
   first's twin), only the rim an edge. */
static void fan(builder_t *b, uint32_t centre, uint32_t rim, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
                const uint32_t t = b->triangle_count;
                if (!triangle(b, centre, rim + i, rim + i + 1, centre, rim + i)) continue;
                mark(b, t, centre, rim + i + 1);
        }
        return;
}

/* Cylinder (@p tip 0: radius at both ends) or cone (the top a point), half height @p h. */
static void make_tube(builder_t *b, float r, float h, uint32_t segments, uint32_t rings, int cone) {
        uint32_t v = 0;
        for (uint32_t j = 0; j <= rings; ++j) {
                const float f = (float)j / (float)rings, y = -h + 2.0f * h * f, rr = cone ? r * (1.0f - f) : r;
                for (uint32_t i = 0; i <= segments; ++i, ++v) {
                        const float a = 2.0f * PI * (float)i / (float)segments;
                        const float ca = i == segments ? 1.0f : cosf(a), sa = i == segments ? 0.0f : sinf(a);
                        /* A cone's side leans: its normal tilts up by the slope. */
                        const bmds_vec3_t n = bmds_vec3_normalize(bmds_vec3(
                                ca * (cone ? 2.0f * h : 1.0f), cone ? r : 0.0f, -sa * (cone ? 2.0f * h : 1.0f)));
                        vertex(b, v, bmds_vec3(rr * ca, y, -rr * sa), n, (float)i / (float)segments, 1.0f - f);
                }
        }
        for (uint32_t j = 0; j < rings; ++j)
                for (uint32_t i = 0; i < segments; ++i) {
                        const uint32_t k = j * (segments + 1) + i;
                        quad(b, k, k + 1, k + segments + 2, k + segments + 1);
                }
        /* The caps: the bottom, and a cylinder's top. */
        for (int top = 0; top < (cone ? 1 : 2); ++top) {
                const float       y = top ? h : -h;
                const bmds_vec3_t n = bmds_vec3(0.0f, top ? 1.0f : -1.0f, 0.0f);
                const uint32_t    centre = v++;
                vertex(b, centre, bmds_vec3(0.0f, y, 0.0f), n, 0.5f, 0.5f);
                const uint32_t rim = v;
                for (uint32_t i = 0; i <= segments; ++i, ++v) {
                        const float a = 2.0f * PI * (float)i / (float)segments;
                        const float ca = i == segments ? 1.0f : cosf(a), sa = i == segments ? 0.0f : sinf(a);
                        vertex(b, v, bmds_vec3(r * ca, y, -r * sa), n, 0.5f + 0.5f * ca, 0.5f - 0.5f * sa);
                }
                fan(b, centre, rim, segments);
        }
        return;
}

/* A (p, q) torus knot: a tube round the curve, framed as three.js's TorusKnotGeometry does. */
static bmds_vec3_t knot_at(float t, float p, float q, float scale) {
        const float cq = cosf(q * t);
        return bmds_vec3_scale(bmds_vec3((2.0f + cq) * cosf(p * t), (2.0f + cq) * sinf(p * t), sinf(q * t)), scale);
}

static void make_knot(builder_t *b, float r, float tube, uint32_t segments, uint32_t rings, uint32_t p, uint32_t q) {
        const float scale = r / 3.0f; /* the curve reaches 3 */
        for (uint32_t i = 0; i <= segments; ++i) {
                const float       t = 2.0f * PI * (float)(i % segments) / (float)segments; /* the seam the same */
                const bmds_vec3_t c1 = knot_at(t, (float)p, (float)q, scale);
                const bmds_vec3_t c2 = knot_at(t + 0.01f, (float)p, (float)q, scale);
                const bmds_vec3_t tangent = bmds_vec3_sub(c2, c1);
                bmds_vec3_t       n = bmds_vec3_add(c2, c1);
                const bmds_vec3_t bi = bmds_vec3_normalize(bmds_vec3_cross(tangent, n));
                n = bmds_vec3_normalize(bmds_vec3_cross(bi, tangent));
                for (uint32_t j = 0; j <= rings; ++j) {
                        const float       a = 2.0f * PI * (float)(j % rings) / (float)rings;
                        const bmds_vec3_t d = bmds_vec3_add(bmds_vec3_scale(n, cosf(a)), bmds_vec3_scale(bi, sinf(a)));
                        vertex(b, i * (rings + 1) + j, bmds_vec3_add(c1, bmds_vec3_scale(d, tube)), d,
                               (float)i / (float)segments, (float)j / (float)rings);
                }
        }
        for (uint32_t i = 0; i < segments; ++i)
                for (uint32_t j = 0; j < rings; ++j) {
                        const uint32_t k = i * (rings + 1) + j;
                        quad(b, k, k + rings + 1, k + rings + 2, k + 1);
                }
        return;
}

/* An icosphere: the icosahedron's faces split @p levels times, each new point pushed out to the
   sphere. An edge's midpoint is made once: a hash of the edge (its two vertices) to the point. */
static int make_icosphere(builder_t *b, float r, uint32_t levels) {
        const float          t = 1.6180339887f;
        static const float   corners[12][3] = {{-1, 1, 0},  {1, 1, 0},  {-1, -1, 0}, {1, -1, 0}, {0, -1, 1},  {0, 1, 1},
                                               {0, -1, -1}, {0, 1, -1}, {1, 0, -1},  {1, 0, 1},  {-1, 0, -1}, {-1, 0, 1}};
        static const uint8_t faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                             {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                             {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                             {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
        float               *pos = b->positions;
        uint32_t             nv = 12, nf = 20;
        /* (0, ±1, ±t) and its cyclic turns: the table's ±1 in the place of the golden ratio. */
        for (uint32_t i = 0; i < 12; ++i) {
                const float scale[3] = {i < 4   ? 1.0f
                                        : i < 8 ? 1.0f
                                                : t,
                                        i < 4 ? t : 1.0f,
                                        i < 4   ? 1.0f
                                        : i < 8 ? t
                                                : 1.0f};
                const bmds_vec3_t v = bmds_vec3_normalize(
                        bmds_vec3(corners[i][0] * scale[0], corners[i][1] * scale[1], corners[i][2] * scale[2]));
                pos[3 * i] = v.x, pos[3 * i + 1] = v.y, pos[3 * i + 2] = v.z;
        }
        const uint32_t final_faces = 20u << (2 * levels);
        uint32_t       size = 1;
        while (size < final_faces * 2) size <<= 1; /* the last level's edges: 1.5 a face */
        uint16_t *list = (uint16_t *)bmds_mem_alloc(final_faces * 3 * 2 * (uint32_t)sizeof(uint16_t), 0, 0);
        uint32_t *keys = (uint32_t *)bmds_mem_alloc(size * 2 * (uint32_t)sizeof(uint32_t), 0, 0);
        if (list == NULL || keys == NULL) {
                bmds_mem_free(list);
                bmds_mem_free(keys);
                return -BMDS_ENOMEM;
        }
        uint32_t *points = keys + size;
        uint16_t *cur = list, *next = list + final_faces * 3;
        for (uint32_t f = 0; f < 20; ++f)
                for (int k = 0; k < 3; ++k) cur[3 * f + k] = faces[f][k];
        for (uint32_t level = 0; level < levels; ++level) {
                for (uint32_t i = 0; i < size; ++i) keys[i] = 0xffffffffu;
                for (uint32_t f = 0; f < nf; ++f) {
                        uint16_t mid[3];
                        for (int k = 0; k < 3; ++k) {
                                uint32_t a = cur[3 * f + k], c = cur[3 * f + (k + 1) % 3];
                                if (a > c) {
                                        const uint32_t x = a;
                                        a = c, c = x;
                                }
                                const uint32_t key = a << 16 | c;
                                uint32_t       slot = (key * 2654435761u) & (size - 1);
                                while (keys[slot] != 0xffffffffu && keys[slot] != key) slot = (slot + 1) & (size - 1);
                                if (keys[slot] == 0xffffffffu) {
                                        const bmds_vec3_t m = bmds_vec3_normalize(
                                                bmds_vec3_add(bmds_vec3(pos[3 * a], pos[3 * a + 1], pos[3 * a + 2]),
                                                              bmds_vec3(pos[3 * c], pos[3 * c + 1], pos[3 * c + 2])));
                                        pos[3 * nv] = m.x, pos[3 * nv + 1] = m.y, pos[3 * nv + 2] = m.z;
                                        keys[slot] = key;
                                        points[slot] = nv++;
                                }
                                mid[k] = (uint16_t)points[slot];
                        }
                        const uint16_t a = cur[3 * f], bb = cur[3 * f + 1], c = cur[3 * f + 2];
                        const uint16_t out[12] = {a, mid[0], mid[2], bb,     mid[1], mid[0],
                                                  c, mid[2], mid[1], mid[0], mid[1], mid[2]};
                        for (int k = 0; k < 12; ++k) next[12 * f + k] = out[k];
                }
                nf *= 4;
                uint16_t *swap = cur;
                cur = next, next = swap;
        }
        bmds_mem_free(keys);
        for (uint32_t v = 0; v < nv; ++v) {
                const bmds_vec3_t n = bmds_vec3(pos[3 * v], pos[3 * v + 1], pos[3 * v + 2]);
                vertex(b, v, bmds_vec3_scale(n, r), n, 0.5f + atan2f(n.z, n.x) / (2.0f * PI), 0.5f - asinf(n.y) / PI);
        }
        for (uint32_t f = 0; f < nf; ++f) (void)triangle(b, cur[3 * f], cur[3 * f + 1], cur[3 * f + 2], 0, 0);
        bmds_mem_free(list);
        return 0;
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
                case BMDS_3D_SHAPE_CYLINDER:
                case BMDS_3D_SHAPE_CONE:
                        if (segments == 0) segments = 24;
                        if (rings == 0) rings = 1;
                        if (segments < 3 || segments > 1024 || rings > 1024) return -BMDS_EINVAL;
                        vertices = (segments + 1) * (rings + 1) +
                                   (shape->kind == BMDS_3D_SHAPE_CONE ? 1 : 2) * (segments + 2);
                        triangles = 2 * segments * rings + (shape->kind == BMDS_3D_SHAPE_CONE ? 1 : 2) * segments;
                        break;
                case BMDS_3D_SHAPE_ICOSPHERE:
                        if (segments == 0) segments = 2;
                        if (segments > 5) return -BMDS_EINVAL;
                        vertices = 10u * (1u << (2 * segments)) + 2;
                        triangles = 20u << (2 * segments);
                        break;
                case BMDS_3D_SHAPE_TORUS_KNOT:
                        if (segments == 0) segments = 128;
                        if (rings == 0) rings = 8;
                        if (segments < 8 || rings < 3 || segments > 4096 || rings > 256) return -BMDS_EINVAL;
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
        const float r2 = shape->radius2;
        int         made = 0;
        switch (shape->kind) {
                case BMDS_3D_SHAPE_CUBE: make_cube(&b, r); break;
                case BMDS_3D_SHAPE_GRID: make_grid(&b, r, segments); break;
                case BMDS_3D_SHAPE_SPHERE: make_sphere(&b, r, segments, rings); break;
                case BMDS_3D_SHAPE_TORUS: make_torus(&b, r, r2 > 0.0f ? r2 : r / 3.0f, segments, rings); break;
                case BMDS_3D_SHAPE_CYLINDER:
                case BMDS_3D_SHAPE_CONE:
                        make_tube(&b, r, r2 > 0.0f ? r2 : r, segments, rings, shape->kind == BMDS_3D_SHAPE_CONE);
                        break;
                case BMDS_3D_SHAPE_ICOSPHERE: made = make_icosphere(&b, r, segments); break;
                default:
                        make_knot(&b, r, r2 > 0.0f ? r2 : r / 8.0f, segments, rings,
                                  shape->p != 0 || shape->q != 0 ? shape->p : 2,
                                  shape->p != 0 || shape->q != 0 ? shape->q : 3);
                        break;
        }
        if (made < 0) {
                bmds_mem_free(block);
                bmds_mem_free(diagonals);
                return made;
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

/* ---- Extrusion -------------------------------------------------------------------------------- */

static float cross2(const float *xy, uint32_t a, uint32_t b, uint32_t c) {
        return (xy[2 * b] - xy[2 * a]) * (xy[2 * c + 1] - xy[2 * a + 1]) -
               (xy[2 * b + 1] - xy[2 * a + 1]) * (xy[2 * c] - xy[2 * a]);
}

/* Whether point @p p is inside triangle a, b, c (counter-clockwise), its edges included. */
static int inside2(const float *xy, uint32_t p, uint32_t a, uint32_t b, uint32_t c) {
        return cross2(xy, a, b, p) >= 0.0f && cross2(xy, b, c, p) >= 0.0f && cross2(xy, c, a, p) >= 0.0f;
}

/* Ear clipping: @p n points (counter-clockwise) into @p out triangles (3 indices each, n - 2 of
   them), using @p ring (n entries) as scratch. @return 0, or -1 (a polygon that crosses itself). */
static int triangulate(const float *xy, uint32_t n, uint16_t *ring, uint16_t *out) {
        for (uint32_t i = 0; i < n; ++i) ring[i] = (uint16_t)i;
        uint32_t left = n, made = 0, guard = 0;
        uint32_t i = 0;
        while (left > 3) {
                const uint32_t a = ring[(i + left - 1) % left], b = ring[i % left], c = ring[(i + 1) % left];
                int            ear = cross2(xy, a, b, c) >= 0.0f; /* collinear: an ear of no area */
                for (uint32_t k = 0; ear && k < left; ++k) {
                        const uint32_t p = ring[k];
                        if (p != a && p != b && p != c && inside2(xy, p, a, b, c)) ear = 0;
                }
                if (ear) {
                        out[3 * made] = (uint16_t)a, out[3 * made + 1] = (uint16_t)b, out[3 * made + 2] = (uint16_t)c;
                        ++made;
                        for (uint32_t k = i % left; k + 1 < left; ++k) ring[k] = ring[k + 1];
                        --left;
                        guard = 0;
                        if (i >= left) i = 0;
                } else {
                        i = (i + 1) % left;
                        if (++guard > left) return -1; /* no ear left: it crosses itself */
                }
        }
        out[3 * made] = ring[0], out[3 * made + 1] = ring[1], out[3 * made + 2] = ring[2];
        return 0;
}

int bmds_3d_mesh_extrude(const float *xy, uint32_t count, float depth, uint32_t mem_flags, bmds_3d_mesh_t *mesh) {
        if (xy == NULL || mesh == NULL || count < 3 || count > 8000 || !(depth > 0.0f)) return -BMDS_EINVAL;
        /* The points counter-clockwise (a copy, turned if the caller's go the other way). */
        float area = 0.0f;
        for (uint32_t i = 0; i < count; ++i) {
                const uint32_t j = (i + 1) % count;
                area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
        }
        if (!(area != 0.0f)) return -BMDS_EINVAL;
        const uint32_t vertices = 2 * count + 4 * count, triangles = 2 * (count - 2) + 2 * count;
        const size_t   floats = (size_t)vertices * 8;
        const size_t   bytes = floats * sizeof(float) + (size_t)triangles * 3 * sizeof(uint16_t);
        float         *block = (float *)bmds_mem_alloc((uint32_t)bytes, 0, mem_flags);
        uint8_t       *diagonals = (uint8_t *)bmds_mem_alloc(triangles, 0, 0);
        float         *ccw = (float *)bmds_mem_alloc(count * 2 * (uint32_t)sizeof(float), 0, 0);
        uint16_t      *scratch = (uint16_t *)bmds_mem_alloc(count * 4 * (uint32_t)sizeof(uint16_t), 0, 0);
        if (block == NULL || diagonals == NULL || ccw == NULL || scratch == NULL) {
                bmds_mem_free(block), bmds_mem_free(diagonals), bmds_mem_free(ccw), bmds_mem_free(scratch);
                return -BMDS_ENOMEM;
        }
        for (uint32_t i = 0; i < count; ++i) {
                const uint32_t k = area > 0.0f ? i : count - 1 - i;
                ccw[2 * i] = xy[2 * k], ccw[2 * i + 1] = xy[2 * k + 1];
        }
        uint16_t *cap = scratch + count; /* (count - 2) × 3 */
        if (triangulate(ccw, count, scratch, cap) != 0) {
                bmds_mem_free(block), bmds_mem_free(diagonals), bmds_mem_free(ccw), bmds_mem_free(scratch);
                return -BMDS_EINVAL;
        }
        builder_t b = {block,
                       block + 3 * vertices,
                       block + 6 * vertices,
                       (uint16_t *)(void *)(block + floats),
                       diagonals,
                       vertices,
                       0};
        float     lo[2] = {ccw[0], ccw[1]}, hi[2] = {ccw[0], ccw[1]};
        for (uint32_t i = 1; i < count; ++i)
                for (int k = 0; k < 2; ++k) {
                        if (ccw[2 * i + k] < lo[k]) lo[k] = ccw[2 * i + k];
                        if (ccw[2 * i + k] > hi[k]) hi[k] = ccw[2 * i + k];
                }
        const float h = 0.5f * depth;
        /* The caps: front (+Z) and back (-Z); only their outline an edge. */
        for (int back = 0; back < 2; ++back) {
                const uint32_t    base = back ? count : 0;
                const bmds_vec3_t n = bmds_vec3(0.0f, 0.0f, back ? -1.0f : 1.0f);
                for (uint32_t i = 0; i < count; ++i) {
                        const float u = (ccw[2 * i] - lo[0]) / (hi[0] - lo[0] > 0.0f ? hi[0] - lo[0] : 1.0f);
                        const float v = (hi[1] - ccw[2 * i + 1]) / (hi[1] - lo[1] > 0.0f ? hi[1] - lo[1] : 1.0f);
                        vertex(&b, base + i, bmds_vec3(ccw[2 * i], ccw[2 * i + 1], back ? -h : h), n, u, v);
                }
                for (uint32_t t = 0; t + 2 < count; ++t) {
                        const uint32_t i0 = cap[3 * t], i1 = cap[3 * t + 1], i2 = cap[3 * t + 2];
                        const uint32_t k = b.triangle_count;
                        if (!triangle(&b, base + i0, base + i1, base + i2, 0, 0)) continue;
                        /* A side that isn't the outline's (neighbours in the ring) is no edge. */
                        const uint32_t v3[3] = {i0, i1, i2};
                        for (int e = 0; e < 3; ++e) {
                                const uint32_t p = v3[e], q = v3[(e + 1) % 3];
                                if ((p + 1) % count != q && (q + 1) % count != p) mark(&b, k, base + p, base + q);
                        }
                }
        }
        /* The sides: a quad each, faceted. */
        for (uint32_t i = 0; i < count; ++i) {
                const uint32_t    j = (i + 1) % count, v = 2 * count + 4 * i;
                const bmds_vec3_t n = bmds_vec3_normalize(
                        bmds_vec3(ccw[2 * j + 1] - ccw[2 * i + 1], -(ccw[2 * j] - ccw[2 * i]), 0.0f));
                const float u0 = (float)i / (float)count, u1 = (float)(i + 1) / (float)count;
                vertex(&b, v, bmds_vec3(ccw[2 * i], ccw[2 * i + 1], h), n, u0, 0.0f);
                vertex(&b, v + 1, bmds_vec3(ccw[2 * j], ccw[2 * j + 1], h), n, u1, 0.0f);
                vertex(&b, v + 2, bmds_vec3(ccw[2 * j], ccw[2 * j + 1], -h), n, u1, 1.0f);
                vertex(&b, v + 3, bmds_vec3(ccw[2 * i], ccw[2 * i + 1], -h), n, u0, 1.0f);
                quad(&b, v, v + 1, v + 2, v + 3);
        }
        bmds_mem_free(ccw);
        bmds_mem_free(scratch);
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
