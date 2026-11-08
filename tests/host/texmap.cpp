/**
 * @file      texmap.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's texture maps (sdk/game/texmap.c, devplan/3d.md §8): a canvas as an image, the
 * affine map (identity, turned, wrapped, clipped, every format and alignment) and the lookup-table
 * map, against what a pixel at a time gives.
 */

#include <doctest.h>

#include <cmath>
#include <cstring>
#include <vector>
#include <bmds/game.h>

namespace {

        constexpr float Pi = 3.14159265f;

        struct Picture {
                        std::vector<uint8_t> pixels;
                        bmds_canvas_t        canvas{};
                        uint32_t             bpp;

                        Picture(int32_t w, int32_t h, uint32_t format, uint8_t fill = 0) {
                                bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
                                pixels.assign(size_t(w) * h * bpp, fill);
                                REQUIRE(bmds_canvas_init(&canvas, pixels.data(), uint32_t(w) * bpp, w, h, format) == 0);
                        }

                        uint32_t at(int32_t x, int32_t y) const {
                                const uint8_t *p = pixels.data() + size_t(y) * canvas.stride + size_t(x) * bpp;
                                uint32_t       v = 0;
                                for (uint32_t i = 0; i < bpp; ++i) v |= uint32_t(p[i]) << (8 * i);
                                return v;
                        }
        };

        // An image of w × h in @p format, each pixel's value telling where it is.
        struct Image {
                        std::vector<uint8_t> pixels;
                        bmds_game_image_t    image{};
                        uint32_t             bpp;

                        Image(int32_t w, int32_t h, uint32_t format, uint32_t stride = 0) {
                                bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
                                if (stride == 0) stride = uint32_t(w) * bpp;
                                pixels.assign(size_t(stride) * h, 0);
                                for (int32_t y = 0; y < h; ++y)
                                        for (int32_t x = 0; x < w; ++x) {
                                                const uint32_t v = value(x, y);
                                                for (uint32_t i = 0; i < bpp; ++i)
                                                        pixels[size_t(y) * stride + size_t(x) * bpp + i] =
                                                                uint8_t(v >> (8 * i));
                                        }
                                image.pixels = pixels.data();
                                image.stride = stride;
                                image.width = w;
                                image.height = h;
                                image.format = format;
                        }

                        uint32_t value(int32_t x, int32_t y) const {
                                const uint32_t v = uint32_t(x * 7 + y * 31 + 1);
                                return bpp == 1   ? (v & 0xff)
                                       : bpp == 2 ? (v * 0x9e37u) & 0xffff
                                                  : (v * 0x9e3779u) & 0xffffff;
                        }

                        uint32_t wrapped(int64_t x, int64_t y) const {
                                return value(int32_t(x & (image.width - 1)), int32_t(y & (image.height - 1)));
                        }
        };

} // namespace

TEST_CASE("Game texture maps: a canvas as an image, all of it or a part") {
        Picture pic(40, 30, BMDS_PIXEL_RGB565);
        for (size_t i = 0; i < pic.pixels.size(); ++i) pic.pixels[i] = uint8_t(i * 13);
        bmds_game_image_t image;
        REQUIRE(bmds_canvas_image(&pic.canvas, nullptr, &image) == 0);
        CHECK(image.pixels == pic.pixels.data());
        CHECK(image.width == 40);
        CHECK(image.height == 30);
        CHECK(image.stride == 80);
        CHECK(image.format == uint32_t(BMDS_PIXEL_RGB565));
        CHECK(image.flags == 0);
        // A part, cut to the canvas, blitted back out: the same pixels.
        const bmds_rect_t part = {30, -5, 20, 15};
        REQUIRE(bmds_canvas_image(&pic.canvas, &part, &image) == 0);
        CHECK(image.width == 10);
        CHECK(image.height == 10);
        Picture out(10, 10, BMDS_PIXEL_RGB565);
        bmds_canvas_blit(&out.canvas, 0, 0, &image, nullptr);
        for (int32_t y = 0; y < 10; ++y)
                for (int32_t x = 0; x < 10; ++x) CHECK(out.at(x, y) == pic.at(30 + x, y));
        const bmds_rect_t outside = {50, 0, 10, 10};
        CHECK(bmds_canvas_image(&pic.canvas, &outside, &image) == -BMDS_EINVAL);
        CHECK(bmds_canvas_image(nullptr, nullptr, &image) == -BMDS_EINVAL);
}

