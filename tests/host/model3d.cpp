/**
 * @file      model3d.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d's models (devplan/3d.md §12d): bmds-mesh3d's OBJ and Bézier patch readers and its
 * writer, round trips through the loader, the teapot, and the loader against broken and fuzzed
 * files (every one refused or safe to draw).
 */

#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <bmds/3d.h>
#include <bmds/3dfile.h>

#include "../../sdk/tools/mesh3d/mesh3d.h"
#include "fakememory.h"

namespace {

        // A file's bytes, 4-aligned as the loader needs.
        struct File {
                        std::vector<uint32_t> words;
                        size_t                bytes = 0;

                        uint8_t       *data() { return reinterpret_cast<uint8_t *>(words.data()); }
                        const uint8_t *data() const { return reinterpret_cast<const uint8_t *>(words.data()); }

                        uint32_t u32(size_t at) const {
                                uint32_t v;
                                std::memcpy(&v, data() + at, 4);
                                return v;
                        }
                        void setU32(size_t at, uint32_t v) { std::memcpy(data() + at, &v, 4); }
                        void setU16(size_t at, uint16_t v) { std::memcpy(data() + at, &v, 2); }
        };

        m3d_options_t defaults() {
                m3d_options_t o;
                m3d_options_init(&o);
                return o;
        }

        // Reads @p text (OBJ, or patches with @p bpt) and writes it; empty with @p error set if refused.
        File convert(const std::string &text, const m3d_options_t &o, bool bpt = false, std::string *error = nullptr) {
                m3d_scene_t scene{};
                char        err[256] = "";
                File        f;
                int         r = bpt ? m3d_read_bpt(text.data(), text.size(), "patches", &o, &scene, err, sizeof(err))
                                    : m3d_read_obj(text.data(), text.size(), "model", &o, &scene, err, sizeof(err));
                if (r == 0) r = m3d_finish(&scene, &o, err, sizeof(err));
                uint8_t *out = nullptr;
                size_t   n = 0;
                if (r == 0) r = m3d_write(&scene, &o, &out, &n, err, sizeof(err));
                if (r == 0) {
                        f.words.assign((n + 3) / 4, 0);
                        std::memcpy(f.data(), out, n);
                        f.bytes = n;
                }
                std::free(out);
                m3d_scene_free(&scene);
                if (error != nullptr) *error = err;
                if (error == nullptr) REQUIRE_MESSAGE(r == 0, err);
                return f;
        }

        struct Model {
                        bmds_3d_model_t *m = nullptr;
                        Model() = default;
                        Model(const Model &) = delete;
                        Model &operator=(const Model &) = delete;
                        ~Model() { bmds_3d_model_free(m); }
                        int load(const File &f, size_t bytes) {
                                bmds_3d_model_free(m);
                                m = nullptr;
                                return bmds_3d_model_load_memory(f.data(), uint32_t(bytes), &m);
                        }
                        int load(const File &f) { return load(f, f.bytes); }
        };

        std::string readText(const char *path) {
                std::ifstream     in(path, std::ios::binary);
                std::stringstream s;
                s << in.rdbuf();
                return s.str();
        }

        const char *Cube = "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
                           "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n";
        // The cube's six faces, counter-clockwise from outside.
        const char *CubeFaces = "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 4 8 7 3\nf 1 5 8 4\nf 2 3 7 6\n";

        bmds_vec3_t at(const bmds_3d_mesh_t *m, uint32_t v) {
                return bmds_vec3(m->positions[3 * v], m->positions[3 * v + 1], m->positions[3 * v + 2]);
        }

        // Every triangle's winding agrees with its vertices' normals (fronts outside).
        uint32_t disagreeing(const bmds_3d_mesh_t *m) {
                uint32_t bad = 0;
                for (uint32_t t = 0; t < m->triangle_count; ++t) {
                        const uint16_t   *i = m->triangles + 3 * t;
                        const bmds_vec3_t g = bmds_vec3_cross(bmds_vec3_sub(at(m, i[1]), at(m, i[0])),
                                                              bmds_vec3_sub(at(m, i[2]), at(m, i[0])));
                        float             d = 0.0f;
                        for (int k = 0; k < 3; ++k)
                                d += g.x * m->normals[3 * i[k]] + g.y * m->normals[3 * i[k] + 1] +
                                     g.z * m->normals[3 * i[k] + 2];
                        if (d <= 0.0f) ++bad;
                }
                return bad;
        }

