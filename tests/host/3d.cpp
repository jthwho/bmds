/**
 * @file      3d.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d (sdk/3d, devplan/3d.md): the matrices, made meshes and their edges, lines and points
 * where they should be in each format (clipped in 3D and to the canvas's clip, never outside),
 * hidden edges, shading by distance, projecting and sorting, and no memory left behind.
 */

#include <doctest.h>

#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <utility>
#include <vector>
#include <bmds/3d.h>

#include "fakememory.h"

namespace {

        constexpr float Pi = 3.14159265f;

        // A canvas over its own pixels, with a border round its clip to catch stray writes.
        struct Picture {
                        std::vector<uint8_t> pixels;
                        bmds_canvas_t        canvas{};
                        uint32_t             bpp;

                        Picture(int32_t w, int32_t h, uint32_t format, uint8_t fill = 0) {
                                bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
                                pixels.assign(size_t(w) * h * bpp, fill);
                                REQUIRE(bmds_canvas_init(&canvas, pixels.data(), uint32_t(w) * bpp, w, h, format) == 0);
                        }

                        uint8_t l8(int32_t x, int32_t y) const { return pixels[size_t(y) * canvas.stride + size_t(x)]; }

                        uint32_t value(int32_t x, int32_t y) const {
                                const uint8_t *p = pixels.data() + size_t(y) * canvas.stride + size_t(x) * bpp;
                                if (bpp == 1) return p[0];
                                if (bpp == 2) return uint32_t(p[0] | p[1] << 8);
                                return uint32_t(p[0] | p[1] << 8 | p[2] << 16);
                        }

                        // The pixels not equal to @p background.
                        std::set<std::pair<int32_t, int32_t>> lit(uint32_t background = 0) const {
                                std::set<std::pair<int32_t, int32_t>> s;
                                for (int32_t y = 0; y < canvas.height; ++y)
                                        for (int32_t x = 0; x < canvas.width; ++x)
                                                if (value(x, y) != background) s.insert({x, y});
                                return s;
                        }
        };

        // A context drawing on @p canvas, one unit a pixel (x right, y down, z 0 in the middle).
        struct Scene {
                        bmds_3d_t *ctx = nullptr;

                        explicit Scene(bmds_canvas_t *canvas) {
                                REQUIRE(bmds_3d_create(nullptr, &ctx) == 0);
                                bmds_3d_set_target(ctx, canvas);
                                bmds_mat4_t p;
                                bmds_mat4_ortho(&p, 0.0f, float(canvas->width), float(canvas->height), 0.0f, -10.0f,
                                                10.0f);
                                bmds_3d_set_projection(ctx, &p);
                        }
                        ~Scene() { bmds_3d_destroy(ctx); }
                        Scene(const Scene &) = delete;
                        Scene &operator=(const Scene &) = delete;
        };

        bmds_3d_material_t material(uint32_t mode, uint32_t pixel, uint32_t flags = 0) {
                bmds_3d_material_t m{};
                m.size = sizeof(m);
                m.mode = mode;
                m.pixel = pixel;
                m.flags = flags;
                return m;
        }

        bool near(float a, float b, float eps = 1e-4f) {
                return std::fabs(a - b) <= eps;
        }

        std::set<std::pair<int32_t, int32_t>> pts(std::initializer_list<std::pair<int32_t, int32_t>> l) {
                return std::set<std::pair<int32_t, int32_t>>(l);
        }

        // The edges' vertex positions, as unordered pairs of points (welded).
        struct EdgeInfo {
                        uint32_t open = 0, shared = 0;
        };

        EdgeInfo edgeInfo(const bmds_3d_mesh_t &m) {
                EdgeInfo e;
                for (uint32_t i = 0; i < m.edge_count; ++i) {
                        const bool one = m.edge_faces[2 * i + 1] == BMDS_3D_NO_FACE;
                        (one ? e.open : e.shared)++;
                }
                return e;
        }

} // namespace

