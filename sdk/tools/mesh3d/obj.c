/**
 * @file      obj.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-mesh3d's Wavefront OBJ reader: `v`, `vt`, `vn`, `f` (negative indices, polygons fanned),
 * `o` and `g` naming meshes; the rest (materials, smoothing groups, lines) skipped. A face
 * without normals gets smooth ones from the faces round each corner within the crease angle.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mesh3d.h"

#define PI 3.14159265358979

typedef struct {
                float   *data;
                uint32_t count, cap; /* elements of `width` floats */
                uint32_t width;
} floats_t;

/* A group's faces: corners (position, uv, normal: 0-based, -1 for none) and where each face starts. */
typedef struct {
                char     *name;
                int32_t  *corners; /* 3 a corner */
                uint32_t  corner_count, corner_cap;
                uint32_t *faces; /* first corner a face, and one past the last */
                uint32_t  face_count, face_cap;
} group_t;

typedef struct {
                floats_t v, vt, vn;
                group_t *groups;
                uint32_t group_count;
                group_t *current;
} reader_t;

static int push_floats(floats_t *f, const float *x) {
        if (f->count == f->cap) {
                const uint32_t cap = f->cap != 0 ? 2 * f->cap : 1024;
                float         *d = (float *)realloc(f->data, (size_t)cap * f->width * sizeof(float));
                if (d == NULL) return -1;
                f->data = d;
                f->cap = cap;
        }
        memcpy(f->data + (size_t)f->count * f->width, x, f->width * sizeof(float));
        ++f->count;
        return 0;
}

static group_t *group(reader_t *r, const char *name) {
        for (uint32_t i = 0; i < r->group_count; ++i)
                if (strcmp(r->groups[i].name, name) == 0) return &r->groups[i];
        group_t *g = (group_t *)realloc(r->groups, (r->group_count + 1) * sizeof(group_t));
        if (g == NULL) return NULL;
        r->groups = g;
        group_t *n = &g[r->group_count];
        memset(n, 0, sizeof(*n));
        n->name = strdup(name);
        if (n->name == NULL) return NULL;
        ++r->group_count;
        return n;
}

static int push_corner(group_t *g, const int32_t *c) {
        if (g->corner_count == g->corner_cap) {
                const uint32_t cap = g->corner_cap != 0 ? 2 * g->corner_cap : 1024;
                int32_t       *d = (int32_t *)realloc(g->corners, (size_t)cap * 3 * sizeof(int32_t));
                if (d == NULL) return -1;
                g->corners = d;
                g->corner_cap = cap;
        }
        memcpy(g->corners + 3 * g->corner_count++, c, 3 * sizeof(int32_t));
        return 0;
}

static int push_face(group_t *g) {
        if (g->face_count + 2 > g->face_cap) {
                const uint32_t cap = g->face_cap != 0 ? 2 * g->face_cap : 1024;
                uint32_t      *d = (uint32_t *)realloc(g->faces, cap * sizeof(uint32_t));
                if (d == NULL) return -1;
                g->faces = d;
                g->face_cap = cap;
        }
        if (g->face_count == 0) g->faces[g->face_count++] = 0;
        g->faces[g->face_count++] = g->corner_count;
        return 0;
}

static void reader_free(reader_t *r) {
        free(r->v.data), free(r->vt.data), free(r->vn.data);
        for (uint32_t i = 0; i < r->group_count; ++i) {
                free(r->groups[i].name);
                free(r->groups[i].corners);
                free(r->groups[i].faces);
        }
        free(r->groups);
        return;
}

/* An index as written (1-based, or negative from the end) into 0-based, or -2 if out of range. */
static int32_t resolve(long i, uint32_t count) {
        if (i > 0 && (unsigned long)i <= count) return (int32_t)(i - 1);
        if (i < 0 && (unsigned long)-i <= count) return (int32_t)((long)count + i);
        return -2;
}

/* Reads up to @p max floats from @p s. @return how many. */
static int read_floats(const char *s, float *out, int max) {
        int n = 0;
        while (n < max) {
                char       *end;
                const float f = strtof(s, &end);
                if (end == s) break;
                out[n++] = f;
                s = end;
        }
        return n;
}