        float volume(const bmds_3d_mesh_t *m) {
                float v = 0.0f;
                for (uint32_t t = 0; t < m->triangle_count; ++t) {
                        const uint16_t *i = m->triangles + 3 * t;
                        v += bmds_vec3_dot(at(m, i[0]), bmds_vec3_cross(at(m, i[1]), at(m, i[2]))) / 6.0f;
                }
                return v;
        }

        // The offset of section @p index's entry in the table.
        size_t entry(const File &f, uint32_t index) {
                uint16_t header;
                std::memcpy(&header, f.data() + 6, 2);
                return header + 16 * size_t(index);
        }

} // namespace

TEST_CASE("3d models: an OBJ's groups become named meshes, read back in place") {
        const size_t      before = FakeMemory::live();
        const std::string obj = std::string(Cube) + "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n" +
                                "o top\nf 4/1 8/2 7/3 3/4\n" // +y, with uvs, no normals
                                "g rest\nf 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 1 5 8 4\nf 2 3 7 6\n";
        const File f = convert(obj, defaults());
        {
                Model m;
                REQUIRE(m.load(f) == 0);
                REQUIRE(bmds_3d_model_mesh_count(m.m) == 2);
                CHECK(std::string(bmds_3d_model_mesh_name(m.m, 0)) == "top");
                CHECK(std::string(bmds_3d_model_mesh_name(m.m, 1)) == "rest");
                CHECK(bmds_3d_model_find(m.m, "rest") == 1);
                CHECK(bmds_3d_model_find(m.m, "lid") == -BMDS_ENOENT);
                CHECK(bmds_3d_model_mesh(m.m, 2) == nullptr);
                CHECK(bmds_3d_model_mesh_name(m.m, 2) == nullptr);

                const bmds_3d_mesh_t *top = bmds_3d_model_mesh(m.m, 0);
                REQUIRE(top->vertex_count == 4);
                REQUIRE(top->triangle_count == 2);
                REQUIRE(top->normals != nullptr);
                REQUIRE(top->uvs != nullptr);
                for (uint32_t v = 0; v < 4; ++v) {
                        CHECK(top->normals[3 * v + 1] == doctest::Approx(1.0f));
                        // OBJ's v goes up the image; ours goes down its rows.
                        const float x = top->positions[3 * v], z = top->positions[3 * v + 2];
                        if (x < 0 && z < 0) CHECK((top->uvs[2 * v] == 0.0f && top->uvs[2 * v + 1] == 1.0f));
                        if (x < 0 && z > 0) CHECK((top->uvs[2 * v] == 1.0f && top->uvs[2 * v + 1] == 1.0f));
                }
                CHECK(top->edge_count == 4); // the quad's diagonal isn't an edge
                CHECK(top->bounds[3] == doctest::Approx(std::sqrt(2.0f)).epsilon(1e-3));
                CHECK(disagreeing(top) == 0);

                const bmds_3d_mesh_t *rest = bmds_3d_model_mesh(m.m, 1);
                CHECK(rest->vertex_count == 20); // 60°: a cube's corners stay sharp
                CHECK(rest->triangle_count == 10);
                CHECK(rest->uvs == nullptr);
                CHECK(disagreeing(rest) == 0);
                CHECK(rest->edge_count == 12); // a cube's edges (the open top's 4 with one face each)
                for (uint32_t e = 0; e < rest->edge_count; ++e) {
                        CHECK(rest->edge_faces[2 * e] < rest->triangle_count);
                        CHECK((rest->edge_faces[2 * e + 1] < rest->triangle_count ||
                               rest->edge_faces[2 * e + 1] == BMDS_3D_NO_FACE));
                }
        }
        CHECK(FakeMemory::live() == before);
}