TEST_CASE("3d: matrices") {
        bmds_mat4_t a, b, c;
        bmds_mat4_translation(&a, 1.0f, 2.0f, 3.0f);
        bmds_vec3_t p = bmds_mat4_transform_point(&a, bmds_vec3(1.0f, 1.0f, 1.0f));
        CHECK(near(p.x, 2.0f));
        CHECK(near(p.y, 3.0f));
        CHECK(near(p.z, 4.0f));
        CHECK(near(bmds_mat4_transform_vector(&a, bmds_vec3(1, 0, 0)).x, 1.0f));

        // Rotations turn the axes the right way round (right-handed).
        bmds_mat4_rotation_z(&b, Pi / 2.0f);
        p = bmds_mat4_transform_point(&b, bmds_vec3(1, 0, 0));
        CHECK(near(p.x, 0.0f));
        CHECK(near(p.y, 1.0f));
        bmds_mat4_rotation_x(&b, Pi / 2.0f);
        p = bmds_mat4_transform_point(&b, bmds_vec3(0, 1, 0));
        CHECK(near(p.z, 1.0f));
        bmds_mat4_rotation_y(&b, Pi / 2.0f);
        p = bmds_mat4_transform_point(&b, bmds_vec3(0, 0, 1));
        CHECK(near(p.x, 1.0f));
        bmds_mat4_rotation(&c, bmds_vec3(0, 0, 2), Pi / 2.0f);
        for (int i = 0; i < 16; ++i) CHECK(near(c.m[i], (bmds_mat4_rotation_z(&b, Pi / 2.0f), b.m[i])));

        // Multiplying applies the right-hand matrix first; the inverse undoes it.
        bmds_mat4_multiply(&c, &a, &b); // rotate, then translate
        p = bmds_mat4_transform_point(&c, bmds_vec3(1, 0, 0));
        CHECK(near(p.x, 1.0f));
        CHECK(near(p.y, 3.0f));
        CHECK(near(p.z, 3.0f));
        bmds_mat4_t inv;
        REQUIRE(bmds_mat4_invert_affine(&inv, &c) == 0);
        bmds_vec3_t q = bmds_mat4_transform_point(&inv, p);
        CHECK(near(q.x, 1.0f));
        CHECK(near(q.y, 0.0f));
        CHECK(near(q.z, 0.0f));
        bmds_mat4_scaling(&a, 0.0f, 1.0f, 1.0f);
        CHECK(bmds_mat4_invert_affine(&inv, &a) == -BMDS_EINVAL);

        // GLU's perspective: near maps to -1, far to +1; look_at puts the target down -Z.
        bmds_mat4_perspective(&a, Pi / 2.0f, 2.0f, 1.0f, 10.0f);
        CHECK(near(bmds_mat4_transform_point(&a, bmds_vec3(0, 0, -1)).z, -1.0f));
        CHECK(near(bmds_mat4_transform_point(&a, bmds_vec3(0, 0, -10)).z, 1.0f));
        CHECK(near(bmds_mat4_transform_point(&a, bmds_vec3(2, 1, -1)).x, 1.0f));
        CHECK(near(bmds_mat4_transform_point(&a, bmds_vec3(2, 1, -1)).y, 1.0f));
        bmds_mat4_look_at(&b, bmds_vec3(0, 0, 5), bmds_vec3(0, 0, 0), bmds_vec3(0, 1, 0));
        p = bmds_mat4_transform_point(&b, bmds_vec3(0, 0, 0));
        CHECK(near(p.z, -5.0f));
        bmds_mat4_look_at(&b, bmds_vec3(3, 0, 0), bmds_vec3(0, 0, 0), bmds_vec3(0, 1, 0));
        p = bmds_mat4_transform_point(&b, bmds_vec3(0, 1, 0));
        CHECK(near(p.y, 1.0f));
        CHECK(near(p.z, -3.0f));
}

TEST_CASE("3d: made meshes, their edges and bounds") {
        const size_t    before = FakeMemory::live();
        bmds_3d_shape_t shape{};
        shape.size = sizeof(shape);
        bmds_3d_mesh_t m{};

        SUBCASE("cube: 12 edges between two faces each, no diagonals") {
                shape.kind = BMDS_3D_SHAPE_CUBE;
                REQUIRE(bmds_3d_mesh_make(&shape, 0, &m) == 0);
                CHECK(m.vertex_count == 24);
                CHECK(m.triangle_count == 12);
                CHECK(m.edge_count == 12);
                CHECK(edgeInfo(m).shared == 12);
                CHECK(near(m.bounds[3], std::sqrt(3.0f), 1e-3f));
        }
        SUBCASE("grid: its lines, the border open") {
                shape.kind = BMDS_3D_SHAPE_GRID;
                shape.segments = 4;
                REQUIRE(bmds_3d_mesh_make(&shape, 0, &m) == 0);
                CHECK(m.edge_count == 2 * 4 * 5);
                CHECK(edgeInfo(m).open == 16);
        }
        SUBCASE("sphere: meridians and parallels, the seam welded") {
                shape.kind = BMDS_3D_SHAPE_SPHERE;
                shape.segments = 8;
                shape.rings = 4;
                REQUIRE(bmds_3d_mesh_make(&shape, 0, &m) == 0);
                CHECK(m.triangle_count == 8 * 2 * 2 + 8 * 2); // the poles' quads are triangles
                CHECK(m.edge_count == 8 * 4 + 8 * 3);
                CHECK(edgeInfo(m).open == 0);
        }
        SUBCASE("torus: closed both ways") {
                shape.kind = BMDS_3D_SHAPE_TORUS;
                shape.segments = 8;
                shape.rings = 4;
                REQUIRE(bmds_3d_mesh_make(&shape, 0, &m) == 0);
                CHECK(m.edge_count == 2 * 8 * 4);
                CHECK(edgeInfo(m).open == 0);
                CHECK(near(m.bounds[3], 1.0f + 1.0f / 3.0f, 1e-3f));
        }
        // Every triangle faces outwards (its normal along the vertices').
        for (uint32_t t = 0; t < m.triangle_count; ++t) {
                const uint16_t *i = m.triangles + 3 * t;
                auto            pos = [&](uint32_t v) {
                        return bmds_vec3(m.positions[3 * v], m.positions[3 * v + 1], m.positions[3 * v + 2]);
                };
                const bmds_vec3_t n =
                        bmds_vec3_cross(bmds_vec3_sub(pos(i[1]), pos(i[0])), bmds_vec3_sub(pos(i[2]), pos(i[0])));
                const bmds_vec3_t want =
                        bmds_vec3(m.normals[3 * i[0]], m.normals[3 * i[0] + 1], m.normals[3 * i[0] + 2]);
                CHECK(bmds_vec3_dot(n, want) > 0.0f);
        }
        bmds_3d_mesh_free(&m);
        CHECK(FakeMemory::live() == before);

        shape.kind = 99;
        CHECK(bmds_3d_mesh_make(&shape, 0, &m) == -BMDS_EINVAL);
        shape.kind = BMDS_3D_SHAPE_SPHERE;
        shape.segments = 2;
        CHECK(bmds_3d_mesh_make(&shape, 0, &m) == -BMDS_EINVAL);
        CHECK(FakeMemory::live() == before);
}

