/**
 * @file      solid.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's solids (devplan/3d.md §12a): a draw's record (how its shades become pixels), its
 * lights in view space, shades per vertex (Gouraud) or per triangle (flat), each triangle clipped
 * (Sutherland and Hodgman: near, far, and the side planes widened into a guard band), projected to
 * 28.4 fixed point, culled by its winding, fanned into the queue; and the queue drawn far to near
 * (an LSD radix sort on the depth's bits, stable).
 */

#include <math.h>
#include <string.h>
#include "3d_internal.h"

#define GUARD BMDS_3D_GUARD
#define MAX_POLY 9 /* a triangle through six planes */

/* A vertex being clipped: clip space, and its shade. */
typedef struct {
                float x, y, z, w, s;
} clip_vertex_t;

/* ---- Records and lights ----------------------------------------------------------------------- */

static uint32_t rgb_pixel(uint32_t format, uint32_t r, uint32_t g, uint32_t b) {
        if (format == BMDS_PIXEL_RGB565) return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
        return r << 16 | g << 8 | b;
}

static int make_record(const bmds_3d_t *ctx, const bmds_3d_material_t *m, bmds_3d_record_t *rec) {
        const uint32_t format = ctx->target->format;
        memset(rec, 0, sizeof(*rec));
        if ((m->flags & BMDS_3D_UNLIT) != 0 && (m->flags & BMDS_3D_DEPTH_CUE) == 0) {
                rec->first = m->pixel;
                rec->shades[0] = m->pixel;
                return 0;
        }
        if (format == BMDS_PIXEL_L8) {
                if (m->ramp_count == 0 || m->ramp_first + m->ramp_count > 256) return -BMDS_EINVAL;
                rec->first = m->ramp_first;
                rec->top = m->ramp_count - 1;
        } else {
                const uint32_t r = m->rgb >> 16 & 0xffu, g = m->rgb >> 8 & 0xffu, b = m->rgb & 0xffu;
                const uint32_t top = BMDS_3D_RGB_SHADES - 1;
                for (uint32_t i = 0; i <= top; ++i)
                        rec->shades[i] = rgb_pixel(format, r * i / top, g * i / top, b * i / top);
                rec->top = top;
        }
        rec->smooth = m->mode == BMDS_3D_GOURAUD && rec->top > 0;
        return 0;
}

/* @p ctx's lights in view space; @return how many are on. */
static uint32_t view_lights(const bmds_3d_t *ctx, bmds_3d_view_light_t *out) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < BMDS_3D_LIGHTS; ++i) {
                const bmds_3d_light_t *l = &ctx->lights[i];
                if (l->kind == BMDS_3D_LIGHT_OFF) continue;
                bmds_vec3_t v = bmds_vec3(l->x, l->y, l->z);
                const int   in_view = (l->flags & BMDS_3D_LIGHT_VIEW) != 0;
                if (l->kind == BMDS_3D_LIGHT_DIRECTIONAL) {
                        if (!in_view) v = bmds_mat4_transform_vector(&ctx->view, v);
                        v = bmds_vec3_normalize(v);
                } else if (!in_view) {
                        v = bmds_mat4_transform_point(&ctx->view, v);
                }
                out[n].kind = l->kind;
                out[n].x = v.x, out[n].y = v.y, out[n].z = v.z;
                out[n].intensity = l->intensity;
                ++n;
        }
        return n;
}

/* The light at view-space point @p p with unit normal @p n, 0..1. */
static float light_at(const bmds_3d_t *ctx, const bmds_3d_view_light_t *lights, uint32_t count, bmds_vec3_t p,
                      bmds_vec3_t n) {
        float i = ctx->ambient;
        for (uint32_t k = 0; k < count; ++k) {
                bmds_vec3_t l = bmds_vec3(lights[k].x, lights[k].y, lights[k].z);
                if (lights[k].kind == BMDS_3D_LIGHT_POINT) l = bmds_vec3_normalize(bmds_vec3_sub(l, p));
                const float d = bmds_vec3_dot(n, l);
                if (d > 0.0f) i += d * lights[k].intensity;
        }
        return i < 0.0f ? 0.0f : i > 1.0f ? 1.0f : i;
}