TEST_CASE("3d models: OBJ normals by crease, negative indices, polygons, merging, errors") {
        m3d_options_t o = defaults();
        {
                const File f = convert(std::string(Cube) + CubeFaces, o);
                Model      m;
                REQUIRE(m.load(f) == 0);
                const bmds_3d_mesh_t *c = bmds_3d_model_mesh(m.m, 0);
                CHECK(c->vertex_count == 24);
                CHECK(c->triangle_count == 12);
                CHECK(volume(c) == doctest::Approx(8.0f));
                CHECK(std::string(bmds_3d_model_mesh_name(m.m, 0)) == "model");
        }
        o.crease = 100.0f; // past 90°: smooth corners
        {
                const File f = convert(std::string(Cube) + CubeFaces, o);
                Model      m;
                REQUIRE(m.load(f) == 0);
                const bmds_3d_mesh_t *c = bmds_3d_model_mesh(m.m, 0);
                REQUIRE(c->vertex_count == 8);
                for (uint32_t v = 0; v < 8; ++v)
                        for (int k = 0; k < 3; ++k)
                                CHECK(c->normals[3 * v + k] ==
                                      doctest::Approx(c->positions[3 * v + k] / std::sqrt(3.0f)).epsilon(1e-4));
                CHECK(disagreeing(c) == 0);
        }
        o = defaults();
        {
                // A pentagon by negative indices, fanned; normals given (and normalised).
                const File f = convert("v 0 0 0\nv 2 0 0\nv 3 2 0\nv 1 3 0\nv -1 2 0\nvn 0 0 5\n"
                                       "f -5//-1 -4//-1 -3//-1 -2//-1 -1//-1\n",
                                       o);
                Model      f2;
                REQUIRE(f2.load(f) == 0);
                const bmds_3d_mesh_t *p = bmds_3d_model_mesh(f2.m, 0);
                CHECK(p->vertex_count == 5);
                CHECK(p->triangle_count == 3);
                CHECK(p->normals[2] == 1.0f);
                CHECK(disagreeing(p) == 0);
        }
        {
                // A bent quad: its triangles aren't in one plane, its diagonal still no edge; a
                // pentagon's two spokes neither.
                const File f = convert("v 0 0 0\nv 1 0 0\nv 1 1 0.4\nv 0 1 0\nf 1 2 3 4\n"
                                       "v 3 0 0\nv 4 0 0\nv 4.5 1 0\nv 3.5 2 0\nv 2.5 1 0\nf 5 6 7 8 9\n",
                                       o);
                Model      m;
                REQUIRE(m.load(f) == 0);
                CHECK(bmds_3d_model_mesh(m.m, 0)->edge_count == 4 + 5);
        }
        o.merge = 1;
        {
                const File f = convert(std::string(Cube) + "o a\nf 1 4 3 2\no b\nf 5 6 7 8\n", o);
                Model      m;
                REQUIRE(m.load(f) == 0);
                CHECK(bmds_3d_model_mesh_count(m.m) == 1);
                CHECK(bmds_3d_model_mesh(m.m, 0)->triangle_count == 4);
        }
        o = defaults();
        o.no_normals = o.no_uvs = o.no_edges = 1;
        {
                const File f = convert(std::string(Cube) + "vt 0 0\n" + CubeFaces, o);
                Model      m;
                REQUIRE(m.load(f) == 0);
                const bmds_3d_mesh_t *c = bmds_3d_model_mesh(m.m, 0);
                CHECK(c->vertex_count == 8); // nothing but positions left to tell them apart
                CHECK(c->normals == nullptr);
                CHECK(c->uvs == nullptr);
                CHECK(c->edge_count == 0);
                CHECK(c->edges == nullptr);
        }
        o = defaults();
        o.up_z = 1;
        o.fit = 1;
        o.scale = 2.0f;
        {
                const File f = convert("v 10 10 10\nv 12 10 10\nv 10 10 12\nf 1 2 3\n", o); // facing -y, z up
                Model      m;
                REQUIRE(m.load(f) == 0);
                const bmds_3d_mesh_t *t = bmds_3d_model_mesh(m.m, 0);
                CHECK(t->bounds[3] == doctest::Approx(2.0f).epsilon(1e-3));
                CHECK(t->normals[2] == doctest::Approx(1.0f)); // -y in Z-up is +z in Y-up
                CHECK(t->positions[1] == doctest::Approx(-std::sqrt(0.5f) * 2.0f).epsilon(1e-3));
        }

        std::string err;
        CHECK(convert(std::string(Cube) + "f 1 2 9\n", defaults(), false, &err).bytes == 0);
        CHECK(err.find("out of range") != std::string::npos);
        CHECK(convert("v 1 2\n", defaults(), false, &err).bytes == 0);
        CHECK(convert(std::string(Cube) + "f 1 2\n", defaults(), false, &err).bytes == 0);
        CHECK(convert(std::string(Cube) + "f 1 2 x\n", defaults(), false, &err).bytes == 0);
        CHECK(convert(std::string(Cube), defaults(), false, &err).bytes == 0);
        CHECK(err == "no triangles");
}

