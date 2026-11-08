/**
 * @file      context.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's contexts (devplan/3d.md §4b, §4e-§4f): the target and viewport, the matrices,
 * drawing meshes (culled whole by their bounds, transformed once, then as points or wire),
 * projecting points for the caller's own drawing, the counters.
 */

#include <math.h>
#include <string.h>
#include "3d_internal.h"

#define DEFAULT_MAX_VERTICES 4096u
#define MAX_POINT_SIZE 64u
#define DEFAULT_MAX_TRIANGLES 2048u
#define DEFAULT_MAX_DRAWS 64u

/* ---- The context ------------------------------------------------------------------------------ */

/* @p bytes of scratch with @p mem_flags, else (internal RAM short) in PSRAM. */
static void *scratch(uint32_t bytes, uint32_t mem_flags) {
        void *p = bmds_mem_alloc(bytes, 0, mem_flags);
        return p != NULL || mem_flags == 0 ? p : bmds_mem_alloc(bytes, 0, 0);
}

int bmds_3d_create(const bmds_3d_config_t *config, bmds_3d_t **out) {
        if (out == NULL) return -BMDS_EINVAL;
        *out = NULL;
        if (config != NULL && config->size < sizeof(bmds_3d_config_t)) return -BMDS_EINVAL;
        const uint32_t max = config != NULL && config->max_vertices != 0 ? config->max_vertices : DEFAULT_MAX_VERTICES;
        const uint32_t mem_flags = config != NULL ? config->mem_flags : 0;
        const uint32_t tris =
                config != NULL && config->max_triangles != 0 ? config->max_triangles : DEFAULT_MAX_TRIANGLES;
        const uint32_t draws = config != NULL && config->max_draws != 0 ? config->max_draws : DEFAULT_MAX_DRAWS;
        if (max > 65535u || tris > 65535u || draws > 65535u || (mem_flags & ~BMDS_MEM_FAST) != 0) return -BMDS_EINVAL;
        bmds_3d_t *ctx = (bmds_3d_t *)bmds_mem_alloc(sizeof(bmds_3d_t), 0, 0);
        if (ctx == NULL) return -BMDS_ENOMEM;
        memset(ctx, 0, sizeof(*ctx));
        const uint32_t bytes = max * (uint32_t)sizeof(bmds_3d_vertex_t);
        ctx->vertices = (bmds_3d_vertex_t *)bmds_mem_alloc(bytes, 0, mem_flags);
        /* Internal RAM if it fits, as the config says; else PSRAM. */
        if (ctx->vertices == NULL && mem_flags != 0) ctx->vertices = (bmds_3d_vertex_t *)bmds_mem_alloc(bytes, 0, 0);
        /* The solids' queue in PSRAM: written once a triangle, read once at the end. Internal RAM is
           the kernel's too (a full-screen presenter needs ~30 KB of it); the vertices, read several
           times a draw, are what gains from it (3d.md §13, step 3). */
        ctx->tris = (bmds_3d_tri_t *)scratch(tris * (uint32_t)sizeof(bmds_3d_tri_t), 0);
        ctx->keys = (uint32_t *)scratch(tris * 2 * (uint32_t)sizeof(uint32_t), 0);
        ctx->order = (uint16_t *)scratch(tris * 2 * (uint32_t)sizeof(uint16_t), 0);
        ctx->records = (bmds_3d_record_t *)scratch(draws * (uint32_t)sizeof(bmds_3d_record_t), 0);
        ctx->ranked = (uint16_t *)scratch(draws * (uint32_t)sizeof(uint16_t), 0);
        if (ctx->vertices == NULL || ctx->tris == NULL || ctx->keys == NULL || ctx->order == NULL ||
            ctx->records == NULL || ctx->ranked == NULL) {
                bmds_3d_destroy(ctx);
                return -BMDS_ENOMEM;
        }
        ctx->keys2 = ctx->keys + tris;
        ctx->order2 = ctx->order + tris;
        ctx->max_tris = tris;
        ctx->max_records = draws;
        ctx->max_vertices = max;
        if (config != NULL && config->cores > 2) {
                bmds_3d_destroy(ctx);
                return -BMDS_EINVAL;
        }
        if (config != NULL && config->cores == 2) (void)bmds_3d_helper_start(ctx); /* else one core */
        ctx->ambient = 0.25f;
        ctx->lights[0] = (bmds_3d_light_t){
                sizeof(bmds_3d_light_t), BMDS_3D_LIGHT_DIRECTIONAL, BMDS_3D_LIGHT_VIEW, -0.4f, 0.6f, 1.0f, 0.8f};
        bmds_mat4_identity(&ctx->projection);
        bmds_mat4_identity(&ctx->view);
        bmds_mat4_identity(&ctx->model[0]);
        ctx->fog_near = 1.0f;
        ctx->fog_far = 100.0f;
        ctx->viewport_whole = 1;
        ctx->stats.size = sizeof(ctx->stats);
        *out = ctx;
        return 0;
}