TEST_CASE("3d: edges of a caller's mesh") {
        // A square of two triangles and a fan: the diagonal goes; with COPLANAR, the spokes too.
        static const float    square[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        static const uint16_t squareTris[] = {0, 1, 2, 0, 2, 3};
        bmds_3d_mesh_t        m{};
        m.size = sizeof(m);
        m.vertex_count = 4;
        m.positions = square;
        m.triangle_count = 2;
        m.triangles = squareTris;
        REQUIRE(bmds_3d_mesh_edges(&m, 0, 0) == 0);
        CHECK(m.edge_count == 4);
        static const float    hexagon[] = {0, 0,  0, 1, 0,     0,      0.5f, 0.87f, 0,      -0.5f, 0.87f,
                                           0, -1, 0, 0, -0.5f, -0.87f, 0,    0.5f,  -0.87f, 0};
        static const uint16_t fan[] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 6, 0, 6, 1};
        m.vertex_count = 7;
        m.positions = hexagon;
        m.triangle_count = 6;
        m.triangles = fan;
        REQUIRE(bmds_3d_mesh_edges(&m, BMDS_3D_EDGES_COPLANAR, 0) == 0);
        CHECK(m.edge_count == 6);
        REQUIRE(bmds_3d_mesh_edges(&m, 0, 0) == 0); // the earlier arrays freed
        CHECK(m.edge_count >= 6);
        CHECK(bmds_3d_mesh_edges(&m, 0x80, 0) == -BMDS_EINVAL);
        bmds_3d_mesh_free(&m);
        CHECK(m.positions == nullptr);
}

TEST_CASE("3d: lines land on the pixels their centres pass") {
        Picture     pic(16, 16, BMDS_PIXEL_L8);
        Scene       s(&pic.canvas);
        const auto  ink = material(BMDS_3D_WIRE, 7);
        const float horizontal[] = {2.5f, 3.5f, 0, 10.5f, 3.5f, 0};
        REQUIRE(bmds_3d_lines(s.ctx, horizontal, 2, 0, &ink) == 0);
        auto expected = pts({});
        for (int32_t x = 2; x <= 10; ++x) expected.insert({x, 3});
        CHECK(pic.lit() == expected);
        CHECK(pic.l8(2, 3) == 7);

        Picture     pic2(16, 16, BMDS_PIXEL_L8);
        Scene       s2(&pic2.canvas);
        const float diagonal[] = {0.5f, 0.5f, 0, 5.5f, 5.5f, 0};
        REQUIRE(bmds_3d_lines(s2.ctx, diagonal, 2, 0, &ink) == 0);
        CHECK(pic2.lit() == pts({{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}}));
        // The damage covers it.
        REQUIRE(pic2.canvas.damage_count >= 1);
        CHECK(pic2.canvas.damage[0].x <= 0);
        CHECK(pic2.canvas.damage[0].width >= 6);

        // A strip: three segments, joined.
        Picture     pic3(16, 16, BMDS_PIXEL_L8);
        Scene       s3(&pic3.canvas);
        const float strip[] = {1.5f, 1.5f, 0, 8.5f, 1.5f, 0, 8.5f, 8.5f, 0, 1.5f, 8.5f, 0};
        REQUIRE(bmds_3d_lines(s3.ctx, strip, 4, 1, &ink) == 0);
        CHECK(pic3.lit().size() == 8 + 7 + 7);
        bmds_3d_stats_t st{};
        st.size = sizeof(st);
        bmds_3d_stats(s3.ctx, &st);
        CHECK(st.lines == 3);
}

TEST_CASE("3d: lines are clipped to the canvas's clip, in every format") {
        for (uint32_t format : {uint32_t(BMDS_PIXEL_L8), uint32_t(BMDS_PIXEL_RGB565), uint32_t(BMDS_PIXEL_BGR24)}) {
                CAPTURE(format);
                Picture           pic(32, 32, format);
                const bmds_rect_t clip = {8, 8, 16, 16};
                bmds_canvas_set_clip(&pic.canvas, &clip);
                Scene          s(&pic.canvas);
                const uint32_t pixel = format == BMDS_PIXEL_L8 ? 9u : format == BMDS_PIXEL_RGB565 ? 0xf800u : 0x123456u;
                const auto     ink = material(BMDS_3D_WIRE, pixel);
                const float    across[] = {-1000.0f, 12.5f, 0, 1000.0f, 12.5f, 0};
                REQUIRE(bmds_3d_lines(s.ctx, across, 2, 0, &ink) == 0);
                auto expected = pts({});
                for (int32_t x = 8; x < 24; ++x) expected.insert({x, 12});
                CHECK(pic.lit() == expected);
                CHECK(pic.value(8, 12) == pixel);

                // Random lines, huge, tiny, through the eye: nothing outside the clip.
                std::mt19937                          rng(7);
                std::uniform_real_distribution<float> big(-5000.0f, 5000.0f), z(-30.0f, 30.0f);
                bmds_mat4_t                           p;
                bmds_mat4_perspective(&p, 1.2f, 1.0f, 0.5f, 50.0f);
                for (int round = 0; round < 2; ++round) {
                        if (round == 1) bmds_3d_set_projection(s.ctx, &p);
                        for (int i = 0; i < 2000; ++i) {
                                const float l[] = {big(rng), big(rng), z(rng), big(rng), big(rng), z(rng)};
                                REQUIRE(bmds_3d_lines(s.ctx, l, 2, 0, &ink) == 0);
                        }
                        const float nan[] = {NAN, 1, 1, 2, 2, 2};
                        REQUIRE(bmds_3d_lines(s.ctx, nan, 2, 0, &ink) == 0);
                }
                for (const auto &q : pic.lit()) {
                        CHECK(q.first >= 8);
                        CHECK(q.first < 24);
                        CHECK(q.second >= 8);
                        CHECK(q.second < 24);
                }
        }
}