TEST_CASE("3d models: Bézier patches, and the teapot") {
        m3d_options_t o = defaults();
        o.detail = 2;
        // A flat patch: x along u, y along v, so its front is +z.
        std::string flat = "1\n3 3\n";
        for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) flat += std::to_string(j) + " " + std::to_string(i) + " 0\n";
        {
                const File f = convert(flat, o, true);
                Model      m;
                REQUIRE(m.load(f) == 0);
                const bmds_3d_mesh_t *p = bmds_3d_model_mesh(m.m, 0);
                CHECK(p->vertex_count == 9);
                CHECK(p->triangle_count == 8);
                for (uint32_t v = 0; v < 9; ++v) CHECK(p->normals[3 * v + 2] == doctest::Approx(1.0f));
                CHECK(disagreeing(p) == 0);
                CHECK(p->edge_count == 12); // the grid's lines, no diagonals
                REQUIRE(p->uvs != nullptr);
        }
        std::string err;
        CHECK(convert("1\n3 2\n", o, true, &err).bytes == 0);
        CHECK(convert("2\n" + flat.substr(2), o, true, &err).bytes == 0);
        CHECK(err.find("patch 2") != std::string::npos);

        // Newell's teapot (tools/teapot-bpt): Z-up, made one unit across, as the demo will.
        const std::string teapot = readText(BMDS_TEST_TEAPOT);
        REQUIRE(!teapot.empty());
        o = defaults();
        o.up_z = o.fit = o.no_uvs = 1;
        for (uint32_t detail : {3u, 4u}) {
                o.detail = detail;
                const File f = convert(teapot, o, true);
                Model      m;
                REQUIRE(m.load(f) == 0);
                const bmds_3d_mesh_t *t = bmds_3d_model_mesh(m.m, 0);
                CAPTURE(detail);
                // 38 patches of 2·N² triangles, less those where the lid and bottom pinch to a point.
                CHECK(t->triangle_count == (detail == 3 ? 648u : 1168u));
                CHECK(t->vertex_count < t->triangle_count); // seams welded
                CHECK(disagreeing(t) == 0);
                CHECK(volume(t) > 0.5f); // fronts outside
                CHECK(t->bounds[3] == doctest::Approx(1.0f).epsilon(1e-3));
        }
}