void bmds_3d_destroy(bmds_3d_t *ctx) {
        if (ctx == NULL) return;
        bmds_3d_helper_stop(ctx);
        bmds_mem_free(ctx->vertices);
        bmds_mem_free(ctx->tris);
        bmds_mem_free(ctx->keys);
        bmds_mem_free(ctx->order);
        bmds_mem_free(ctx->records);
        bmds_mem_free(ctx->ranked);
        bmds_mem_free(ctx->tuvs);
        bmds_mem_free(ctx->envuv);
        bmds_mem_free(ctx);
        return;
}

void bmds_3d_set_target(bmds_3d_t *ctx, bmds_canvas_t *canvas) {
        if (ctx->tri_count != 0 && canvas != ctx->target) {
                ++ctx->stats.flushes;
                bmds_3d_flush(ctx); /* the queue is the old target's */
        }
        ctx->target = canvas;
        ctx->viewport_whole = 1;
        return;
}

void bmds_3d_set_viewport(bmds_3d_t *ctx, int32_t x, int32_t y, int32_t width, int32_t height) {
        ctx->vx = x;
        ctx->vy = y;
        ctx->vw = width;
        ctx->vh = height;
        ctx->viewport_whole = 0;
        return;
}

void bmds_3d_set_projection(bmds_3d_t *ctx, const bmds_mat4_t *m) {
        ctx->projection = *m;
        return;
}

void bmds_3d_set_view(bmds_3d_t *ctx, const bmds_mat4_t *m) {
        ctx->view = *m;
        return;
}

int bmds_3d_set_light(bmds_3d_t *ctx, uint32_t index, const bmds_3d_light_t *light) {
        if (index >= BMDS_3D_LIGHTS) return -BMDS_EINVAL;
        if (light == NULL) {
                memset(&ctx->lights[index], 0, sizeof(ctx->lights[index]));
                return 0;
        }
        if (light->size < sizeof(bmds_3d_light_t) || light->kind > BMDS_3D_LIGHT_POINT ||
            (light->flags & ~BMDS_3D_LIGHT_VIEW) != 0)
                return -BMDS_EINVAL;
        ctx->lights[index] = *light;
        ctx->lights[index].size = sizeof(bmds_3d_light_t);
        return 0;
}

int bmds_3d_set_order(bmds_3d_t *ctx, uint32_t order) {
        if (order > BMDS_3D_ORDER_TRIANGLES) return -BMDS_EINVAL;
        ctx->sort_order = order;
        return 0;
}

void bmds_3d_set_ambient(bmds_3d_t *ctx, float ambient) {
        ctx->ambient = ambient;
        return;
}

void bmds_3d_set_fog(bmds_3d_t *ctx, float near_z, float far_z) {
        ctx->fog_near = near_z;
        ctx->fog_far = far_z;
        return;
}

int bmds_3d_push(bmds_3d_t *ctx) {
        if (ctx->depth + 1 >= BMDS_3D_STACK) return -BMDS_ENOSPC;
        ctx->model[ctx->depth + 1] = ctx->model[ctx->depth];
        ++ctx->depth;
        return 0;
}

