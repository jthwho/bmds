/**
 * @file      pixelrepeat.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pixels' row expanders (devplan/games.md §10, §22): whole factors across, any ratio, the palette's
 * lookup.
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

TEST_CASE("Pixels::repeat8 writes each index the factor's times, at any alignment") {
        uint8_t in[37];
        for (size_t i = 0; i < sizeof(in); ++i) in[i] = uint8_t(i * 29 + 3);
        for (unsigned times = 1; times <= 4; ++times) {
                for (size_t offset = 0; offset < 4; ++offset) {
                        for (size_t count = 0; count <= sizeof(in); ++count) {
                                std::vector<uint8_t> out(offset + count * times + 1, 0x5a);
                                Pixels::repeat8(in, count, out.data() + offset, times, Simd::Kernel::Scalar);
                                for (size_t i = 0; i < count * times; ++i) CHECK(out[offset + i] == in[i / times]);
                                CHECK(out[offset + count * times] == 0x5a); // nothing past the end
                                for (size_t i = 0; i < offset; ++i) CHECK(out[i] == 0x5a);
                        }
                }
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

TEST_CASE("Pixels::expandIndexed's word paths match a byte at a time") {
        std::vector<uint32_t> palette(256);
        for (uint32_t i = 0; i < 256; ++i) palette[i] = 0xff000000u | (i * 0x9e3779b1u >> 8); // the top byte ignored
        uint8_t in[13];
        for (size_t i = 0; i < sizeof(in); ++i) in[i] = uint8_t(i * 37 + 11);
        for (unsigned times = 1; times <= 8; ++times) {
                for (size_t count = 0; count <= sizeof(in); ++count) {
                        for (size_t shift = 0; shift < 4; ++shift) { // the output's alignment
                                std::vector<uint8_t> buf(shift + 3 * times * count + 1, 0x5a);
                                Pixels::expandIndexed(in, count, palette.data(), buf.data() + shift, times);
                                bool same = true;
                                for (size_t i = 0; i < count * times; ++i) {
                                        const uint32_t c = palette[in[i / times]];
                                        const uint8_t *p = buf.data() + shift + 3 * i;
                                        same = same && p[0] == uint8_t(c) && p[1] == uint8_t(c >> 8) &&
                                               p[2] == uint8_t(c >> 16);
                                }
                                CHECK(same);
                                CHECK(buf[shift + 3 * times * count] == 0x5a); // nothing past the end
                                for (size_t i = 0; i < shift; ++i) CHECK(buf[i] == 0x5a);
                        }
                }
        }
}

TEST_CASE("Pixels::expandTriple matches expandIndexed at 3 times") {
        std::vector<uint32_t> palette(256), table(Pixels::TripleTableWords);
        for (uint32_t i = 0; i < 256; ++i) palette[i] = 0xff000000u | (i * 0x9e3779b1u >> 8); // the top byte ignored
        Pixels::makeTripleTable(palette.data(), table.data());
        uint8_t in[23];
        for (size_t i = 0; i < sizeof(in); ++i) in[i] = uint8_t(i * 37 + 11);
        for (size_t count = 0; count <= sizeof(in); ++count) {
                for (size_t shift = 0; shift < 4; ++shift) { // the output's alignment
                        std::vector<uint8_t> want(shift + 9 * count + 1, 0x5a), got(want);
                        Pixels::expandIndexed(in, count, palette.data(), want.data() + shift, 3);
                        Pixels::expandTriple(in, count, table.data(), got.data() + shift);
                        CHECK(got == want);
                }
        }
}

TEST_CASE("Pixels::stretch scales a row across by any ratio, the nearest pixel") {
        uint16_t in[64];
        for (uint16_t i = 0; i < 64; ++i) in[i] = uint16_t(0x100 + i);
        // 1.5 times (640 to 960): output column X shows input X × 2 / 3, from any first column.
        for (uint32_t first : {0u, 4u, 12u, 30u}) {
                std::vector<uint16_t> out(33, 0x5a5a);
                Pixels::stretch(reinterpret_cast<const uint8_t *>(in), 2, nullptr, first, 32, 640, 960,
                                reinterpret_cast<uint8_t *>(out.data()));
                for (uint32_t i = 0; i < 32; ++i) CHECK(out[i] == in[(first + i) * 2 / 3]);
                CHECK(out[32] == 0x5a5a);
        }
        // Down (5 to 3) and 3-byte pixels.
        uint8_t rgb[3 * 10];
        for (uint8_t i = 0; i < 30; ++i) rgb[i] = i;
        std::vector<uint8_t> out(3 * 6, 0);
        Pixels::stretch(rgb, 3, nullptr, 0, 6, 5, 3, out.data());
        for (uint32_t i = 0; i < 6; ++i)
                for (uint32_t b = 0; b < 3; ++b) CHECK(out[i * 3 + b] == rgb[(i * 5 / 3) * 3 + b]);
        // Indices through the palette as B, G, R.
        std::vector<uint32_t> palette(256, 0);
        palette[1] = 0x112233;
        palette[2] = 0x445566;
        const uint8_t        idx[2] = {1, 2};
        std::vector<uint8_t> bgr(3 * 3, 0);
        Pixels::stretch(idx, 1, palette.data(), 0, 3, 2, 3, bgr.data());
        const uint8_t want[9] = {0x33, 0x22, 0x11, 0x33, 0x22, 0x11, 0x66, 0x55, 0x44};
        for (size_t i = 0; i < 9; ++i) CHECK(bgr[i] == want[i]);
}