TEST_CASE("Game texture maps: affine, as a pixel at a time gives, every format, wrapped and clipped") {
        for (uint32_t format : {uint32_t(BMDS_PIXEL_L8), uint32_t(BMDS_PIXEL_RGB565), uint32_t(BMDS_PIXEL_BGR24)}) {
                for (uint32_t stride : {0u, 1u}) { // tight (a power of two), or wider and not
                        CAPTURE(format);
                        CAPTURE(stride);
                        const uint32_t bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
                        Image          img(16, 8, format, stride != 0 ? 16 * bpp + 6 : 0);
                        struct Map {
                                        int32_t u, v, dux, dvx, duy, dvy;
                        };
                        // The identity, a shear with negative steps, a quarter turn, a zoom.
                        for (const Map k :
                             {Map{0, 0, 65536, 0, 0, 65536}, Map{-5 << 16, 3 << 15, -65536, 20000, 30000, -65536},
                              Map{2 << 16, 9 << 16, 0, -65536, 65536, 0}, Map{1 << 15, 1 << 15, 16384, 0, 0, 16384}}) {
                                Picture           pic(37, 23, format, 0xee);
                                const bmds_rect_t clip = {1, 2, 33, 19};
                                bmds_canvas_set_clip(&pic.canvas, &clip);
                                const bmds_game_affine_t map = {k.u, k.v, k.dux, k.dvx, k.duy, k.dvy};
                                REQUIRE(bmds_canvas_texture_affine(&pic.canvas, -3, 0, 40, 30, &img.image, &map) == 0);
                                for (int32_t y = 0; y < 23; ++y)
                                        for (int32_t x = 0; x < 37; ++x) {
                                                CAPTURE(x);
                                                CAPTURE(y);
                                                const bool     in = x >= 1 && x < 34 && y >= 2 && y < 21;
                                                const int64_t  i = x + 3, j = y;
                                                const int64_t  u = int64_t(k.u) + i * k.dux + j * k.duy;
                                                const int64_t  v = int64_t(k.v) + i * k.dvx + j * k.dvy;
                                                const uint32_t want = in ? img.wrapped(u >> 16, v >> 16)
                                                                         : (bpp == 1   ? 0xeeu
                                                                            : bpp == 2 ? 0xeeeeu
                                                                                       : 0xeeeeeeu);
                                                CHECK(pic.at(x, y) == want);
                                        }
                                CHECK(pic.canvas.damage_count == 1);
                                CHECK(pic.canvas.damage[0].x == 1);
                                CHECK(pic.canvas.damage[0].width == 33u);
                        }
                }
        }
}

TEST_CASE("Game texture maps: a rotozoom's quarter turn lands on whole texels") {
        Image              img(16, 16, BMDS_PIXEL_L8);
        Picture            pic(24, 20, BMDS_PIXEL_L8);
        bmds_game_affine_t map;
        // Pixel (8, 8)'s centre shows texel (4.5, 4.5); turned a quarter clockwise, a pixel a texel.
        bmds_game_affine_rotozoom(&map, 0, 0, 8.5f, 8.5f, 4.5f, 4.5f, Pi / 2.0f, 1.0f);
        CHECK(map.du_dx == 0);
        CHECK(map.dv_dx == -65536);
        CHECK(map.du_dy == 65536);
        CHECK(map.dv_dy == 0);
        REQUIRE(bmds_canvas_texture_affine(&pic.canvas, 0, 0, 24, 20, &img.image, &map) == 0);
        for (int32_t y = 0; y < 20; ++y)
                for (int32_t x = 0; x < 24; ++x) {
                        // Right on the screen is up the texture; down is right.
                        CHECK(pic.at(x, y) == img.wrapped(4 + (y - 8), 4 - (x - 8)));
                }
        // Zoomed twice: two pixels a texel.
        bmds_game_affine_rotozoom(&map, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f);
        REQUIRE(bmds_canvas_texture_affine(&pic.canvas, 0, 0, 24, 20, &img.image, &map) == 0);
        for (int32_t y = 0; y < 20; ++y)
                for (int32_t x = 0; x < 24; ++x) CHECK(pic.at(x, y) == img.wrapped(x / 2, y / 2));
        // Refused: formats that differ, a size not a power of two.
        Image odd(12, 16, BMDS_PIXEL_L8);
        CHECK(bmds_canvas_texture_affine(&pic.canvas, 0, 0, 4, 4, &odd.image, &map) == -BMDS_EINVAL);
        Image rgb(16, 16, BMDS_PIXEL_RGB565);
        CHECK(bmds_canvas_texture_affine(&pic.canvas, 0, 0, 4, 4, &rgb.image, &map) == -BMDS_EINVAL);
}