int bmds_3d_pop(bmds_3d_t *ctx) {
        if (ctx->depth == 0) return -BMDS_EINVAL;
        --ctx->depth;
        return 0;
}

void bmds_3d_load(bmds_3d_t *ctx, const bmds_mat4_t *m) {
        ctx->model[ctx->depth] = *m;
        return;
}

void bmds_3d_mult(bmds_3d_t *ctx, const bmds_mat4_t *m) {
        bmds_mat4_multiply(&ctx->model[ctx->depth], &ctx->model[ctx->depth], m);
        return;
}

void bmds_3d_translate(bmds_3d_t *ctx, float x, float y, float z) {
        bmds_mat4_t m;
        bmds_mat4_translation(&m, x, y, z);
        bmds_3d_mult(ctx, &m);
        return;
}

void bmds_3d_rotate(bmds_3d_t *ctx, bmds_vec3_t axis, float angle) {
        bmds_mat4_t m;
        bmds_mat4_rotation(&m, axis, angle);
        bmds_3d_mult(ctx, &m);
        return;
}

void bmds_3d_scale(bmds_3d_t *ctx, float x, float y, float z) {
        bmds_mat4_t m;
        bmds_mat4_scaling(&m, x, y, z);
        bmds_3d_mult(ctx, &m);
        return;
}

const bmds_mat4_t *bmds_3d_model(const bmds_3d_t *ctx) {
        return &ctx->model[ctx->depth];
}

void bmds_3d_begin(bmds_3d_t *ctx) {
        memset(&ctx->stats, 0, sizeof(ctx->stats));
        ctx->stats.size = sizeof(ctx->stats);
        return;
}

void bmds_3d_end(bmds_3d_t *ctx) {
        bmds_3d_flush(ctx);
        return;
}

void bmds_3d_stats(const bmds_3d_t *ctx, bmds_3d_stats_t *stats) {
        if (stats == NULL || stats->size == 0) return;
        const uint32_t n = stats->size < sizeof(ctx->stats) ? stats->size : (uint32_t)sizeof(ctx->stats);
        const uint32_t size = stats->size;
        memcpy(stats, &ctx->stats, n);
        stats->size = size;
        return;
}

/* ---- Shared pieces ---------------------------------------------------------------------------- */

int bmds_3d_area(const bmds_3d_t *ctx, bmds_3d_area_t *area) {
        const bmds_canvas_t *c = ctx->target;
        if (c == NULL) return 0;
        int32_t       x0 = ctx->viewport_whole ? 0 : ctx->vx, y0 = ctx->viewport_whole ? 0 : ctx->vy;
        int32_t       x1 = ctx->viewport_whole ? c->width : ctx->vx + ctx->vw;
        int32_t       y1 = ctx->viewport_whole ? c->height : ctx->vy + ctx->vh;
        const int32_t kx1 = c->clip.x + (int32_t)c->clip.width, ky1 = c->clip.y + (int32_t)c->clip.height;
        if (x0 < c->clip.x) x0 = c->clip.x;
        if (y0 < c->clip.y) y0 = c->clip.y;
        if (x1 > kx1) x1 = kx1;
        if (y1 > ky1) y1 = ky1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > c->width) x1 = c->width;
        if (y1 > c->height) y1 = c->height;
        if (x1 <= x0 || y1 <= y0) return 0;
        area->ix0 = x0;
        area->iy0 = y0;
        area->ix1 = x1;
        area->iy1 = y1;
        area->x0 = (float)x0;
        area->y0 = (float)y0;
        area->x1 = (float)x1;
        area->y1 = (float)y1;
        return 1;
}

void bmds_3d_matrices(const bmds_3d_t *ctx, bmds_mat4_t *mvp, bmds_mat4_t *mv) {
        bmds_mat4_multiply(mv, &ctx->view, &ctx->model[ctx->depth]);
        bmds_mat4_multiply(mvp, &ctx->projection, mv);
        return;
}