/* One `f` line's corners into @p g. */
static int read_face(reader_t *r, group_t *g, const char *s, char *err, size_t err_size, uint32_t line) {
        uint32_t corners = 0;
        while (*s != '\0') {
                while (*s == ' ' || *s == '\t') ++s;
                if (*s == '\0' || *s == '#') break;
                int32_t c[3] = {-1, -1, -1};
                for (int k = 0; k < 3; ++k) {
                        if (k > 0) {
                                if (*s != '/') break;
                                ++s;
                        }
                        if (*s == '/' || *s == ' ' || *s == '\t' || *s == '\0') continue; /* "1//3" */
                        char      *end;
                        const long i = strtol(s, &end, 10);
                        if (end == s) {
                                snprintf(err, err_size, "line %u: a face's index isn't a number", line);
                                return -1;
                        }
                        s = end;
                        const uint32_t count = k == 0 ? r->v.count : k == 1 ? r->vt.count : r->vn.count;
                        c[k] = resolve(i, count);
                        if (c[k] == -2) {
                                snprintf(err, err_size, "line %u: index %ld is out of range", line, i);
                                return -1;
                        }
                }
                if (c[0] < 0) {
                        snprintf(err, err_size, "line %u: a face's corner has no position", line);
                        return -1;
                }
                if (*s != ' ' && *s != '\t' && *s != '\0' && *s != '#') {
                        snprintf(err, err_size, "line %u: can't read a face's corner", line);
                        return -1;
                }
                if (push_corner(g, c) != 0) goto nomem;
                ++corners;
        }
        if (corners < 3) {
                snprintf(err, err_size, "line %u: a face needs 3 corners", line);
                return -1;
        }
        if (push_face(g) != 0) goto nomem;
        return 0;
nomem:
        snprintf(err, err_size, "out of memory");
        return -1;
}

/* The group's faces as triangles in @p m, normals made where the file has none. */
static int build(const reader_t *r, const group_t *g, const m3d_options_t *options, m3d_mesh_t *m) {
        const uint32_t faces = g->face_count != 0 ? g->face_count - 1 : 0;
        if (faces == 0) return 0;
        /* Each face's normal by Newell's method (as long as its area) and the faces at each position. */
        float    *fn = (float *)calloc((size_t)faces * 3, sizeof(float));
        uint32_t *first = (uint32_t *)calloc((size_t)r->v.count + 1, sizeof(uint32_t));
        uint32_t *at = (uint32_t *)malloc(((size_t)g->corner_count + 1) * sizeof(uint32_t));
        int       err = fn == NULL || first == NULL || at == NULL ? -1 : 0;
        for (uint32_t f = 0; err == 0 && f < faces; ++f) {
                const uint32_t a = g->faces[f], b = g->faces[f + 1];
                double         n[3] = {0.0, 0.0, 0.0};
                for (uint32_t c = a; c < b; ++c) {
                        const float *p = r->v.data + 3 * g->corners[3 * c];
                        const float *q = r->v.data + 3 * g->corners[3 * (c + 1 < b ? c + 1 : a)];
                        n[0] += ((double)p[1] - q[1]) * ((double)p[2] + q[2]);
                        n[1] += ((double)p[2] - q[2]) * ((double)p[0] + q[0]);
                        n[2] += ((double)p[0] - q[0]) * ((double)p[1] + q[1]);
                }
                for (int k = 0; k < 3; ++k) fn[3 * f + k] = (float)(0.5 * n[k]);
                for (uint32_t c = a; c < b; ++c) ++first[g->corners[3 * c] + 1];
        }
        if (err == 0) {
                for (uint32_t i = 0; i < r->v.count; ++i) first[i + 1] += first[i];
                uint32_t *fill = (uint32_t *)malloc(((size_t)r->v.count + 1) * sizeof(uint32_t));
                if (fill == NULL) err = -1;
                for (uint32_t i = 0; err == 0 && i < r->v.count; ++i) fill[i] = first[i];
                for (uint32_t f = 0; err == 0 && f < faces; ++f)
                        for (uint32_t c = g->faces[f]; c < g->faces[f + 1]; ++c) at[fill[g->corners[3 * c]]++] = f;
                free(fill);
        }
        const double cos_crease = cos(options->crease * PI / 180.0) - 1e-6;

        uint32_t *index = (uint32_t *)malloc(((size_t)g->corner_count + 1) * sizeof(uint32_t));
        if (index == NULL) err = -1;
        for (uint32_t f = 0; err == 0 && f < faces; ++f) {
                const float *nf = fn + 3 * f;
                const double lf = sqrt((double)nf[0] * nf[0] + (double)nf[1] * nf[1] + (double)nf[2] * nf[2]);
                for (uint32_t c = g->faces[f]; err == 0 && c < g->faces[f + 1]; ++c) {
                        const int32_t *k = g->corners + 3 * c;
                        float          n[3], uv[2];
                        const float   *np = NULL, *uvp = NULL;
                        if (k[2] >= 0) {
                                np = r->vn.data + 3 * k[2];
                        } else if (lf > 0.0) {
                                double s[3] = {0.0, 0.0, 0.0};
                                for (uint32_t j = first[k[0]]; j < first[k[0] + 1]; ++j) {
                                        const float *ng = fn + 3 * at[j];
                                        const double lg = sqrt((double)ng[0] * ng[0] + (double)ng[1] * ng[1] +
                                                               (double)ng[2] * ng[2]);
                                        if (lg <= 0.0) continue;
                                        const double d = ((double)nf[0] * ng[0] + (double)nf[1] * ng[1] +
                                                          (double)nf[2] * ng[2]) /
                                                         (lf * lg);
                                        if (d < cos_crease && at[j] != f) continue;
                                        for (int q = 0; q < 3; ++q) s[q] += ng[q];
                                }
                                const double l = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
                                for (int q = 0; q < 3; ++q) n[q] = (float)(l > 0.0 ? s[q] / l : nf[q] / lf);
                                np = n;
                        }
                        if (k[1] >= 0) {
                                uv[0] = r->vt.data[2 * k[1]];
                                uv[1] = 1.0f - r->vt.data[2 * k[1] + 1]; /* OBJ's v goes up; rows go down */
                                uvp = uv;
                        }
                        const int32_t v = m3d_vertex(m, r->v.data + 3 * k[0], np, uvp);
                        if (v < 0) err = -2;
                        index[c] = (uint32_t)v;
                }
                /* The polygon fanned from its first corner: the spokes inside it aren't edges. */
                const uint32_t hub = g->faces[f], last = g->faces[f + 1] - 1;
                for (uint32_t c = hub + 1; err == 0 && c + 1 <= last; ++c) {
                        const uint32_t inner = (c != hub + 1 ? 1u : 0u) | (c + 1 != last ? 4u : 0u);
                        err = m3d_triangle_inner(m, index[hub], index[c], index[c + 1], inner);
                }
        }
        free(fn), free(first), free(at), free(index);
        return err;
}

