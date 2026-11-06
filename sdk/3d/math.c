/**
 * @file      math.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's matrices (devplan/3d.md §4a): column-major, as GL's (`m[column * 4 + row]`).
 */

#include <math.h>
#include <string.h>
#include "3d_internal.h"

#define M(mat, row, col) ((mat)->m[(col) * 4 + (row)])

void bmds_mat4_identity(bmds_mat4_t *m) {
        memset(m, 0, sizeof(*m));
        m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
        return;
}

void bmds_mat4_multiply(bmds_mat4_t *out, const bmds_mat4_t *a, const bmds_mat4_t *b) {
        bmds_mat4_t r;
        for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                        M(&r, row, col) = M(a, row, 0) * M(b, 0, col) + M(a, row, 1) * M(b, 1, col) +
                                          M(a, row, 2) * M(b, 2, col) + M(a, row, 3) * M(b, 3, col);
                }
        }
        *out = r;
        return;
}

void bmds_mat4_translation(bmds_mat4_t *m, float x, float y, float z) {
        bmds_mat4_identity(m);
        M(m, 0, 3) = x;
        M(m, 1, 3) = y;
        M(m, 2, 3) = z;
        return;
}

void bmds_mat4_scaling(bmds_mat4_t *m, float x, float y, float z) {
        bmds_mat4_identity(m);
        M(m, 0, 0) = x;
        M(m, 1, 1) = y;
        M(m, 2, 2) = z;
        return;
}

void bmds_mat4_rotation_x(bmds_mat4_t *m, float angle) {
        const float c = cosf(angle), s = sinf(angle);
        bmds_mat4_identity(m);
        M(m, 1, 1) = c;
        M(m, 1, 2) = -s;
        M(m, 2, 1) = s;
        M(m, 2, 2) = c;
        return;
}

void bmds_mat4_rotation_y(bmds_mat4_t *m, float angle) {
        const float c = cosf(angle), s = sinf(angle);
        bmds_mat4_identity(m);
        M(m, 0, 0) = c;
        M(m, 0, 2) = s;
        M(m, 2, 0) = -s;
        M(m, 2, 2) = c;
        return;
}

void bmds_mat4_rotation_z(bmds_mat4_t *m, float angle) {
        const float c = cosf(angle), s = sinf(angle);
        bmds_mat4_identity(m);
        M(m, 0, 0) = c;
        M(m, 0, 1) = -s;
        M(m, 1, 0) = s;
        M(m, 1, 1) = c;
        return;
}

void bmds_mat4_rotation(bmds_mat4_t *m, bmds_vec3_t axis, float angle) {
        const float l = bmds_vec3_length(axis);
        if (!(l > 0.0f)) {
                bmds_mat4_identity(m);
                return;
        }
        const float x = axis.x / l, y = axis.y / l, z = axis.z / l;
        const float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
        bmds_mat4_identity(m);
        M(m, 0, 0) = t * x * x + c;
        M(m, 0, 1) = t * x * y - s * z;
        M(m, 0, 2) = t * x * z + s * y;
        M(m, 1, 0) = t * x * y + s * z;
        M(m, 1, 1) = t * y * y + c;
        M(m, 1, 2) = t * y * z - s * x;
        M(m, 2, 0) = t * x * z - s * y;
        M(m, 2, 1) = t * y * z + s * x;
        M(m, 2, 2) = t * z * z + c;
        return;
}

void bmds_mat4_perspective(bmds_mat4_t *m, float fovy, float aspect, float near_z, float far_z) {
        const float f = 1.0f / tanf(fovy * 0.5f);
        memset(m, 0, sizeof(*m));
        M(m, 0, 0) = f / aspect;
        M(m, 1, 1) = f;
        M(m, 2, 2) = (far_z + near_z) / (near_z - far_z);
        M(m, 2, 3) = 2.0f * far_z * near_z / (near_z - far_z);
        M(m, 3, 2) = -1.0f;
        return;
}

void bmds_mat4_ortho(bmds_mat4_t *m, float left, float right, float bottom, float top, float near_z, float far_z) {
        bmds_mat4_identity(m);
        M(m, 0, 0) = 2.0f / (right - left);
        M(m, 1, 1) = 2.0f / (top - bottom);
        M(m, 2, 2) = -2.0f / (far_z - near_z);
        M(m, 0, 3) = -(right + left) / (right - left);
        M(m, 1, 3) = -(top + bottom) / (top - bottom);
        M(m, 2, 3) = -(far_z + near_z) / (far_z - near_z);
        return;
}