TEST_CASE("3d: near clipping, and points") {
        Picture     pic(32, 32, BMDS_PIXEL_L8);
        Scene       s(&pic.canvas);
        bmds_mat4_t p;
        bmds_mat4_perspective(&p, Pi / 2.0f, 1.0f, 1.0f, 100.0f);
        bmds_3d_set_projection(s.ctx, &p);
        // From in front of the camera to behind it: only the part in front is drawn, from the
        // point straight ahead (the middle) outwards.
        const auto  ink = material(BMDS_3D_WIRE, 1);
        const float through[] = {0.0f, 0.0f, -2.0f, 0.0f, 3.0f, 5.0f};
        REQUIRE(bmds_3d_lines(s.ctx, through, 2, 0, &ink) == 0);
        const auto lit = pic.lit();
        CHECK(!lit.empty());
        CHECK(lit.count({16, 16}) == 1);
        for (const auto &q : lit) CHECK(q.first == 16);

        // Points: a square of point_size, centred.
        Picture pic2(16, 16, BMDS_PIXEL_L8);
        Scene   s2(&pic2.canvas);
        auto    dot = material(BMDS_3D_POINTS, 5);
        dot.point_size = 3;
        const float at[] = {5.5f, 5.5f, 0.0f, 100.0f, 100.0f, 0.0f};
        REQUIRE(bmds_3d_points(s2.ctx, at, 2, &dot) == 0);
        auto expected = pts({});
        for (int32_t y = 4; y <= 6; ++y)
                for (int32_t x = 4; x <= 6; ++x) expected.insert({x, y});
        CHECK(pic2.lit() == expected);
}

TEST_CASE("3d: shading by distance along a ramp") {
        Picture     pic(32, 32, BMDS_PIXEL_L8);
        Scene       s(&pic.canvas);
        bmds_mat4_t p;
        bmds_mat4_perspective(&p, Pi / 2.0f, 1.0f, 0.5f, 100.0f);
        bmds_3d_set_projection(s.ctx, &p);
        bmds_3d_set_fog(s.ctx, 1.0f, 10.0f);
        auto dot = material(BMDS_3D_POINTS, 0, BMDS_3D_DEPTH_CUE);
        dot.ramp_first = 16;
        dot.ramp_count = 16;
        const float nearPoint[] = {0.0f, 0.0f, -1.0f}, farPoint[] = {0.0f, 0.0f, -10.0f}, mid[] = {0.0f, 0.0f, -5.5f};
        REQUIRE(bmds_3d_points(s.ctx, nearPoint, 1, &dot) == 0);
        CHECK(pic.l8(16, 16) == 31);
        REQUIRE(bmds_3d_points(s.ctx, farPoint, 1, &dot) == 0);
        CHECK(pic.l8(16, 16) == 16);
        REQUIRE(bmds_3d_points(s.ctx, mid, 1, &dot) == 0);
        CHECK(pic.l8(16, 16) == 24); // half way: 7.5 rounds up
        // A ramp past the palette's end is refused.
        dot.ramp_first = 250;
        CHECK(bmds_3d_points(s.ctx, mid, 1, &dot) == -BMDS_EINVAL);

        // RGB: the colour scaled, black at the far end.
        Picture pic2(32, 32, BMDS_PIXEL_BGR24);
        Scene   s2(&pic2.canvas);
        bmds_3d_set_projection(s2.ctx, &p);
        bmds_3d_set_fog(s2.ctx, 1.0f, 10.0f);
        auto rgb = material(BMDS_3D_POINTS, 0, BMDS_3D_DEPTH_CUE);
        rgb.rgb = 0xff8040;
        REQUIRE(bmds_3d_points(s2.ctx, nearPoint, 1, &rgb) == 0);
        CHECK(pic2.value(16, 16) == 0xff8040u);
        REQUIRE(bmds_3d_points(s2.ctx, farPoint, 1, &rgb) == 0);
        CHECK(pic2.value(16, 16) == 0u);
}