TEST_CASE("3d models: broken files are refused") {
        const File   good = convert(std::string(Cube) + "vt 0 0\no box\nf 1/1 4/1 3/1 2/1\nf 5 6 7 8\n", defaults());
        const size_t before = FakeMemory::live();
        Model        m;
        REQUIRE(m.load(good) == 0);
        const size_t names = entry(good, 1), mesh = good.u32(entry(good, 0) + 8);
        REQUIRE(good.u32(names) == BMDS_3D_SECTION_NAMES);
        const size_t   name = good.u32(names + 8) + 8 + 8; // the first name's offset field
        const uint32_t tris = mesh + good.u32(mesh + 52), faces = mesh + good.u32(mesh + 60);

        for (size_t n = 0; n < good.bytes; ++n) {
                CAPTURE(n);
                CHECK(m.load(good, n) == -BMDS_EILSEQ); // every truncation
        }
        auto broken = [&](auto change) {
                File f = good;
                change(f);
                return m.load(f);
        };
        CHECK(broken([](File &f) { f.data()[0] = 'X'; }) == -BMDS_EILSEQ);
        CHECK(broken([](File &f) { f.setU16(4, 2); }) == -BMDS_ENOTSUP); // a later version
        CHECK(broken([](File &f) { f.setU16(4, 0); }) == -BMDS_EILSEQ);
        CHECK(broken([](File &f) { f.setU32(16, 1); }) == -BMDS_ENOTSUP);                // a header flag
        CHECK(broken([](File &f) { f.setU16(6, 20); }) == -BMDS_EILSEQ);                 // header too short
        CHECK(broken([](File &f) { f.setU32(12, 1000); }) == -BMDS_EILSEQ);              // sections past the end
        CHECK(broken([&](File &f) { f.setU32(names + 8, 2); }) == -BMDS_EILSEQ);         // misaligned section
        CHECK(broken([&](File &f) { f.setU32(names + 12, 1u << 20); }) == -BMDS_EILSEQ); // past the end
        CHECK(broken([&](File &f) { f.setU32(mesh + 4, 1); }) == -BMDS_ENOTSUP);         // a mesh flag
        CHECK(broken([&](File &f) { f.setU32(mesh + 8, 0); }) == -BMDS_EILSEQ);          // no vertices
        CHECK(broken([&](File &f) { f.setU32(mesh + 8, 60000); }) == -BMDS_EILSEQ);      // arrays past the section
        CHECK(broken([&](File &f) { f.setU16(tris, 8); }) == -BMDS_EILSEQ);              // a vertex that isn't there
        CHECK(broken([&](File &f) { f.setU16(faces, 4); }) == -BMDS_EILSEQ);             // a triangle that isn't there
        CHECK(broken([&](File &f) { f.setU32(mesh + 40, f.u32(mesh + 40) + 2); }) == -BMDS_EILSEQ); // misaligned
        CHECK(broken([&](File &f) { f.setU32(mesh + 36, 0x7fc00000u); }) == -BMDS_EILSEQ);          // NaN radius
        CHECK(broken([&](File &f) { f.setU32(mesh + 36, 0xbf800000u); }) == -BMDS_EILSEQ);          // -1 radius
        CHECK(broken([&](File &f) { f.setU32(mesh + 24, 0x7f800000u); }) == -BMDS_EILSEQ);          // infinite centre
        CHECK(broken([&](File &f) { f.setU32(mesh + f.u32(mesh + 40) + 4, 0xffc00000u); }) == -BMDS_EILSEQ); // NaN y
        CHECK(broken([&](File &f) { f.setU32(mesh + f.u32(mesh + 44), 0x7f800001u); }) == -BMDS_EILSEQ);     // normal
        CHECK(broken([&](File &f) { f.setU32(mesh + f.u32(mesh + 48) + 4, 0xff800000u); }) == -BMDS_EILSEQ); // uv
        CHECK(broken([&](File &f) { f.setU32(mesh + 52, 0); }) == -BMDS_EILSEQ); // triangles without an array
        CHECK(broken([&](File &f) { f.setU32(name, uint32_t(good.bytes)); }) == -BMDS_EILSEQ); // name past the end

        // An unknown section is skipped, unless it is required.
        CHECK(broken([&](File &f) { f.setU32(names, 99); }) == 0);
        CHECK(bmds_3d_model_mesh_name(m.m, 0) == nullptr);
        CHECK(broken([&](File &f) {
                      f.setU32(names, 99);
                      f.setU32(names + 4, BMDS_3D_SECTION_REQUIRED);
              }) == -BMDS_ENOTSUP);
        // Arrays must lie past the mesh's fixed part, however long a later version makes it.
        CHECK(broken([&](File &f) { f.setU32(mesh, 68); }) == -BMDS_EILSEQ);
        CHECK(broken([&](File &f) { f.setU32(mesh, 62); }) == -BMDS_EILSEQ);
        bmds_3d_model_free(m.m);
        m.m = nullptr;
        CHECK(bmds_3d_model_load_memory(good.data() + 2, uint32_t(good.bytes), &m.m) == -BMDS_EINVAL);
        CHECK(m.m == nullptr);
        CHECK(bmds_3d_model_load_memory(nullptr, 0, &m.m) == -BMDS_EINVAL);
        CHECK(FakeMemory::live() == before);
}

