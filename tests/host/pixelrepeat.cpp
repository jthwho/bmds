/**
 * @file      pixelrepeat.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pixels' row expanders (devplan/games.md §10): whole factors across, the palette's lookup.
 */

#include <doctest.h>

#include <cstdint>
#include <vector>

#include <bmds/image.h>

using namespace bmds;

TEST_CASE("Pixels::repeat16 writes each pixel the factor's times") {
        const uint16_t in[5] = {0x1234, 0xffff, 0x0000, 0xabcd, 0x0001};
        for (unsigned times = 1; times <= 4; ++times) {
                std::vector<uint16_t> out(5 * times + 1, 0x5a5a);
                Pixels::repeat16(in, 5, out.data(), times, Simd::Kernel::Scalar);
                for (size_t i = 0; i < 5 * times; ++i) CHECK(out[i] == in[i / times]);
                CHECK(out[5 * times] == 0x5a5a); // nothing past the end
        }
}

TEST_CASE("Pixels::repeat24 writes each 3-byte pixel the factor's times") {
        const uint8_t        in[6] = {1, 2, 3, 4, 5, 6};
        std::vector<uint8_t> out(3 * 2 * 3 + 1, 0x5a);
        Pixels::repeat24(in, 2, out.data(), 3);
        const uint8_t want[18] = {1, 2, 3, 1, 2, 3, 1, 2, 3, 4, 5, 6, 4, 5, 6, 4, 5, 6};
        for (size_t i = 0; i < 18; ++i) CHECK(out[i] == want[i]);
        CHECK(out[18] == 0x5a);
}

TEST_CASE("Pixels::expandIndexed looks each index up as B, G, R") {
        std::vector<uint32_t> palette(256, 0);
        palette[0] = 0x112233;
        palette[7] = 0xa0b0c0;
        palette[255] = 0xffffff;
        const uint8_t        in[3] = {7, 0, 255};
        std::vector<uint8_t> out(3 * 3 * 2 + 1, 0x5a);
        Pixels::expandIndexed(in, 3, palette.data(), out.data(), 2);
        const uint8_t want[18] = {0xc0, 0xb0, 0xa0, 0xc0, 0xb0, 0xa0, 0x33, 0x22, 0x11,
                                  0x33, 0x22, 0x11, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        for (size_t i = 0; i < 18; ++i) CHECK(out[i] == want[i]);
        CHECK(out[18] == 0x5a);
}