int m3d_read_obj(const char *text, size_t length, const char *name, const m3d_options_t *options, m3d_scene_t *scene,
                 char *err, size_t err_size) {
        reader_t r;
        memset(&r, 0, sizeof(r));
        r.v.width = 3, r.vt.width = 2, r.vn.width = 3;
        int      result = 0;
        uint32_t line = 0;
        char     buf[4096];
        for (size_t at = 0; result == 0 && at < length;) {
                size_t end = at;
                while (end < length && text[end] != '\n') ++end;
                ++line;
                size_t n = end - at;
                if (n >= sizeof(buf)) {
                        snprintf(err, err_size, "line %u is too long", line);
                        result = -1;
                        break;
                }
                memcpy(buf, text + at, n);
                buf[n] = '\0';
                at = end + 1;
                while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';
                const char *s = buf;
                while (*s == ' ' || *s == '\t') ++s;
                if (*s == '\0' || *s == '#') continue;
                const char *word = s;
                while (*s != '\0' && *s != ' ' && *s != '\t') ++s;
                const size_t w = (size_t)(s - word);
                while (*s == ' ' || *s == '\t') ++s;

                if (w == 1 && word[0] == 'v') {
                        float x[3];
                        if (read_floats(s, x, 3) != 3) {
                                snprintf(err, err_size, "line %u: a position needs x, y and z", line);
                                result = -1;
                        } else if (push_floats(&r.v, x) != 0) {
                                result = -2;
                        }
                } else if (w == 2 && word[0] == 'v' && word[1] == 't') {
                        float x[2] = {0.0f, 0.0f};
                        if (read_floats(s, x, 2) < 1) {
                                snprintf(err, err_size, "line %u: a uv needs u", line);
                                result = -1;
                        } else if (push_floats(&r.vt, x) != 0) {
                                result = -2;
                        }
                } else if (w == 2 && word[0] == 'v' && word[1] == 'n') {
                        float x[3];
                        if (read_floats(s, x, 3) != 3) {
                                snprintf(err, err_size, "line %u: a normal needs x, y and z", line);
                                result = -1;
                        } else {
                                const float l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                                if (l > 0.0f)
                                        for (int k = 0; k < 3; ++k) x[k] /= l;
                                if (push_floats(&r.vn, x) != 0) result = -2;
                        }
                } else if (w == 1 && word[0] == 'f') {
                        if (r.current == NULL && (r.current = group(&r, name)) == NULL)
                                result = -2;
                        else
                                result = read_face(&r, r.current, s, err, err_size, line);
                } else if (w == 1 && (word[0] == 'o' || word[0] == 'g')) {
                        if (!options->merge && (r.current = group(&r, *s != '\0' ? s : name)) == NULL) result = -2;
                }
                /* Anything else (vp, l, p, s, usemtl, mtllib, ...) is skipped. */
        }
        for (uint32_t i = 0; result == 0 && i < r.group_count; ++i) {
                if (r.groups[i].face_count == 0) continue;
                m3d_mesh_t *m = m3d_scene_find(scene, r.groups[i].name);
                if (m == NULL && (m = m3d_scene_add(scene, r.groups[i].name)) == NULL) {
                        result = -2;
                        break;
                }
                result = build(&r, &r.groups[i], options, m);
                if (result == -2)
                        snprintf(err, err_size, "mesh \"%s\": more than 65,535 vertices", r.groups[i].name);
                else if (result != 0)
                        snprintf(err, err_size, "out of memory");
                if (result != 0) result = -1;
        }
        if (result == -2) snprintf(err, err_size, "out of memory");
        reader_free(&r);
        return result != 0 ? -1 : 0;
}