void bmds_3d_transform_vertex(const bmds_mat4_t *mvp, const bmds_mat4_t *mv, const float *p, bmds_3d_vertex_t *out) {
        const float  x = p[0], y = p[1], z = p[2];
        const float *a = mvp->m, *b = mv->m;
        out->cx = a[0] * x + a[4] * y + a[8] * z + a[12];
        out->cy = a[1] * x + a[5] * y + a[9] * z + a[13];
        out->cz = a[2] * x + a[6] * y + a[10] * z + a[14];
        out->cw = a[3] * x + a[7] * y + a[11] * z + a[15];
        out->vx = b[0] * x + b[4] * y + b[8] * z + b[12];
        out->vy = b[1] * x + b[5] * y + b[9] * z + b[13];
        out->vz = b[2] * x + b[6] * y + b[10] * z + b[14];
        return;
}

bmds_3d_viewport_t bmds_3d_viewport(const bmds_3d_t *ctx) {
        const bmds_canvas_t *c = ctx->target;
        bmds_3d_viewport_t   vp;
        vp.x = (float)(ctx->viewport_whole ? 0 : ctx->vx);
        vp.y = (float)(ctx->viewport_whole ? 0 : ctx->vy);
        vp.w = (float)(ctx->viewport_whole ? (c != NULL ? c->width : 0) : ctx->vw);
        vp.h = (float)(ctx->viewport_whole ? (c != NULL ? c->height : 0) : ctx->vh);
        return vp;
}

void bmds_3d_screen(const bmds_3d_viewport_t *vp, bmds_3d_vertex_t *v) {
        const float w = v->cw, g = BMDS_3D_GUARD * w;
        uint32_t    oc = 0;
        if (!(v->cz >= -w)) oc |= BMDS_3D_OUT_NEAR; /* NaN: outside */
        if (!(v->cz <= w)) oc |= BMDS_3D_OUT_FAR;
        if (!(v->cx >= -g)) oc |= 0x4u;
        if (!(v->cx <= g)) oc |= 0x8u;
        if (!(v->cy >= -g)) oc |= 0x10u;
        if (!(v->cy <= g)) oc |= 0x20u;
        v->oc = oc;
        if ((oc & BMDS_3D_OUT_NEAR) != 0 || !(w > 0.0f)) return;
        const float inv = 1.0f / w;
        v->sx = vp->x + (v->cx * inv + 1.0f) * 0.5f * vp->w;
        v->sy = vp->y + (1.0f - v->cy * inv) * 0.5f * vp->h;
        if (oc == 0) {
                v->fx = bmds_3d_fixed(v->sx);
                v->fy = bmds_3d_fixed(v->sy);
        }
        return;
}

int bmds_3d_check_material(const bmds_3d_material_t *m) {
        /* An older source's material (before `texture`) is whole as far as it goes. */
        if (m == NULL || m->size < BMDS_3D_MATERIAL_SIZE_1) return -BMDS_EINVAL;
        if (m->mode < BMDS_3D_POINTS || m->mode > BMDS_3D_ENVMAP) return -BMDS_EINVAL;
        if ((m->flags & ~(BMDS_3D_CULL_BACK | BMDS_3D_DEPTH_CUE | BMDS_3D_UNLIT | BMDS_3D_PERSPECTIVE)) != 0)
                return -BMDS_EINVAL;
        if (m->point_size > MAX_POINT_SIZE) return -BMDS_EINVAL;
        return 0;
}

/* Whether @p v is a power of two (1 included). */
static int is_pow2(uint32_t v) {
        return v != 0 && (v & (v - 1)) == 0;
}