TEST_CASE("3d: a wire cube, its hidden edges gone") {
        const size_t     before = FakeMemory::live();
        Picture          pic(64, 64, BMDS_PIXEL_L8);
        bmds_3d_t       *ctx = nullptr;
        bmds_3d_config_t config{};
        config.size = sizeof(config);
        config.max_vertices = 64;
        config.mem_flags = BMDS_MEM_FAST;
        REQUIRE(bmds_3d_create(&config, &ctx) == 0);
        bmds_3d_set_target(ctx, &pic.canvas);
        bmds_mat4_t p, v;
        bmds_mat4_perspective(&p, Pi / 3.0f, 1.0f, 0.5f, 50.0f);
        bmds_mat4_look_at(&v, bmds_vec3(0, 0, 5), bmds_vec3(0, 0, 0), bmds_vec3(0, 1, 0));
        bmds_3d_set_projection(ctx, &p);
        bmds_3d_set_view(ctx, &v);
        bmds_3d_shape_t shape{};
        shape.size = sizeof(shape);
        shape.kind = BMDS_3D_SHAPE_CUBE;
        bmds_3d_mesh_t cube{};
        REQUIRE(bmds_3d_mesh_make(&shape, 0, &cube) == 0);

        bmds_3d_stats_t st{};
        st.size = sizeof(st);
        auto wire = material(BMDS_3D_WIRE, 3);
        bmds_3d_begin(ctx);
        REQUIRE(bmds_3d_draw(ctx, &cube, &wire) == 0);
        bmds_3d_stats(ctx, &st);
        CHECK(st.lines == 12);
        CHECK(st.vertices == 24);

        // Head on, only the front face's four edges face the viewer.
        std::fill(pic.pixels.begin(), pic.pixels.end(), 0);
        wire.flags = BMDS_3D_CULL_BACK;
        bmds_3d_begin(ctx);
        REQUIRE(bmds_3d_draw(ctx, &cube, &wire) == 0);
        bmds_3d_stats(ctx, &st);
        CHECK(st.lines == 4);
        // Turned to a corner, three faces show: nine edges.
        bmds_3d_rotate(ctx, bmds_vec3(0, 1, 0), Pi / 4.0f);
        bmds_3d_rotate(ctx, bmds_vec3(1, 0, 0), 0.6f);
        bmds_3d_begin(ctx);
        REQUIRE(bmds_3d_draw(ctx, &cube, &wire) == 0);
        bmds_3d_stats(ctx, &st);
        CHECK(st.lines == 9);

        // Behind the camera: culled whole by its bounds.
        bmds_mat4_t m;
        bmds_mat4_translation(&m, 0.0f, 0.0f, 20.0f);
        bmds_3d_load(ctx, &m);
        bmds_3d_begin(ctx);
        REQUIRE(bmds_3d_draw(ctx, &cube, &wire) == 0);
        bmds_3d_stats(ctx, &st);
        CHECK(st.culled == 1);
        CHECK(st.lines == 0);

        // A lit solid on L8 needs a ramp; too many vertices; a bad material.
        bmds_mat4_identity(&m);
        bmds_3d_load(ctx, &m);
        auto flat = material(BMDS_3D_FLAT, 1);
        CHECK(bmds_3d_draw(ctx, &cube, &flat) == -BMDS_EINVAL);
        shape.kind = BMDS_3D_SHAPE_SPHERE;
        bmds_3d_mesh_t sphere{};
        REQUIRE(bmds_3d_mesh_make(&shape, 0, &sphere) == 0);
        CHECK(bmds_3d_draw(ctx, &sphere, &wire) == -BMDS_ENOSPC);
        auto bad = material(BMDS_3D_WIRE, 1, 0x8000);
        CHECK(bmds_3d_draw(ctx, &cube, &bad) == -BMDS_EINVAL);

        // The model stack.
        CHECK(bmds_3d_pop(ctx) == -BMDS_EINVAL);
        for (uint32_t i = 0; i + 1 < BMDS_3D_STACK; ++i) REQUIRE(bmds_3d_push(ctx) == 0);
        CHECK(bmds_3d_push(ctx) == -BMDS_ENOSPC);

        bmds_3d_mesh_free(&sphere);
        bmds_3d_mesh_free(&cube);
        bmds_3d_destroy(ctx);
        CHECK(FakeMemory::live() == before);
}

TEST_CASE("3d: projecting and sorting points") {
        Picture     pic(64, 32, BMDS_PIXEL_L8);
        Scene       s(&pic.canvas);
        bmds_mat4_t p;
        bmds_mat4_perspective(&p, Pi / 2.0f, 2.0f, 1.0f, 100.0f);
        bmds_3d_set_projection(s.ctx, &p);
        const float         points[] = {0.0f, 0.0f, -2.0f, 0.0f, 0.0f, -8.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, -5.0f};
        bmds_3d_projected_t out[4];
        bmds_3d_project(s.ctx, points, 4, out);
        CHECK(near(out[0].x, 32.0f));
        CHECK(near(out[0].y, 16.0f));
        CHECK(near(out[0].depth, 2.0f));
        CHECK(near(out[0].scale, 8.0f)); // 16 pixels a half height at distance 1, over 2
        CHECK(out[0].visible == 1);
        CHECK(out[2].visible == 0); // behind
        uint16_t order[4] = {0, 1, 2, 3};
        bmds_3d_sort_depth(out, order, 4);
        CHECK(order[0] == 1);
        CHECK(order[1] == 3);
        CHECK(order[2] == 0);
        CHECK(order[3] == 2);
}

namespace {

        // A caller's mesh from vectors.
        struct Mesh {
                        std::vector<float>    pos;
                        std::vector<float>    normals;
                        std::vector<uint16_t> tri;
                        bmds_3d_mesh_t        m{};

                        uint16_t add(float x, float y, float z = 0.0f) {
                                pos.insert(pos.end(), {x, y, z});
                                return uint16_t(pos.size() / 3 - 1);
                        }
                        void face(uint16_t a, uint16_t b, uint16_t c) { tri.insert(tri.end(), {a, b, c}); }
                        const bmds_3d_mesh_t *mesh() {
                                m = bmds_3d_mesh_t{};
                                m.size = sizeof(m);
                                m.vertex_count = uint32_t(pos.size() / 3);
                                m.positions = pos.data();
                                m.normals = normals.empty() ? nullptr : normals.data();
                                m.triangle_count = uint32_t(tri.size() / 3);
                                m.triangles = tri.data();
                                return &m;
                        }
        };