TEST_CASE("3d models: fuzzed files are refused or safe to draw") {
        m3d_options_t o = defaults();
        o.detail = 2;
        const File teapot = convert(readText(BMDS_TEST_TEAPOT), o, true);
        const File cube =
                convert(std::string(Cube) + "vt 0 0\no a\nf 1/1 4/1 3/1 2/1\nf 5 6 7 8\no b\n" + CubeFaces, defaults());
        const size_t before = FakeMemory::live();

        std::vector<uint8_t> pixels(64 * 64);
        bmds_canvas_t        canvas;
        REQUIRE(bmds_canvas_init(&canvas, pixels.data(), 64, 64, 64, BMDS_PIXEL_L8) == 0);
        bmds_3d_t *ctx = nullptr;
        REQUIRE(bmds_3d_create(nullptr, &ctx) == 0);
        bmds_3d_set_target(ctx, &canvas);
        bmds_mat4_t p;
        bmds_mat4_ortho(&p, -2.0f, 2.0f, -2.0f, 2.0f, -10.0f, 10.0f);
        bmds_3d_set_projection(ctx, &p);
        bmds_3d_material_t wire{}, flat{};
        wire.size = flat.size = sizeof(wire);
        wire.mode = BMDS_3D_WIRE;
        flat.mode = BMDS_3D_FLAT;
        flat.flags = BMDS_3D_UNLIT;
        wire.pixel = flat.pixel = 7;

        std::mt19937 rng(1234);
        uint32_t     loaded = 0, tries = 0;
        for (const File *source : {&cube, &teapot}) {
                for (int round = 0; round < 3000; ++round, ++tries) {
                        File f = *source;
                        // Mostly the headers and tables (where offsets and counts are), some anywhere.
                        const int changes = 1 + int(rng() % 4);
                        for (int c = 0; c < changes; ++c) {
                                const size_t span = rng() % 4 != 0 ? std::min<size_t>(f.bytes, 160) : f.bytes;
                                const size_t at = rng() % span;
                                if (rng() % 2 != 0)
                                        f.data()[at] = uint8_t(rng());
                                else if (at + 4 <= f.bytes)
                                        f.setU32(at & ~size_t(3), rng() % 3 == 0 ? rng() : rng() % 512);
                        }
                        const size_t     bytes = rng() % 8 == 0 ? rng() % (f.bytes + 1) : f.bytes;
                        bmds_3d_model_t *m = nullptr;
                        if (bmds_3d_model_load_memory(f.data(), uint32_t(bytes), &m) != 0) {
                                CHECK(m == nullptr);
                                continue;
                        }
                        ++loaded;
                        bmds_3d_begin(ctx);
                        for (uint32_t i = 0; i < bmds_3d_model_mesh_count(m); ++i) {
                                const bmds_3d_mesh_t *mesh = bmds_3d_model_mesh(m, i);
                                for (uint32_t k = 0; k < 3 * mesh->triangle_count; ++k)
                                        REQUIRE(mesh->triangles[k] < mesh->vertex_count);
                                for (uint32_t k = 0; k < 2 * mesh->edge_count; ++k)
                                        REQUIRE(mesh->edges[k] < mesh->vertex_count);
                                (void)bmds_3d_draw(ctx, mesh, &wire);
                                (void)bmds_3d_draw(ctx, mesh, &flat);
                                const char *n = bmds_3d_model_mesh_name(m, i);
                                if (n != nullptr) (void)std::strlen(n);
                        }
                        bmds_3d_end(ctx);
                        bmds_3d_model_free(m);
                }
        }
        bmds_3d_destroy(ctx);
        CHECK(loaded > 0); // changes to the arrays' contents are fine
        CHECK(FakeMemory::live() == before);
}