int bmds_3d_check_texture(const bmds_3d_material_t *m) {
        const bmds_game_image_t *t = m->texture;
        if (t == NULL || t->pixels == NULL || t->format != BMDS_PIXEL_L8 || t->width <= 0 || t->height <= 0)
                return -BMDS_EINVAL;
        if (!is_pow2((uint32_t)t->width) || !is_pow2((uint32_t)t->height) || !is_pow2(t->stride) ||
            t->stride < (uint32_t)t->width || t->stride > 65536u)
                return -BMDS_EINVAL;
        const int lit = (m->mode == BMDS_3D_TEXTURED_FLAT || m->mode == BMDS_3D_TEXTURED_GOURAUD) &&
                        (m->flags & BMDS_3D_UNLIT) == 0;
        if ((lit || (m->flags & BMDS_3D_DEPTH_CUE) != 0) && m->light_table == NULL) return -BMDS_EINVAL;
        return 0;
}

void bmds_3d_full_material(const bmds_3d_material_t *m, bmds_3d_material_t *out) {
        const size_t n = m->size < sizeof(*out) ? m->size : sizeof(*out);
        memset(out, 0, sizeof(*out));
        memcpy(out, m, n);
        out->size = sizeof(*out);
        return;
}

static uint32_t rgb_pixel(uint32_t format, uint32_t r, uint32_t g, uint32_t b) {
        if (format == BMDS_PIXEL_RGB565) return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
        return r << 16 | g << 8 | b; /* BGR24's value: 0xRRGGBB */
}

int bmds_3d_ink(const bmds_3d_t *ctx, const bmds_3d_material_t *m, bmds_3d_ink_t *ink) {
        const uint32_t format = ctx->target->format;
        ink->count = 1;
        ink->shades[0] = m->pixel;
        ink->fog_near = ctx->fog_near;
        const float range = ctx->fog_far - ctx->fog_near;
        ink->fog_scale = range != 0.0f ? 1.0f / range : 0.0f;
        if ((m->flags & BMDS_3D_DEPTH_CUE) == 0) return 0;
        if (format == BMDS_PIXEL_L8) {
                if (m->ramp_count == 0 || m->ramp_first + m->ramp_count > 256) return -BMDS_EINVAL;
                ink->count = m->ramp_count;
                for (uint32_t i = 0; i < m->ramp_count; ++i) ink->shades[i] = m->ramp_first + i;
                return 0;
        }
        ink->count = BMDS_3D_RGB_SHADES;
        const uint32_t r = m->rgb >> 16 & 0xffu, g = m->rgb >> 8 & 0xffu, b = m->rgb & 0xffu,
                       top = BMDS_3D_RGB_SHADES - 1;
        for (uint32_t i = 0; i < BMDS_3D_RGB_SHADES; ++i)
                ink->shades[i] = rgb_pixel(format, r * i / top, g * i / top, b * i / top);
        return 0;
}

void bmds_3d_damage(bmds_3d_t *ctx, const bmds_3d_bounds_t *bounds) {
        if (bounds->x1 <= bounds->x0 || bounds->y1 <= bounds->y0) return;
        const bmds_rect_t r = {bounds->x0, bounds->y0, (uint32_t)(bounds->x1 - bounds->x0),
                               (uint32_t)(bounds->y1 - bounds->y0)};
        bmds_canvas_damage(ctx->target, &r);
        return;
}

/* ---- Drawing meshes --------------------------------------------------------------------------- */

/* Whether the sphere (@p b: centre, radius) is wholly outside one of @p mvp's six planes. */
static int outside(const bmds_mat4_t *mvp, const float *b) {
        const float *m = mvp->m;
        for (int plane = 0; plane < 6; ++plane) {
                /* Row 3 plus or minus row 0, 1 or 2 (Gribb and Hartmann). */
                const int   row = plane >> 1;
                const float sign = (plane & 1) != 0 ? -1.0f : 1.0f;
                const float a = m[3] + sign * m[row], bb = m[7] + sign * m[4 + row];
                const float c = m[11] + sign * m[8 + row], d = m[15] + sign * m[12 + row];
                const float len = sqrtf(a * a + bb * bb + c * c);
                if (!(len > 0.0f)) continue;
                if ((a * b[0] + bb * b[1] + c * b[2] + d) / len < -b[3]) return 1;
        }
        return 0;
}