void bmds_mat4_look_at(bmds_mat4_t *m, bmds_vec3_t eye, bmds_vec3_t target, bmds_vec3_t up) {
        const bmds_vec3_t f = bmds_vec3_normalize(bmds_vec3_sub(target, eye));
        const bmds_vec3_t s = bmds_vec3_normalize(bmds_vec3_cross(f, up));
        const bmds_vec3_t u = bmds_vec3_cross(s, f);
        bmds_mat4_identity(m);
        M(m, 0, 0) = s.x;
        M(m, 0, 1) = s.y;
        M(m, 0, 2) = s.z;
        M(m, 1, 0) = u.x;
        M(m, 1, 1) = u.y;
        M(m, 1, 2) = u.z;
        M(m, 2, 0) = -f.x;
        M(m, 2, 1) = -f.y;
        M(m, 2, 2) = -f.z;
        M(m, 0, 3) = -bmds_vec3_dot(s, eye);
        M(m, 1, 3) = -bmds_vec3_dot(u, eye);
        M(m, 2, 3) = bmds_vec3_dot(f, eye);
        return;
}

int bmds_mat4_invert_affine(bmds_mat4_t *out, const bmds_mat4_t *m) {
        /* The 3×3 part's inverse by its cofactors, then the translation through it. */
        const float a = M(m, 0, 0), b = M(m, 0, 1), c = M(m, 0, 2);
        const float d = M(m, 1, 0), e = M(m, 1, 1), f = M(m, 1, 2);
        const float g = M(m, 2, 0), h = M(m, 2, 1), i = M(m, 2, 2);
        const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        const float det = a * A + b * B + c * C;
        if (!(fabsf(det) > 1e-30f)) return -BMDS_EINVAL;
        const float k = 1.0f / det;
        bmds_mat4_t r;
        bmds_mat4_identity(&r);
        M(&r, 0, 0) = A * k;
        M(&r, 0, 1) = -(b * i - c * h) * k;
        M(&r, 0, 2) = (b * f - c * e) * k;
        M(&r, 1, 0) = B * k;
        M(&r, 1, 1) = (a * i - c * g) * k;
        M(&r, 1, 2) = -(a * f - c * d) * k;
        M(&r, 2, 0) = C * k;
        M(&r, 2, 1) = -(a * h - b * g) * k;
        M(&r, 2, 2) = (a * e - b * d) * k;
        const float tx = M(m, 0, 3), ty = M(m, 1, 3), tz = M(m, 2, 3);
        for (int row = 0; row < 3; ++row)
                M(&r, row, 3) = -(M(&r, row, 0) * tx + M(&r, row, 1) * ty + M(&r, row, 2) * tz);
        *out = r;
        return 0;
}

bmds_vec4_t bmds_mat4_transform(const bmds_mat4_t *m, bmds_vec3_t p) {
        bmds_vec4_t r;
        r.x = M(m, 0, 0) * p.x + M(m, 0, 1) * p.y + M(m, 0, 2) * p.z + M(m, 0, 3);
        r.y = M(m, 1, 0) * p.x + M(m, 1, 1) * p.y + M(m, 1, 2) * p.z + M(m, 1, 3);
        r.z = M(m, 2, 0) * p.x + M(m, 2, 1) * p.y + M(m, 2, 2) * p.z + M(m, 2, 3);
        r.w = M(m, 3, 0) * p.x + M(m, 3, 1) * p.y + M(m, 3, 2) * p.z + M(m, 3, 3);
        return r;
}

bmds_vec3_t bmds_mat4_transform_point(const bmds_mat4_t *m, bmds_vec3_t p) {
        const bmds_vec4_t r = bmds_mat4_transform(m, p);
        const float       k = r.w != 0.0f ? 1.0f / r.w : 1.0f;
        return bmds_vec3(r.x * k, r.y * k, r.z * k);
}

bmds_vec3_t bmds_mat4_transform_vector(const bmds_mat4_t *m, bmds_vec3_t v) {
        return bmds_vec3(M(m, 0, 0) * v.x + M(m, 0, 1) * v.y + M(m, 0, 2) * v.z,
                         M(m, 1, 0) * v.x + M(m, 1, 1) * v.y + M(m, 1, 2) * v.z,
                         M(m, 2, 0) * v.x + M(m, 2, 1) * v.y + M(m, 2, 2) * v.z);
}