        // How many times each pixel is drawn when each triangle of @p mesh is drawn alone.
        std::vector<int> coverage(Mesh &mesh, int32_t w, int32_t h) {
                std::vector<int> count(size_t(w) * h, 0);
                Picture          pic(w, h, BMDS_PIXEL_L8);
                Scene            s(&pic.canvas);
                const auto       ink = material(BMDS_3D_FLAT, 1, BMDS_3D_UNLIT);
                for (size_t t = 0; t < mesh.tri.size(); t += 3) {
                        Mesh one;
                        one.pos = mesh.pos;
                        one.tri = {mesh.tri[t], mesh.tri[t + 1], mesh.tri[t + 2]};
                        std::fill(pic.pixels.begin(), pic.pixels.end(), 0);
                        bmds_3d_begin(s.ctx);
                        REQUIRE(bmds_3d_draw(s.ctx, one.mesh(), &ink) == 0);
                        bmds_3d_end(s.ctx);
                        for (size_t i = 0; i < count.size(); ++i) count[i] += pic.pixels[i];
                }
                return count;
        }

} // namespace

TEST_CASE("3d: triangles sharing edges draw each pixel once (the fill rule)") {
        SUBCASE("a fan round a point, at sub-pixel places") {
                Mesh           fan;
                const uint16_t c = fan.add(20.3f, 17.7f);
                const int      n = 13;
                for (int i = 0; i < n; ++i) {
                        const float a = 2.0f * Pi * float(i) / float(n), r = 11.0f + 3.0f * float(i % 3);
                        fan.add(20.3f + r * std::cos(a), 17.7f + r * std::sin(a));
                }
                for (int i = 0; i < n; ++i) fan.face(c, uint16_t(1 + i), uint16_t(1 + (i + 1) % n));
                const auto count = coverage(fan, 48, 40);
                int        covered = 0;
                for (int v : count) {
                        CHECK(v <= 1);
                        covered += v;
                }
                CHECK(covered > 400);
                CHECK(count[size_t(17) * 48 + 20] == 1);
        }
        SUBCASE("a jittered grid of quads: no gap, no overlap inside") {
                Mesh                                  grid;
                std::mt19937                          rng(3);
                std::uniform_real_distribution<float> jitter(-1.3f, 1.3f);
                const int                             n = 8;
                for (int j = 0; j <= n; ++j)
                        for (int i = 0; i <= n; ++i)
                                grid.add(6.0f + 6.0f * float(i) + (i % n != 0 ? jitter(rng) : 0.0f),
                                         5.0f + 5.0f * float(j) + (j % n != 0 ? jitter(rng) : 0.0f));
                for (int j = 0; j < n; ++j)
                        for (int i = 0; i < n; ++i) {
                                const uint16_t v = uint16_t(j * (n + 1) + i);
                                grid.face(v, uint16_t(v + 1), uint16_t(v + n + 2));
                                grid.face(v, uint16_t(v + n + 2), uint16_t(v + n + 1));
                        }
                const auto count = coverage(grid, 64, 52);
                for (int y = 0; y < 52; ++y)
                        for (int x = 0; x < 64; ++x) {
                                const int v = count[size_t(y) * 64 + x];
                                CHECK(v <= 1);
                                // The pixels whose centres are inside the grid's square: every one.
                                if (x + 0.5f > 6.0f && x + 0.5f < 54.0f && y + 0.5f > 5.0f && y + 0.5f < 45.0f)
                                        CHECK(v == 1);
                        }
        }
}

TEST_CASE("3d: flat solids lit, and the painter's order") {
        const size_t before = FakeMemory::live();
        {
                Picture pic(32, 32, BMDS_PIXEL_L8);
                Scene   s(&pic.canvas);
                Mesh    quad; // facing the viewer (+z), counter-clockwise seen from it
                quad.add(4, 28, 0), quad.add(28, 28, 0), quad.add(28, 4, 0), quad.add(4, 4, 0);
                quad.face(0, 1, 2), quad.face(0, 2, 3);
                auto lit = material(BMDS_3D_FLAT, 0, BMDS_3D_CULL_BACK);
                lit.ramp_first = 32;
                lit.ramp_count = 32;
                bmds_3d_light_t sun{sizeof(sun), BMDS_3D_LIGHT_DIRECTIONAL, BMDS_3D_LIGHT_VIEW, 0, 0, 1, 1.0f};
                REQUIRE(bmds_3d_set_light(s.ctx, 0, &sun) == 0);
                bmds_3d_set_ambient(s.ctx, 0.0f);
                bmds_3d_begin(s.ctx);
                REQUIRE(bmds_3d_draw(s.ctx, quad.mesh(), &lit) == 0);
                bmds_3d_end(s.ctx);
                CHECK(pic.l8(16, 16) == 63);
                CHECK(pic.l8(3, 16) == 0);
                bmds_3d_stats_t st{};
                st.size = sizeof(st);
                bmds_3d_stats(s.ctx, &st);
                CHECK(st.triangles == 2);
                CHECK(st.drawn == 2);
                CHECK(st.pixels == 24 * 24);

                // The light behind it: only the ambient; half of it.
                sun.z = -1;
                REQUIRE(bmds_3d_set_light(s.ctx, 0, &sun) == 0);
                bmds_3d_begin(s.ctx);
                REQUIRE(bmds_3d_draw(s.ctx, quad.mesh(), &lit) == 0);
                bmds_3d_end(s.ctx);
                CHECK(pic.l8(16, 16) == 32);
                REQUIRE(bmds_3d_set_light(s.ctx, 0, nullptr) == 0);
                bmds_3d_set_ambient(s.ctx, 0.5f);
                bmds_3d_begin(s.ctx);
                REQUIRE(bmds_3d_draw(s.ctx, quad.mesh(), &lit) == 0);
                bmds_3d_end(s.ctx);
                CHECK(pic.l8(16, 16) == 48);

                // Wound away from the viewer: culled.
                Mesh back = quad;
                back.tri = {0, 2, 1, 0, 3, 2};
                std::fill(pic.pixels.begin(), pic.pixels.end(), 0);
                bmds_3d_begin(s.ctx);
                REQUIRE(bmds_3d_draw(s.ctx, back.mesh(), &lit) == 0);
                bmds_3d_end(s.ctx);
                bmds_3d_stats(s.ctx, &st);
                CHECK(st.backfaces == 2);
                CHECK(pic.lit().empty());

                // Near drawn first, far second: near still on top after the sort.
                Mesh nearQuad = quad, farQuad = quad;
                for (size_t i = 2; i < nearQuad.pos.size(); i += 3) nearQuad.pos[i] = 1.0f;
                for (size_t i = 0; i < farQuad.pos.size(); i += 3) farQuad.pos[i] += 6.0f, farQuad.pos[i + 2] = -1.0f;
                const auto two = material(BMDS_3D_FLAT, 2, BMDS_3D_UNLIT),
                           three = material(BMDS_3D_FLAT, 3, BMDS_3D_UNLIT);
                bmds_3d_begin(s.ctx);
                REQUIRE(bmds_3d_draw(s.ctx, nearQuad.mesh(), &two) == 0);
                REQUIRE(bmds_3d_draw(s.ctx, farQuad.mesh(), &three) == 0);
                bmds_3d_end(s.ctx);
                CHECK(pic.l8(16, 16) == 2); // both cover it
                CHECK(pic.l8(30, 16) == 3); // only the far one
                CHECK(pic.canvas.damage_count >= 1);
        }
        CHECK(FakeMemory::live() == before);
}