/* Whether triangle @p t faces the viewer (view space; @p ortho: looking down -Z everywhere). */
static int is_front(const bmds_3d_t *ctx, const bmds_3d_mesh_t *mesh, uint32_t t, int ortho) {
        const uint16_t         *i = mesh->triangles + 3 * t;
        const bmds_3d_vertex_t *a = &ctx->vertices[i[0]], *b = &ctx->vertices[i[1]], *c = &ctx->vertices[i[2]];
        const bmds_vec3_t       pa = bmds_vec3(a->vx, a->vy, a->vz);
        const bmds_vec3_t       n = bmds_vec3_cross(bmds_vec3_sub(bmds_vec3(b->vx, b->vy, b->vz), pa),
                                                    bmds_vec3_sub(bmds_vec3(c->vx, c->vy, c->vz), pa));
        return ortho ? n.z > 0.0f : bmds_vec3_dot(n, pa) < 0.0f;
}

static int check_mesh(const bmds_3d_mesh_t *mesh) {
        if (mesh == NULL || mesh->size < sizeof(bmds_3d_mesh_t) || mesh->positions == NULL) return -BMDS_EINVAL;
        if (mesh->triangle_count != 0 && mesh->triangles == NULL) return -BMDS_EINVAL;
        if (mesh->edge_count != 0 && mesh->edges == NULL) return -BMDS_EINVAL;
        return 0;
}

static void draw_wire(bmds_3d_t *ctx, const bmds_3d_mesh_t *mesh, const bmds_3d_material_t *material,
                      const bmds_3d_area_t *area, const bmds_3d_ink_t *ink, bmds_3d_bounds_t *bounds) {
        const int      ortho = ctx->projection.m[11] == 0.0f;
        const int      cull = (material->flags & BMDS_3D_CULL_BACK) != 0;
        const uint32_t n = mesh->vertex_count;
        if (mesh->edge_count != 0) {
                for (uint32_t e = 0; e < mesh->edge_count; ++e) {
                        const uint32_t a = mesh->edges[2 * e], b = mesh->edges[2 * e + 1];
                        if (a >= n || b >= n) continue;
                        if (cull && mesh->edge_faces != NULL) {
                                const uint32_t f1 = mesh->edge_faces[2 * e], f2 = mesh->edge_faces[2 * e + 1];
                                const int      v1 = f1 < mesh->triangle_count && is_front(ctx, mesh, f1, ortho);
                                const int      v2 = f2 < mesh->triangle_count && is_front(ctx, mesh, f2, ortho);
                                if (!v1 && !v2 && (f1 < mesh->triangle_count || f2 < mesh->triangle_count)) continue;
                        }
                        ctx->stats.lines +=
                                (uint32_t)bmds_3d_segment(ctx, area, ink, &ctx->vertices[a], &ctx->vertices[b], bounds);
                }
                return;
        }
        for (uint32_t t = 0; t < mesh->triangle_count; ++t) {
                const uint16_t *i = mesh->triangles + 3 * t;
                if (i[0] >= n || i[1] >= n || i[2] >= n) continue;
                if (cull && !is_front(ctx, mesh, t, ortho)) continue;
                for (int s = 0; s < 3; ++s) {
                        ctx->stats.lines += (uint32_t)bmds_3d_segment(ctx, area, ink, &ctx->vertices[i[s]],
                                                                      &ctx->vertices[i[(s + 1) % 3]], bounds);
                }
        }
        return;
}

