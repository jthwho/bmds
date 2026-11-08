/**
 * @file      bpt.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-mesh3d's Bézier patches (`.bpt`): a patch count, then each patch's degrees (`3 3`: only
 * bicubic) and its 16 control points, x y z, in rows of u (point `4 * row + column`, the column
 * along u). The front is where ∂P/∂u × ∂P/∂v points. Each patch becomes a `detail` × `detail`
 * grid; normals from the derivatives, moved a step inside the patch where one vanishes (a patch
 * pinched to a point, as the teapot's lid and bottom); seams welded within the crease angle.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mesh3d.h"

static void bernstein(double t, double *b, double *d) {
        const double s = 1.0 - t;
        b[0] = s * s * s;
        b[1] = 3.0 * t * s * s;
        b[2] = 3.0 * t * t * s;
        b[3] = t * t * t;
        d[0] = -3.0 * s * s;
        d[1] = 3.0 * s * s - 6.0 * t * s;
        d[2] = 6.0 * t * s - 3.0 * t * t;
        d[3] = 3.0 * t * t;
        return;
}

/* The patch @p cp's point at (@p u, @p v) into @p p, and its derivatives' cross into @p n. */
static void evaluate(const double *cp, double u, double v, double *p, double *n) {
        double bu[4], du[4], bv[4], dv[4];
        bernstein(u, bu, du);
        bernstein(v, bv, dv);
        double pu[3] = {0.0, 0.0, 0.0}, pv[3] = {0.0, 0.0, 0.0};
        p[0] = p[1] = p[2] = 0.0;
        for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) {
                        const double *c = cp + 3 * (4 * i + j);
                        for (int k = 0; k < 3; ++k) {
                                p[k] += bv[i] * bu[j] * c[k];
                                pu[k] += bv[i] * du[j] * c[k];
                                pv[k] += dv[i] * bu[j] * c[k];
                        }
                }
        n[0] = pu[1] * pv[2] - pu[2] * pv[1];
        n[1] = pu[2] * pv[0] - pu[0] * pv[2];
        n[2] = pu[0] * pv[1] - pu[1] * pv[0];
        return;
}

/* Reads the next number from @p *s. @return 0, or -1 (none). */
static int number(const char **s, const char *end, double *out) {
        const char *p = *s;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) ++p;
        if (p >= end) return -1;
        char  *e;
        double x = strtod(p, &e);
        if (e == p || e > end) return -1;
        *out = x;
        *s = e;
        return 0;
}

int m3d_read_bpt(const char *text, size_t length, const char *name, const m3d_options_t *options, m3d_scene_t *scene,
                 char *err, size_t err_size) {
        const char *s = text, *end = text + length;
        double      count;
        if (number(&s, end, &count) != 0 || count < 1.0 || count > 100000.0 || count != floor(count)) {
                snprintf(err, err_size, "no patch count");
                return -1;
        }
        const uint32_t patches = (uint32_t)count, n = options->detail;
        if (n < 1 || n > 64) {
                snprintf(err, err_size, "detail %u: 1 to 64", n);
                return -1;
        }
        m3d_mesh_t *m = m3d_scene_add(scene, name);
        if (m == NULL) {
                snprintf(err, err_size, "out of memory");
                return -1;
        }
        uint32_t *grid = (uint32_t *)malloc((size_t)(n + 1) * (n + 1) * sizeof(uint32_t));
        if (grid == NULL) {
                snprintf(err, err_size, "out of memory");
                return -1;
        }
        int result = 0;
        for (uint32_t pt = 0; result == 0 && pt < patches; ++pt) {
                double deg[2], cp[48];
                if (number(&s, end, &deg[0]) != 0 || number(&s, end, &deg[1]) != 0) {
                        snprintf(err, err_size, "patch %u: no degrees", pt + 1);
                        result = -1;
                        break;
                }
                if (deg[0] != 3.0 || deg[1] != 3.0) {
                        snprintf(err, err_size, "patch %u: degrees %g %g (only 3 3, bicubic)", pt + 1, deg[0], deg[1]);
                        result = -1;
                        break;
                }
                for (int i = 0; result == 0 && i < 48; ++i)
                        if (number(&s, end, &cp[i]) != 0) {
                                snprintf(err, err_size, "patch %u: fewer than 16 points", pt + 1);
                                result = -1;
                        }
                for (uint32_t a = 0; result == 0 && a <= n; ++a)
                        for (uint32_t b = 0; result == 0 && b <= n; ++b) {
                                const double u = (double)b / n, v = (double)a / n;
                                double       p[3], nn[3], dummy[3];
                                evaluate(cp, u, v, p, nn);
                                /* Where a side is pinched the cross vanishes: take it a little inside. */
                                for (double step = 1e-4; step < 0.2; step *= 10.0) {
                                        if (nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2] > 1e-18) break;
                                        evaluate(cp, u + (0.5 - u) * step, v + (0.5 - v) * step, dummy, nn);
                                }
                                const double  l = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                                const float   pf[3] = {(float)p[0], (float)p[1], (float)p[2]};
                                const float   nf[3] = {(float)(l > 0.0 ? nn[0] / l : 0.0),
                                                       (float)(l > 0.0 ? nn[1] / l : 0.0),
                                                       (float)(l > 0.0 ? nn[2] / l : 0.0)};
                                const float   uv[2] = {(float)u, (float)v};
                                const int32_t i = m3d_vertex(m, pf, nf, options->no_uvs ? NULL : uv);
                                if (i < 0) {
                                        snprintf(err, err_size, "more than 65,535 vertices: less detail");
                                        result = -1;
                                }
                                grid[a * (n + 1) + b] = (uint32_t)i;
                        }
                /* Counter-clockwise with u right and v up: (u0 v0, u1 v0, u1 v1), (u0 v0, u1 v1, u0 v1). */
                for (uint32_t a = 0; result == 0 && a < n; ++a)
                        for (uint32_t b = 0; result == 0 && b < n; ++b) {
                                const uint32_t p00 = grid[a * (n + 1) + b], p10 = grid[a * (n + 1) + b + 1];
                                const uint32_t p01 = grid[(a + 1) * (n + 1) + b], p11 = grid[(a + 1) * (n + 1) + b + 1];
                                /* The diagonal (p11 to p00, then p00 to p11) is no edge. */
                                if (m3d_triangle_inner(m, p00, p10, p11, 4u) != 0 ||
                                    m3d_triangle_inner(m, p00, p11, p01, 1u) != 0) {
                                        snprintf(err, err_size, "out of memory");
                                        result = -1;
                                }
                        }
        }
        free(grid);
        if (result == 0 && m3d_weld(m, options->crease) != 0) {
                snprintf(err, err_size, "out of memory, or more than 65,535 vertices");
                result = -1;
        }
        return result;
}