TEST_CASE("3d: Gouraud spheres stay in their ramp, brightest facing the light, in every format") {
        for (uint32_t format : {uint32_t(BMDS_PIXEL_L8), uint32_t(BMDS_PIXEL_RGB565), uint32_t(BMDS_PIXEL_BGR24)}) {
                CAPTURE(format);
                Picture    pic(64, 64, format);
                bmds_3d_t *ctx = nullptr;
                REQUIRE(bmds_3d_create(nullptr, &ctx) == 0);
                bmds_3d_set_target(ctx, &pic.canvas);
                bmds_mat4_t p, v;
                bmds_mat4_perspective(&p, Pi / 3.0f, 1.0f, 0.5f, 50.0f);
                bmds_mat4_look_at(&v, bmds_vec3(0, 0, 4), bmds_vec3(0, 0, 0), bmds_vec3(0, 1, 0));
                bmds_3d_set_projection(ctx, &p);
                bmds_3d_set_view(ctx, &v);
                bmds_3d_light_t sun{sizeof(sun), BMDS_3D_LIGHT_DIRECTIONAL, BMDS_3D_LIGHT_VIEW, 0, 0, 1, 0.75f};
                REQUIRE(bmds_3d_set_light(ctx, 0, &sun) == 0);
                bmds_3d_shape_t shape{};
                shape.size = sizeof(shape);
                shape.kind = BMDS_3D_SHAPE_SPHERE;
                shape.segments = 24;
                shape.rings = 16;
                bmds_3d_mesh_t sphere{};
                REQUIRE(bmds_3d_mesh_make(&shape, 0, &sphere) == 0);
                auto m = material(BMDS_3D_GOURAUD, 0, BMDS_3D_CULL_BACK);
                m.ramp_first = 100;
                m.ramp_count = 50;
                m.rgb = 0xffffff;
                bmds_3d_begin(ctx);
                REQUIRE(bmds_3d_draw(ctx, &sphere, &m) == 0);
                bmds_3d_end(ctx);
                const uint32_t middle = pic.value(32, 32), edge = pic.value(32, 13);
                if (format == BMDS_PIXEL_L8) {
                        for (const auto &q : pic.lit()) {
                                CHECK(pic.l8(q.first, q.second) >= 100);
                                CHECK(pic.l8(q.first, q.second) <= 149);
                        }
                        CHECK(middle == 149); // 0.25 + 0.75 facing it
                        CHECK(edge < middle);
                } else {
                        const uint32_t green = format == BMDS_PIXEL_RGB565 ? (middle >> 5 & 63) : (middle >> 8 & 255);
                        const uint32_t greenEdge = format == BMDS_PIXEL_RGB565 ? (edge >> 5 & 63) : (edge >> 8 & 255);
                        CHECK(green > greenEdge);
                        CHECK(middle == (format == BMDS_PIXEL_RGB565 ? 0xffffu : 0xffffffu));
                }
                bmds_3d_mesh_free(&sphere);
                bmds_3d_destroy(ctx);
        }
}