int bmds_3d_draw(bmds_3d_t *ctx, const bmds_3d_mesh_t *mesh, const bmds_3d_material_t *given) {
        int e = check_mesh(mesh);
        if (e == 0) e = bmds_3d_check_material(given);
        if (e < 0) return e;
        if (ctx->target == NULL) return -BMDS_EINVAL;
        if (mesh->vertex_count > ctx->max_vertices) return -BMDS_ENOSPC;
        bmds_3d_material_t whole;
        bmds_3d_full_material(given, &whole);
        const bmds_3d_material_t *material = &whole;
        const int                 solid = material->mode >= BMDS_3D_FLAT;
        if (bmds_3d_is_textured(material->mode)) {
                if (ctx->target->format != BMDS_PIXEL_L8) return -BMDS_ENOSYS;
                if (material->mode != BMDS_3D_ENVMAP && mesh->uvs == NULL) return -BMDS_EINVAL;
                e = bmds_3d_check_texture(material);
                if (e < 0) return e;
                if (ctx->tuvs == NULL) {
                        /* Allocated once, at the first textured draw: programs without textures pay
                           nothing for them. */
                        ctx->tuvs =
                                (bmds_3d_tuv_t *)bmds_mem_alloc(ctx->max_tris * (uint32_t)sizeof(bmds_3d_tuv_t), 0, 0);
                        if (ctx->tuvs == NULL) return -BMDS_ENOMEM;
                }
                if (material->mode == BMDS_3D_ENVMAP && mesh->normals != NULL && ctx->envuv == NULL) {
                        ctx->envuv = (float *)bmds_mem_alloc(ctx->max_vertices * 2u * (uint32_t)sizeof(float), 0, 0);
                        if (ctx->envuv == NULL) return -BMDS_ENOMEM;
                }
        }
        bmds_3d_ink_t ink;
        if (!solid) {
                e = bmds_3d_ink(ctx, material, &ink);
                if (e < 0) return e;
        }
        bmds_3d_area_t area;
        if (!bmds_3d_area(ctx, &area)) return 0;

        bmds_mat4_t mvp, mv;
        bmds_3d_matrices(ctx, &mvp, &mv);
        if (mesh->bounds[3] > 0.0f && outside(&mvp, mesh->bounds)) {
                ++ctx->stats.culled;
                return 0;
        }
        ++ctx->stats.meshes;
        const bmds_3d_viewport_t vp = bmds_3d_viewport(ctx);
        for (uint32_t v = 0; v < mesh->vertex_count; ++v) {
                bmds_3d_transform_vertex(&mvp, &mv, mesh->positions + 3 * v, &ctx->vertices[v]);
                bmds_3d_screen(&vp, &ctx->vertices[v]);
        }
        ctx->stats.vertices += mesh->vertex_count;
        if (solid) return bmds_3d_queue_solid(ctx, mesh, material, &mv);

        bmds_3d_bounds_t bounds = {0, 0, 0, 0};
        if (material->mode == BMDS_3D_WIRE) {
                draw_wire(ctx, mesh, material, &area, &ink, &bounds);
        } else {
                const int32_t size = material->point_size != 0 ? (int32_t)material->point_size : 1;
                for (uint32_t v = 0; v < mesh->vertex_count; ++v)
                        ctx->stats.points +=
                                (uint32_t)bmds_3d_point(ctx, &area, &ink, &ctx->vertices[v], size, &bounds);
        }
        bmds_3d_damage(ctx, &bounds);
        return 0;
}

int bmds_3d_lines(bmds_3d_t *ctx, const float *points, uint32_t count, int strip, const bmds_3d_material_t *material) {
        int e = bmds_3d_check_material(material);
        if (e < 0) return e;
        if (ctx->target == NULL || (points == NULL && count != 0)) return -BMDS_EINVAL;
        bmds_3d_ink_t ink;
        e = bmds_3d_ink(ctx, material, &ink);
        if (e < 0) return e;
        bmds_3d_area_t area;
        if (!bmds_3d_area(ctx, &area) || count < 2) return 0;
        bmds_mat4_t mvp, mv;
        bmds_3d_matrices(ctx, &mvp, &mv);
        bmds_3d_bounds_t         bounds = {0, 0, 0, 0};
        bmds_3d_vertex_t         a, b;
        const bmds_3d_viewport_t vp = bmds_3d_viewport(ctx);
        bmds_3d_transform_vertex(&mvp, &mv, points, &a);
        bmds_3d_screen(&vp, &a);
        for (uint32_t i = 1; i < count; ++i) {
                if (!strip && (i & 1u) == 0) {
                        bmds_3d_transform_vertex(&mvp, &mv, points + 3 * i, &a);
                        bmds_3d_screen(&vp, &a);
                        continue;
                }
                bmds_3d_transform_vertex(&mvp, &mv, points + 3 * i, &b);
                bmds_3d_screen(&vp, &b);
                ctx->stats.lines += (uint32_t)bmds_3d_segment(ctx, &area, &ink, &a, &b, &bounds);
                if (strip) a = b;
        }
        ctx->stats.vertices += count;
        bmds_3d_damage(ctx, &bounds);
        return 0;
}

