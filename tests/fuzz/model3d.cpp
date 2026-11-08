/**
 * @file      model3d.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * libFuzzer target for libbmds3d's models (sdk/3d/model.c, devplan/3d.md §12d), which load the
 * `.bm3d` files programs are given: any bytes as a model; if loaded, every index inside its mesh,
 * every name terminated, and every mesh drawn wire, flat, Gouraud and environment-mapped; no
 * fault, no read outside the file, no undefined behaviour, no leak. Seed it with
 * make-model-seeds.sh.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <bmds/3d.h>

#include "fakememory.h"

namespace {

        struct Table {
                        uint32_t      magic, abi_version, count, reserved;
                        bmds_sys_fn_t slot[BMDS_SYS_MEM_FREE + 1];
        };

        Table makeTable() {
                Table t = {BMDS_SYS_MAGIC, BMDS_ABI_VERSION, BMDS_SYS_MEM_FREE + 1, 0, {}};
                FakeMemory::install(t.slot);
                return t;
        }

        Table table = makeTable();

        // A small L8 picture, a context drawing on it, and the four materials.
        struct Stage {
                        uint8_t            pixels[96 * 64] = {};
                        uint8_t            texels[16 * 16];
                        uint8_t            light[BMDS_3D_LIGHT_TABLE_SIZE];
                        bmds_canvas_t      canvas{};
                        bmds_game_image_t  texture{};
                        bmds_3d_t         *ctx = nullptr;
                        bmds_3d_material_t materials[4]{};

                        Stage() {
                                bmds_canvas_init(&canvas, pixels, 96, 96, 64, BMDS_PIXEL_L8);
                                if (bmds_3d_create(nullptr, &ctx) != 0) std::abort();
                                bmds_3d_set_target(ctx, &canvas);
                                bmds_mat4_t m;
                                bmds_mat4_perspective(&m, 1.0f, 1.5f, 0.1f, 100.0f);
                                bmds_3d_set_projection(ctx, &m);
                                uint32_t palette[256];
                                for (uint32_t i = 0; i < 256; ++i) palette[i] = i * 0x010101u;
                                bmds_3d_light_table(light, palette, 0, 256, 0);
                                for (uint32_t i = 0; i < sizeof(texels); ++i) texels[i] = uint8_t(i * 7);
                                texture = {texels, 16, 16, 16, BMDS_PIXEL_L8, 0, 0};
                                const uint32_t modes[4] = {BMDS_3D_WIRE, BMDS_3D_FLAT, BMDS_3D_GOURAUD, BMDS_3D_ENVMAP};
                                for (int i = 0; i < 4; ++i) {
                                        bmds_3d_material_t &mt = materials[i];
                                        mt.size = sizeof(mt);
                                        mt.mode = modes[i];
                                        mt.pixel = 9;
                                        mt.ramp_first = 16;
                                        mt.ramp_count = 32;
                                        mt.texture = &texture;
                                        mt.light_table = light;
                                }
                        }
        };

} // namespace

const bmds_sys_t *bmds_sys = reinterpret_cast<const bmds_sys_t *>(&table);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        static Stage stage;
        if (size > 1u << 20) return 0;
        std::vector<uint32_t> words((size + 3) / 4); // the loader needs 4-aligned bytes
        if (size != 0) std::memcpy(words.data(), data, size);
        const size_t     before = FakeMemory::live();
        bmds_3d_model_t *model = nullptr;
        if (bmds_3d_model_load_memory(words.data(), uint32_t(size), &model) != 0) {
                if (model != nullptr || FakeMemory::live() != before) std::abort();
                return 0;
        }
        const uint32_t count = bmds_3d_model_mesh_count(model);
        for (uint32_t i = 0; i < count; ++i) {
                const bmds_3d_mesh_t *m = bmds_3d_model_mesh(model, i);
                for (uint32_t k = 0; k < 3 * m->triangle_count; ++k)
                        if (m->triangles[k] >= m->vertex_count) std::abort();
                for (uint32_t k = 0; k < 2 * m->edge_count; ++k)
                        if (m->edges[k] >= m->vertex_count) std::abort();
                const char *name = bmds_3d_model_mesh_name(model, i);
                if (name != nullptr && bmds_3d_model_find(model, name) < 0) std::abort();
        }
        const size_t drawing = FakeMemory::live();
        bmds_3d_begin(stage.ctx);
        bmds_mat4_t identity;
        bmds_mat4_identity(&identity);
        bmds_3d_load(stage.ctx, &identity);
        bmds_3d_translate(stage.ctx, 0.0f, 0.0f, -4.0f);
        for (uint32_t i = 0; i < count; ++i)
                for (const bmds_3d_material_t &mt : stage.materials)
                        (void)bmds_3d_draw(stage.ctx, bmds_3d_model_mesh(model, i), &mt);
        bmds_3d_end(stage.ctx);
        /* The context keeps what it grew while drawing (ENVMAP's coordinates): the model's own
           memory is what must all come back. */
        const size_t grown = FakeMemory::live() - drawing;
        bmds_3d_model_free(model);
        if (FakeMemory::live() != before + grown) std::abort();
        return 0;
}