TEST_CASE("3d: solids clipped: through the eye, huge, NaN, never outside the clip") {
        for (uint32_t format : {uint32_t(BMDS_PIXEL_L8), uint32_t(BMDS_PIXEL_RGB565), uint32_t(BMDS_PIXEL_BGR24)}) {
                CAPTURE(format);
                Picture           pic(48, 48, format);
                const bmds_rect_t clip = {8, 8, 32, 32};
                bmds_canvas_set_clip(&pic.canvas, &clip);
                bmds_3d_t *ctx = nullptr;
                REQUIRE(bmds_3d_create(nullptr, &ctx) == 0);
                bmds_3d_set_target(ctx, &pic.canvas);
                bmds_mat4_t p;
                bmds_mat4_perspective(&p, 1.2f, 1.0f, 0.5f, 50.0f);
                bmds_3d_set_projection(ctx, &p);
                std::mt19937                          rng(11);
                std::uniform_real_distribution<float> big(-200.0f, 200.0f), z(-80.0f, 20.0f);
                auto                                  m = material(BMDS_3D_GOURAUD, 0, 0);
                m.ramp_first = 10;
                m.ramp_count = 20;
                m.rgb = 0x80ff40;
                Mesh tri;
                tri.add(0, 0, 0), tri.add(0, 0, 0), tri.add(0, 0, 0);
                tri.face(0, 1, 2);
                bmds_3d_begin(ctx);
                for (int i = 0; i < 3000; ++i) {
                        for (float &f : tri.pos) f = big(rng);
                        for (size_t k = 2; k < 9; k += 3) tri.pos[k] = z(rng);
                        if (i % 500 == 0) tri.pos[4] = NAN;
                        REQUIRE(bmds_3d_draw(ctx, tri.mesh(), &m) == 0);
                        if (i % 100 == 99) bmds_3d_end(ctx);
                }
                bmds_3d_end(ctx);
                bmds_3d_stats_t st{};
                st.size = sizeof(st);
                bmds_3d_stats(ctx, &st);
                CHECK(st.clipped > 0);
                CHECK(st.drawn > 0);
                CHECK(!pic.lit().empty());
                for (const auto &q : pic.lit()) {
                        CHECK(q.first >= 8);
                        CHECK(q.first < 40);
                        CHECK(q.second >= 8);
                        CHECK(q.second < 40);
                }
                bmds_3d_destroy(ctx);
        }
}

TEST_CASE("3d: a full queue is drawn and goes on") {
        const size_t     before = FakeMemory::live();
        Picture          pic(64, 64, BMDS_PIXEL_L8);
        bmds_3d_t       *ctx = nullptr;
        bmds_3d_config_t config{};
        config.size = sizeof(config);
        config.max_triangles = 16;
        config.max_draws = 2;
        REQUIRE(bmds_3d_create(&config, &ctx) == 0);
        bmds_3d_set_target(ctx, &pic.canvas);
        bmds_mat4_t p, v;
        bmds_mat4_perspective(&p, Pi / 3.0f, 1.0f, 0.5f, 50.0f);
        bmds_mat4_look_at(&v, bmds_vec3(0, 0, 5), bmds_vec3(0, 0, 0), bmds_vec3(0, 1, 0));
        bmds_3d_set_projection(ctx, &p);
        bmds_3d_set_view(ctx, &v);
        bmds_3d_shape_t shape{};
        shape.size = sizeof(shape);
        shape.kind = BMDS_3D_SHAPE_TORUS;
        bmds_3d_mesh_t torus{};
        REQUIRE(bmds_3d_mesh_make(&shape, 0, &torus) == 0);
        auto m = material(BMDS_3D_FLAT, 0, BMDS_3D_CULL_BACK);
        m.ramp_first = 1;
        m.ramp_count = 16;
        bmds_3d_begin(ctx);
        for (int i = 0; i < 3; ++i) REQUIRE(bmds_3d_draw(ctx, &torus, &m) == 0);
        bmds_3d_end(ctx);
        bmds_3d_stats_t st{};
        st.size = sizeof(st);
        bmds_3d_stats(ctx, &st);
        CHECK(st.flushes > 10);
        CHECK(st.drawn > 0);
        CHECK(st.triangles + st.backfaces >= 3 * torus.triangle_count - 3 * 8); // a few wholly off the view at most
        for (const auto &q : pic.lit()) CHECK(pic.l8(q.first, q.second) <= 16);
        CHECK(!pic.lit().empty());
        bmds_3d_light_t bad{sizeof(bad), 9, 0, 0, 0, 0, 0};
        CHECK(bmds_3d_set_light(ctx, 0, &bad) == -BMDS_EINVAL);
        CHECK(bmds_3d_set_light(ctx, BMDS_3D_LIGHTS, nullptr) == -BMDS_EINVAL);
        bmds_3d_mesh_free(&torus);
        bmds_3d_destroy(ctx);
        CHECK(FakeMemory::live() == before);
}

TEST_CASE("3d: draws ordered whole by default, triangles mixed on request") {
        Picture pic(32, 32, BMDS_PIXEL_L8);
        Scene   s(&pic.canvas);
        // A: a quad tilted in depth (near on the left, far on the right); its upper-left triangle's
        // mean depth is nearer than B's, though at B's place A is far behind it.
        Mesh a;
        a.add(2, 30, 5), a.add(30, 30, -5), a.add(30, 2, -5), a.add(2, 2, 5);
        a.face(0, 1, 2), a.face(0, 2, 3);
        Mesh b;
        b.add(20, 7, 1), b.add(24, 7, 1), b.add(24, 3, 1), b.add(20, 3, 1);
        b.face(0, 1, 2), b.face(0, 2, 3);
        const auto inkA = material(BMDS_3D_FLAT, 5, BMDS_3D_UNLIT), inkB = material(BMDS_3D_FLAT, 9, BMDS_3D_UNLIT);
        for (uint32_t order : {uint32_t(BMDS_3D_ORDER_DRAWS), uint32_t(BMDS_3D_ORDER_TRIANGLES)}) {
                CAPTURE(order);
                REQUIRE(bmds_3d_set_order(s.ctx, order) == 0);
                bmds_3d_begin(s.ctx);
                REQUIRE(bmds_3d_draw(s.ctx, b.mesh(), &inkB) == 0);
                REQUIRE(bmds_3d_draw(s.ctx, a.mesh(), &inkA) == 0);
                bmds_3d_end(s.ctx);
                CHECK(pic.l8(22, 5) == (order == BMDS_3D_ORDER_DRAWS ? 9 : 5));
        }
        CHECK(bmds_3d_set_order(s.ctx, 7) == -BMDS_EINVAL);
}