int bmds_3d_points(bmds_3d_t *ctx, const float *points, uint32_t count, const bmds_3d_material_t *material) {
        int e = bmds_3d_check_material(material);
        if (e < 0) return e;
        if (ctx->target == NULL || (points == NULL && count != 0)) return -BMDS_EINVAL;
        bmds_3d_ink_t ink;
        e = bmds_3d_ink(ctx, material, &ink);
        if (e < 0) return e;
        bmds_3d_area_t area;
        if (!bmds_3d_area(ctx, &area)) return 0;
        bmds_mat4_t mvp, mv;
        bmds_3d_matrices(ctx, &mvp, &mv);
        const int32_t    size = material->point_size != 0 ? (int32_t)material->point_size : 1;
        bmds_3d_bounds_t bounds = {0, 0, 0, 0};
        for (uint32_t i = 0; i < count; ++i) {
                bmds_3d_vertex_t v;
                bmds_3d_transform_vertex(&mvp, &mv, points + 3 * i, &v);
                ctx->stats.points += (uint32_t)bmds_3d_point(ctx, &area, &ink, &v, size, &bounds);
        }
        ctx->stats.vertices += count;
        bmds_3d_damage(ctx, &bounds);
        return 0;
}

/* ---- Projecting ------------------------------------------------------------------------------- */

void bmds_3d_project(bmds_3d_t *ctx, const float *points, uint32_t count, bmds_3d_projected_t *out) {
        bmds_mat4_t mvp, mv;
        bmds_3d_matrices(ctx, &mvp, &mv);
        const bmds_canvas_t *c = ctx->target;
        const float vx = (float)(ctx->viewport_whole ? 0 : ctx->vx), vy = (float)(ctx->viewport_whole ? 0 : ctx->vy);
        const float vw = (float)(ctx->viewport_whole ? (c != NULL ? c->width : 0) : ctx->vw);
        const float vh = (float)(ctx->viewport_whole ? (c != NULL ? c->height : 0) : ctx->vh);
        const float focal = ctx->projection.m[5] * vh * 0.5f;
        for (uint32_t i = 0; i < count; ++i) {
                bmds_3d_vertex_t v;
                bmds_3d_transform_vertex(&mvp, &mv, points + 3 * i, &v);
                bmds_3d_projected_t *o = &out[i];
                o->depth = -v.vz;
                if (v.cw > 0.0f) {
                        const float k = 1.0f / v.cw;
                        o->x = vx + (v.cx * k + 1.0f) * 0.5f * vw;
                        o->y = vy + (1.0f - v.cy * k) * 0.5f * vh;
                        o->scale = focal * k;
                        o->visible = v.cx >= -v.cw && v.cx <= v.cw && v.cy >= -v.cw && v.cy <= v.cw && v.cz >= -v.cw &&
                                     v.cz <= v.cw;
                } else {
                        o->x = o->y = o->scale = 0.0f;
                        o->visible = 0;
                }
        }
        return;
}

void bmds_3d_sort_depth(const bmds_3d_projected_t *points, uint16_t *order, uint32_t count) {
        /* Insertion: stable, and quick on last frame's order (nearly sorted). */
        for (uint32_t i = 1; i < count; ++i) {
                const uint16_t k = order[i];
                const float    d = points[k].depth;
                uint32_t       j = i;
                while (j > 0 && points[order[j - 1]].depth < d) {
                        order[j] = order[j - 1];
                        --j;
                }
                order[j] = k;
        }
        return;
}