static float fog_at(const bmds_3d_t *ctx, float view_z) {
        const float range = ctx->fog_far - ctx->fog_near;
        if (!(range != 0.0f)) return 1.0f;
        const float f = 1.0f - (-view_z - ctx->fog_near) / range;
        return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

/* A shade in 8.8, rounded and kept in range. */
static int16_t shade_fixed(float s) {
        const float v = s * 256.0f + 0.5f;
        return (int16_t)(v < 0.0f ? 0.0f : v > 32767.0f ? 32767.0f : v);
}

/* ---- Clipping --------------------------------------------------------------------------------- */

/* Plane @p k's signed distance for @p v (>= 0: inside). */
static float plane(const clip_vertex_t *v, int k) {
        switch (k) {
                case 0: return v->w + v->z; /* near */
                case 1: return v->w - v->z; /* far */
                case 2: return GUARD * v->w + v->x;
                case 3: return GUARD * v->w - v->x;
                case 4: return GUARD * v->w + v->y;
                default: return GUARD * v->w - v->y;
        }
}

/* Clips the polygon @p in (@p n vertices) against the planes in @p planes into @p out. */
static int clip_polygon(const clip_vertex_t *in, int n, uint32_t planes, clip_vertex_t *out) {
        clip_vertex_t buf[2][MAX_POLY];
        int           cur = 0;
        memcpy(buf[0], in, sizeof(clip_vertex_t) * (size_t)n);
        for (int k = 0; k < 6 && n > 0; ++k) {
                if ((planes & (1u << k)) == 0) continue;
                const clip_vertex_t *src = buf[cur];
                clip_vertex_t       *dst = buf[cur ^ 1];
                int                  m = 0;
                for (int i = 0; i < n; ++i) {
                        const clip_vertex_t *a = &src[i], *b = &src[(i + 1) % n];
                        const float          da = plane(a, k), db = plane(b, k);
                        if (da >= 0.0f && m < MAX_POLY) dst[m++] = *a;
                        if ((da >= 0.0f) != (db >= 0.0f) && m < MAX_POLY) {
                                const float    t = da / (da - db);
                                clip_vertex_t *v = &dst[m++];
                                v->x = a->x + (b->x - a->x) * t;
                                v->y = a->y + (b->y - a->y) * t;
                                v->z = a->z + (b->z - a->z) * t;
                                v->w = a->w + (b->w - a->w) * t;
                                v->s = a->s + (b->s - a->s) * t;
                        }
                }
                n = m;
                cur ^= 1;
        }
        memcpy(out, buf[cur], sizeof(clip_vertex_t) * (size_t)n);
        return n;
}

/* ---- The queue -------------------------------------------------------------------------------- */

/* A float's bits in an order unsigned comparison keeps; then flipped, so far sorts first. */
static uint32_t depth_key(float depth) {
        uint32_t u;
        memcpy(&u, &depth, sizeof(u));
        u = (u & 0x80000000u) != 0 ? ~u : u | 0x80000000u;
        return ~u;
}

/* Sorts @p n keys and their order, least significant byte first (stable); the results in *k, *o. */
static void radix(uint32_t **k, uint32_t **k2, uint16_t **o, uint16_t **o2, uint32_t n) {
        for (int shift = 0; shift < 32; shift += 8) {
                uint32_t count[257];
                memset(count, 0, sizeof(count));
                const uint32_t *a = *k;
                for (uint32_t i = 0; i < n; ++i) ++count[((a[i] >> shift) & 0xffu) + 1];
                for (int b = 0; b < 256; ++b) count[b + 1] += count[b];
                for (uint32_t i = 0; i < n; ++i) {
                        const uint32_t at = count[(a[i] >> shift) & 0xffu]++;
                        (*k2)[at] = a[i];
                        (*o2)[at] = (*o)[i];
                }
                uint32_t *tk = *k;
                *k = *k2, *k2 = tk;
                uint16_t *to = *o;
                *o = *o2, *o2 = to;
        }
        return;
}

void bmds_3d_flush(bmds_3d_t *ctx) {
        const uint32_t n = ctx->tri_count;
        if (n != 0 && ctx->target != NULL) {
                uint32_t *k = ctx->keys, *k2 = ctx->keys2;
                uint16_t *o = ctx->order, *o2 = ctx->order2;
                for (uint32_t i = 0; i < n; ++i) o[i] = (uint16_t)i;
                radix(&k, &k2, &o, &o2, n); /* by triangle */
                if (ctx->sort_order == BMDS_3D_ORDER_DRAWS) {
                        /* Then by draw, stably: each draw's triangles stay in their order. */
                        for (uint32_t i = 0; i < n; ++i) k[i] = ctx->records[ctx->tris[o[i]].draw].key;
                        radix(&k, &k2, &o, &o2, n);
                }
                bmds_3d_area_t area;
                if (bmds_3d_area(ctx, &area)) {
                        bmds_3d_bounds_t bounds = {0, 0, 0, 0};
                        for (uint32_t i = 0; i < n; ++i) {
                                const bmds_3d_tri_t *t = &ctx->tris[o[i]];
                                const uint32_t       px =
                                        bmds_3d_triangle(ctx->target, &area, &ctx->records[t->draw], t, &bounds);
                                if (px != 0) ++ctx->stats.drawn;
                                ctx->stats.pixels += px;
                        }
                        bmds_3d_damage(ctx, &bounds);
                }
        }
        ctx->tri_count = 0;
        ctx->record_count = 0;
        return;
}

/* Room for a record; a full list (or queue) is drawn first. @return its index. */
static uint32_t add_record(bmds_3d_t *ctx, const bmds_3d_record_t *rec) {
        if (ctx->record_count == ctx->max_records) {
                ++ctx->stats.flushes;
                bmds_3d_flush(ctx);
        }
        ctx->records[ctx->record_count] = *rec;
        return ctx->record_count++;
}

/* Queues the triangle @p a, @p b, @p c (28.4, shades 8.8) at @p key with record @p *draw. */
static void push(bmds_3d_t *ctx, const int32_t *xy, const int16_t *s, int i0, int i1, int i2, uint32_t key,
                 uint32_t *draw) {
        if (ctx->tri_count == ctx->max_tris) {
                /* Drawn now; this draw's record kept for the rest of it. */
                const bmds_3d_record_t rec = ctx->records[*draw];
                ++ctx->stats.flushes;
                bmds_3d_flush(ctx);
                ctx->records[0] = rec;
                ctx->record_count = 1;
                *draw = 0;
        }
        bmds_3d_tri_t *t = &ctx->tris[ctx->tri_count];
        const int      idx[3] = {i0, i1, i2};
        for (int k = 0; k < 3; ++k) {
                t->x[k] = xy[2 * idx[k]];
                t->y[k] = xy[2 * idx[k] + 1];
                t->s[k] = s[idx[k]];
        }
        t->draw = (uint16_t)*draw;
        ctx->keys[ctx->tri_count] = key;
        ++ctx->tri_count;
        ++ctx->stats.triangles;
        return;
}

int bmds_3d_queue_solid(bmds_3d_t *ctx, const bmds_3d_mesh_t *mesh, const bmds_3d_material_t *material,
                        const bmds_mat4_t *mv) {
        bmds_3d_record_t rec;
        const int        e = make_record(ctx, material, &rec);
        if (e < 0) return e;
        /* The draw's depth: its bounds' centre, or its vertices' mean. */
        float middle = 0.0f;
        if (mesh->bounds[3] > 0.0f) {
                middle = -bmds_mat4_transform_point(mv, bmds_vec3(mesh->bounds[0], mesh->bounds[1], mesh->bounds[2])).z;
        } else if (mesh->vertex_count != 0) {
                for (uint32_t v = 0; v < mesh->vertex_count; ++v) middle -= ctx->vertices[v].vz;
                middle /= (float)mesh->vertex_count;
        }
        rec.key = depth_key(middle);
        uint32_t draw = add_record(ctx, &rec);

        const int            unlit = (material->flags & BMDS_3D_UNLIT) != 0;
        const int            fog = (material->flags & BMDS_3D_DEPTH_CUE) != 0;
        const int            cull = (material->flags & BMDS_3D_CULL_BACK) != 0;
        const float          top = (float)rec.top;
        bmds_3d_view_light_t lights[BMDS_3D_LIGHTS];
        const uint32_t       nlights = unlit ? 0 : view_lights(ctx, lights);

        /* Gouraud's shades per vertex, from the mesh's normals. */
        const int per_vertex = rec.smooth && (unlit || mesh->normals != NULL);
        if (per_vertex) {
                const float *m = mv->m;
                for (uint32_t v = 0; v < mesh->vertex_count; ++v) {
                        bmds_3d_vertex_t *x = &ctx->vertices[v];
                        float             i = 1.0f;
                        if (!unlit) {
                                const float      *nn = mesh->normals + 3 * v;
                                const bmds_vec3_t n =
                                        bmds_vec3_normalize(bmds_vec3(m[0] * nn[0] + m[4] * nn[1] + m[8] * nn[2],
                                                                      m[1] * nn[0] + m[5] * nn[1] + m[9] * nn[2],
                                                                      m[2] * nn[0] + m[6] * nn[1] + m[10] * nn[2]));
                                i = light_at(ctx, lights, nlights, bmds_vec3(x->vx, x->vy, x->vz), n);
                        }
                        if (fog) i *= fog_at(ctx, x->vz);
                        x->s = i * top;
                }
        }

        if (!per_vertex) ctx->records[draw].smooth = 0; /* a shade a triangle: flat spans */
        const int                ortho = ctx->projection.m[11] == 0.0f;
        const bmds_3d_viewport_t vp = bmds_3d_viewport(ctx);

        for (uint32_t t = 0; t < mesh->triangle_count; ++t) {
                const uint16_t *ix = mesh->triangles + 3 * t;
                if (ix[0] >= mesh->vertex_count || ix[1] >= mesh->vertex_count || ix[2] >= mesh->vertex_count) continue;
                const bmds_3d_vertex_t *p[3] = {&ctx->vertices[ix[0]], &ctx->vertices[ix[1]], &ctx->vertices[ix[2]]};
                if ((p[0]->oc & p[1]->oc & p[2]->oc) != 0) continue; /* wholly outside one plane */
                const uint32_t any = p[0]->oc | p[1]->oc | p[2]->oc;
                const float    depth = -(p[0]->vz + p[1]->vz + p[2]->vz) * (1.0f / 3.0f);
                if (any == 0 && cull) {
                        /* Placed already: a back face goes before it is lit. */
                        const int64_t w = ((int64_t)p[1]->fx - p[0]->fx) * ((int64_t)p[2]->fy - p[0]->fy) -
                                          ((int64_t)p[2]->fx - p[0]->fx) * ((int64_t)p[1]->fy - p[0]->fy);
                        if (w >= 0) { /* y down: a front face winds negative */
                                if (w > 0) ++ctx->stats.backfaces;
                                continue;
                        }
                }

                /* The shades. */
                float shade[3];
                if (per_vertex) {
                        for (int k = 0; k < 3; ++k) shade[k] = p[k]->s;
                } else {
                        float i = 1.0f;
                        if (!unlit) {
                                const bmds_vec3_t a = bmds_vec3(p[0]->vx, p[0]->vy, p[0]->vz);
                                const bmds_vec3_t n = bmds_vec3_normalize(
                                        bmds_vec3_cross(bmds_vec3_sub(bmds_vec3(p[1]->vx, p[1]->vy, p[1]->vz), a),
                                                        bmds_vec3_sub(bmds_vec3(p[2]->vx, p[2]->vy, p[2]->vz), a)));
                                const bmds_vec3_t centre =
                                        bmds_vec3((p[0]->vx + p[1]->vx + p[2]->vx) * (1.0f / 3.0f),
                                                  (p[0]->vy + p[1]->vy + p[2]->vy) * (1.0f / 3.0f), -depth);
                                /* Lit on the side the viewer sees (two-sided, and right through a
                                   mirroring projection). */
                                const bmds_vec3_t eye = ortho ? bmds_vec3(0, 0, 1) : bmds_vec3_scale(centre, -1.0f);
                                i = light_at(ctx, lights, nlights, centre,
                                             bmds_vec3_dot(n, eye) < 0.0f ? bmds_vec3_scale(n, -1.0f) : n);
                        }
                        if (fog) i *= fog_at(ctx, -depth);
                        shade[0] = shade[1] = shade[2] = i * top;
                }

                int32_t xy[2 * MAX_POLY];
                int16_t sh[MAX_POLY];
                int     n = 3;
                if (any == 0) {
                        /* Inside the guard band: placed already (bmds_3d_screen()). */
                        for (int k = 0; k < 3; ++k) {
                                xy[2 * k] = p[k]->fx;
                                xy[2 * k + 1] = p[k]->fy;
                                sh[k] = shade_fixed(shade[k]);
                        }
                } else {
                        clip_vertex_t poly[MAX_POLY];
                        for (int k = 0; k < 3; ++k) {
                                poly[k].x = p[k]->cx, poly[k].y = p[k]->cy, poly[k].z = p[k]->cz, poly[k].w = p[k]->cw;
                                poly[k].s = shade[k];
                        }
                        n = clip_polygon(poly, 3, any, poly);
                        if (n < 3) continue;
                        ++ctx->stats.clipped;
                        int bad = 0;
                        for (int k = 0; k < n && !bad; ++k) {
                                if (!(poly[k].w > 0.0f)) {
                                        bad = 1;
                                        break;
                                }
                                const float inv = 1.0f / poly[k].w;
                                const float sx = vp.x + (poly[k].x * inv + 1.0f) * 0.5f * vp.w;
                                const float sy = vp.y + (1.0f - poly[k].y * inv) * 0.5f * vp.h;
                                /* Inside the guard band by clipping; a NaN fails here. */
                                if (!(fabsf(sx) < 1e6f) || !(fabsf(sy) < 1e6f))
                                        bad = 1;
                                else
                                        xy[2 * k] = bmds_3d_fixed(sx), xy[2 * k + 1] = bmds_3d_fixed(sy);
                                sh[k] = shade_fixed(poly[k].s);
                        }
                        if (bad) continue;
                }

                /* Wound (y down: a front face winds negative), fanned. */
                int64_t area2 = 0;
                for (int k = 0; k < n; ++k) {
                        const int j2 = k + 1 == n ? 0 : k + 1;
                        area2 += (int64_t)xy[2 * k] * xy[2 * j2 + 1] - (int64_t)xy[2 * j2] * xy[2 * k + 1];
                }
                if (area2 == 0) continue;
                if (cull && area2 > 0) {
                        ++ctx->stats.backfaces;
                        continue;
                }
                const uint32_t key = depth_key(depth);
                for (int k = 1; k + 1 < n; ++k) push(ctx, xy, sh, 0, k, k + 1, key, &draw);
        }
        return 0;
}