TEST_CASE("Game texture maps: a lookup table, moved and wrapped, clipped") {
        for (uint32_t format : {uint32_t(BMDS_PIXEL_L8), uint32_t(BMDS_PIXEL_RGB565), uint32_t(BMDS_PIXEL_BGR24)}) {
                CAPTURE(format);
                const uint32_t        bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
                Image                 img(32, 16, format);
                const int32_t         w = 30, h = 12, ts = 31;
                std::vector<uint16_t> table(size_t(ts) * h);
                for (int32_t y = 0; y < h; ++y)
                        for (int32_t x = 0; x < w; ++x)
                                table[size_t(y) * ts + x] = uint16_t(((x * 5 + y) & 0xff) | ((x ^ y * 3) & 0xff) << 8);
                Picture           pic(32, 16, format, 0x11);
                const bmds_rect_t clip = {3, 1, 26, 14};
                bmds_canvas_set_clip(&pic.canvas, &clip);
                REQUIRE(bmds_canvas_remap(&pic.canvas, 1, 2, w, h, &img.image, table.data(), ts, 250, 7) == 0);
                for (int32_t y = 0; y < 16; ++y)
                        for (int32_t x = 0; x < 32; ++x) {
                                CAPTURE(x);
                                CAPTURE(y);
                                const bool in = x >= 3 && x < 29 && y >= 2 && y < 14;
                                uint32_t   want = bpp == 1 ? 0x11u : bpp == 2 ? 0x1111u : 0x111111u;
                                if (in) {
                                        const uint16_t t = table[size_t(y - 2) * ts + (x - 1)];
                                        want = img.wrapped((t & 0xff) + 250, (t >> 8) + 7);
                                }
                                CHECK(pic.at(x, y) == want);
                        }
        }
        // L8 two entries at a time: every byte value, moves past the top bit, rows starting on
        // either half of a word.
        {
                Image                 img(64, 32, BMDS_PIXEL_L8);
                const int32_t         w = 37, h = 9, ts = 39;
                std::vector<uint16_t> table(size_t(ts) * h);
                uint32_t              x = 12345;
                for (uint16_t &t : table) {
                        x = x * 1664525u + 1013904223u;
                        t = uint16_t(x >> 16);
                }
                Picture pic(48, 12, BMDS_PIXEL_L8, 0x11);
                REQUIRE(bmds_canvas_remap(&pic.canvas, 3, 1, w, h, &img.image, table.data(), ts, 0x9c, 0xe1) == 0);
                for (int32_t y = 0; y < h; ++y)
                        for (int32_t c = 0; c < w; ++c) {
                                CAPTURE(c);
                                CAPTURE(y);
                                const uint16_t t = table[size_t(y) * ts + c];
                                CHECK(pic.at(3 + c, 1 + y) == img.wrapped((t & 0xff) + 0x9c, (t >> 8) + 0xe1));
                        }
        }
        Picture  pic(8, 8, BMDS_PIXEL_L8);
        Image    big(512, 4, BMDS_PIXEL_L8);
        uint16_t one = 0;
        CHECK(bmds_canvas_remap(&pic.canvas, 0, 0, 1, 1, &big.image, &one, 1, 0, 0) == -BMDS_EINVAL);
}
