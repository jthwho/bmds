/**
 * @file      palette.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's palettes (bmds/palette.h, devplan/3d.md §8): ramps, mixes, rotation, HSV.
 */

#include <doctest.h>

#include <cstdint>
#include <vector>
#include <bmds/palette.h>

TEST_CASE("palette: ramps end on their colours, evenly between") {
        std::vector<uint32_t> p(256, 0xdeadbe);
        bmds_palette_ramp(p.data(), 16, 5, 0x000000, 0xff8040);
        CHECK(p[15] == 0xdeadbe);
        CHECK(p[16] == 0x000000);
        CHECK(p[18] == 0x804020);
        CHECK(p[20] == 0xff8040);
        CHECK(p[21] == 0xdeadbe);
        bmds_palette_ramp(p.data(), 250, 20, 0x000000, 0xffffff); // cut at the palette's end
        CHECK(p[250] == 0);
        CHECK(p[255] == 0xffffff);
        bmds_palette_ramp(p.data(), 0, 1, 0x111111, 0x222222);
        CHECK(p[0] == 0x222222);
        bmds_palette_ramp3(p.data(), 32, 9, 0x000000, 0xff0000, 0xffffff);
        CHECK(p[32] == 0x000000);
        CHECK(p[36] == 0xff0000);
        CHECK(p[40] == 0xffffff);
        CHECK(p[34] == 0x800000);
}

TEST_CASE("palette: mixes, rotation and HSV") {
        const uint32_t a[3] = {0x000000, 0xffffff, 0x204060}, b[3] = {0xffffff, 0x000000, 0x204060};
        uint32_t       out[3];
        bmds_palette_mix(out, a, b, 3, 0);
        CHECK(out[0] == 0x000000);
        bmds_palette_mix(out, a, b, 3, 256);
        CHECK(out[0] == 0xffffff);
        CHECK(out[1] == 0x000000);
        bmds_palette_mix(out, a, b, 3, 128);
        CHECK(out[0] == 0x808080);
        CHECK(out[1] == 0x7f7f7f);
        CHECK(out[2] == 0x204060);
        bmds_palette_mix_rgb(out, a, 3, 0xff0000, 256);
        CHECK(out[1] == 0xff0000);

        uint32_t p[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        bmds_palette_rotate(p, 2, 4, 1);
        const uint32_t want[8] = {0, 1, 5, 2, 3, 4, 6, 7};
        for (int i = 0; i < 8; ++i) CHECK(p[i] == want[i]);
        bmds_palette_rotate(p, 2, 4, -1);
        for (int i = 0; i < 8; ++i) CHECK(p[i] == uint32_t(i));
        bmds_palette_rotate(p, 0, 8, 11); // 11 is 3
        CHECK(p[3] == 0);
        CHECK(p[0] == 5);

        CHECK(bmds_palette_hsv(0.0f, 1.0f, 1.0f) == 0xff0000);
        CHECK(bmds_palette_hsv(1.0f / 3.0f, 1.0f, 1.0f) == 0x00ff00);
        CHECK(bmds_palette_hsv(2.0f / 3.0f, 1.0f, 1.0f) == 0x0000ff);
        CHECK(bmds_palette_hsv(1.5f, 0.0f, 0.5f) == 0x808080);
}
