/**
 * @file      game.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's canvas, text and image conversion (sdk/game, devplan/games.md §13): clipped
 * drawing in each format, its damage, blits with a key, glyphs, decoded pixels into a canvas's.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <bmds/game.h>

extern "C" {
#include "../../sdk/game/game_internal.h"
}

namespace {

        // A canvas over its own pixels.
        struct Picture {
                        std::vector<uint8_t> pixels;
                        bmds_canvas_t        canvas{};

                        Picture(int32_t w, int32_t h, uint32_t format) {
                                const uint32_t bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
                                pixels.assign(size_t(w) * h * bpp, 0);
                                REQUIRE(bmds_canvas_init(&canvas, pixels.data(), uint32_t(w) * bpp, w, h, format) == 0);
                        }

                        static uint32_t bgr(const uint8_t *p, int32_t x) {
                                return uint32_t(p[3 * x] | p[3 * x + 1] << 8 | p[3 * x + 2] << 16);
                        }

                        uint32_t at(int32_t x, int32_t y) const {
                                const uint8_t *p = canvas.pixels + size_t(y) * canvas.stride;
                                if (canvas.format == BMDS_PIXEL_L8) return p[x];
                                if (canvas.format == BMDS_PIXEL_RGB565) {
                                        uint16_t v;
                                        std::memcpy(&v, p + 2 * x, 2);
                                        return v;
                                }
                                return uint32_t(p[3 * x] | p[3 * x + 1] << 8 | p[3 * x + 2] << 16);
                        }

                        // The rows as text: '#' where the pixel isn't 0.
                        std::string rows() const {
                                std::string out;
                                for (int32_t y = 0; y < canvas.height; ++y) {
                                        for (int32_t x = 0; x < canvas.width; ++x) out += at(x, y) != 0 ? '#' : '.';
                                        out += '\n';
                                }
                                return out;
                        }

                        std::string damage() const {
                                std::string out;
                                for (uint32_t i = 0; i < canvas.damage_count; ++i) {
                                        const bmds_rect_t &r = canvas.damage[i];
                                        out += std::to_string(r.x) + "," + std::to_string(r.y) + " " +
                                               std::to_string(r.width) + "x" + std::to_string(r.height) + ";";
                                }
                                return out;
                        }
        };

} // namespace

TEST_CASE("Game canvas: formats, pixels, refused sizes") {
        Picture p(4, 2, BMDS_PIXEL_RGB565);
        CHECK(bmds_canvas_pixel(&p.canvas, 0xff0000) == 0xf800);
        CHECK(bmds_canvas_pixel(&p.canvas, 0x00ff00) == 0x07e0);
        CHECK(bmds_canvas_pixel(&p.canvas, 0x0000ff) == 0x001f);
        Picture q(4, 2, BMDS_PIXEL_BGR24);
        CHECK(bmds_canvas_pixel(&q.canvas, 0x123456) == 0x123456);
        bmds_canvas_fill(&q.canvas, 1, 0, 1, 1, 0x123456);
        CHECK(q.pixels[3] == 0x56); // B, G, R
        CHECK(q.pixels[5] == 0x12);
        Picture l(4, 2, BMDS_PIXEL_L8);
        CHECK(bmds_canvas_pixel(&l.canvas, 0x1ab) == 0xab);
        bmds_canvas_t c;
        uint8_t       buf[16];
        CHECK(bmds_canvas_init(&c, buf, 2, 4, 2, BMDS_PIXEL_L8) == -1); // stride too small
        CHECK(bmds_canvas_init(&c, buf, 4, 4, 2, 7) == -1);             // a format it can't draw
        CHECK(bmds_canvas_init(&c, nullptr, 4, 4, 2, BMDS_PIXEL_L8) == -1);
}

TEST_CASE("Game canvas: fills clipped, their damage merged") {
        Picture p(8, 4, BMDS_PIXEL_RGB565);
        bmds_canvas_fill(&p.canvas, -2, -1, 4, 3, 1);
        CHECK(p.rows() == "##......\n##......\n........\n........\n");
        CHECK(p.damage() == "0,0 2x2;");
        // Touching: merged. Apart: kept apart.
        bmds_canvas_fill(&p.canvas, 2, 0, 1, 1, 1);
        CHECK(p.damage() == "0,0 3x2;");
        bmds_canvas_fill(&p.canvas, 6, 3, 9, 9, 1);
        CHECK(p.damage() == "0,0 3x2;6,3 2x1;");
        // The clip.
        Picture     q(8, 4, BMDS_PIXEL_L8);
        bmds_rect_t clip = {2, 1, 3, 2};
        bmds_canvas_set_clip(&q.canvas, &clip);
        bmds_canvas_clear(&q.canvas, 9);
        CHECK(q.rows() == "........\n..###...\n..###...\n........\n");
        CHECK(q.damage() == "2,1 3x2;");
        bmds_canvas_fill(&q.canvas, 0, 0, 1, 1, 9); // outside: nothing, no damage
        CHECK(q.damage() == "2,1 3x2;");
        // More than kept: their bounds.
        Picture r(64, 2, BMDS_PIXEL_L8);
        for (int32_t i = 0; i < 20; ++i) bmds_canvas_fill(&r.canvas, i * 3, 0, 1, 1, 1);
        CHECK(r.canvas.damage_count <= BMDS_CANVAS_MAX_DAMAGE);
        uint32_t covered = 0;
        for (uint32_t i = 0; i < r.canvas.damage_count; ++i) covered += r.canvas.damage[i].width;
        CHECK(covered >= 20);
        // Spread out (squares over a big picture): merged in pairs, not into one around them all.
        Picture big(640, 360, BMDS_PIXEL_L8);
        for (int32_t i = 0; i < 64; ++i) bmds_canvas_fill(&big.canvas, (i * 97) % 620, (i * 53) % 340, 16, 16, 1);
        uint64_t area = 0;
        for (uint32_t i = 0; i < big.canvas.damage_count; ++i)
                area += uint64_t(big.canvas.damage[i].width) * big.canvas.damage[i].height;
        CHECK(big.canvas.damage_count == BMDS_CANVAS_MAX_DAMAGE);
        CHECK(area < 640u * 360u / 3); // far less than the picture
        bmds_canvas_damage_all(&r.canvas);
        CHECK(r.damage() == "0,0 64x2;");
        bmds_canvas_damage_clear(&r.canvas);
        CHECK(r.damage().empty());
}

TEST_CASE("Game canvas: outlines and lines") {
        Picture p(6, 4, BMDS_PIXEL_L8);
        bmds_canvas_rect(&p.canvas, 0, 0, 6, 4, 1);
        CHECK(p.rows() == "######\n#....#\n#....#\n######\n");
        Picture q(5, 5, BMDS_PIXEL_RGB565);
        bmds_canvas_line(&q.canvas, 0, 0, 4, 4, 1);
        CHECK(q.rows() == "#....\n.#...\n..#..\n...#.\n....#\n");
        CHECK(q.damage() == "0,0 5x5;");
        Picture r(5, 3, BMDS_PIXEL_BGR24);
        bmds_canvas_line(&r.canvas, 4, 1, 0, 1, 0xffffff);
        bmds_canvas_line(&r.canvas, -3, 0, 7, 2, 0xffffff); // its ends outside: clipped
        CHECK(r.rows()[5] == '\n');
        CHECK(r.rows().substr(6, 5) == "#####");
}

TEST_CASE("Game canvas: blits, a source rectangle, the key, clipping") {
        // A 4x2 sheet: two 2x2 sprites, 0 the key.
        const uint8_t     sheet[] = {1, 0, 3, 3, 0, 1, 3, 3};
        bmds_game_image_t image = {sheet, 4, 4, 2, BMDS_PIXEL_L8, BMDS_GAME_IMAGE_KEYED, 0};
        Picture           p(4, 3, BMDS_PIXEL_L8);
        bmds_canvas_clear(&p.canvas, 7);
        bmds_canvas_damage_clear(&p.canvas);
        const bmds_rect_t first = {0, 0, 2, 2};
        bmds_canvas_blit(&p.canvas, 1, 1, &image, &first);
        CHECK(p.at(1, 1) == 1);
        CHECK(p.at(2, 1) == 7); // the key: left as it was
        CHECK(p.at(2, 2) == 1);
        CHECK(p.damage() == "1,1 2x2;");
        // Unkeyed, half off the left edge.
        image.flags = 0;
        const bmds_rect_t second = {2, 0, 2, 2};
        bmds_canvas_blit(&p.canvas, -1, 0, &image, &second);
        CHECK(p.at(0, 0) == 3);
        CHECK(p.at(1, 0) == 7);
        // Another format: nothing.
        Picture q(4, 3, BMDS_PIXEL_RGB565);
        bmds_canvas_blit(&q.canvas, 0, 0, &image, nullptr);
        CHECK(q.damage().empty());
}

TEST_CASE("Game canvas: scaled blits, mirrored, keyed, clipped") {
        // A 2x2 sprite: 1 2 / 3 0, 0 the key.
        const uint8_t     sheet[] = {1, 2, 3, 0};
        bmds_game_image_t image = {sheet, 2, 2, 2, BMDS_PIXEL_L8, BMDS_GAME_IMAGE_KEYED, 0};
        Picture           p(4, 4, BMDS_PIXEL_L8);
        bmds_canvas_clear(&p.canvas, 7);
        bmds_canvas_damage_clear(&p.canvas);
        bmds_canvas_blit_scaled(&p.canvas, 0, 0, 4, 4, &image, nullptr);
        CHECK(p.at(0, 0) == 1);
        CHECK(p.at(1, 1) == 1);
        CHECK(p.at(2, 0) == 2);
        CHECK(p.at(3, 1) == 2);
        CHECK(p.at(0, 3) == 3);
        CHECK(p.at(3, 3) == 7); // the key
        CHECK(p.damage() == "0,0 4x4;");
        // Mirrored across, squeezed to a column of 1: the source's middle (its right column).
        bmds_canvas_clear(&p.canvas, 7);
        bmds_canvas_blit_scaled(&p.canvas, 0, 0, -2, 2, &image, nullptr);
        CHECK(p.at(0, 0) == 2);
        CHECK(p.at(1, 0) == 1);
        CHECK(p.at(1, 1) == 3);
        CHECK(p.at(0, 1) == 7);
        bmds_canvas_clear(&p.canvas, 7);
        bmds_canvas_blit_scaled(&p.canvas, 1, 0, 1, 2, &image, nullptr);
        CHECK(p.at(1, 0) == 2);
        // Off the right edge: clipped, the damage too.
        bmds_canvas_damage_clear(&p.canvas);
        bmds_canvas_blit_scaled(&p.canvas, 3, 0, 4, 4, &image, nullptr);
        CHECK(p.at(3, 0) == 1);
        CHECK(p.damage() == "3,0 1x4;");
        // Nothing for a size of 0, or another format.
        Picture q(4, 4, BMDS_PIXEL_RGB565);
        bmds_canvas_blit_scaled(&q.canvas, 0, 0, 4, 4, &image, nullptr);
        bmds_canvas_blit_scaled(&p.canvas, 0, 0, 0, 4, &image, nullptr);
        CHECK(q.damage().empty());
}

namespace {

        // The turned blit as first written (a pixel at a time over its bounds, 64-bit steps): the
        // faster one must draw exactly the same.
        int32_t ref_sine16(uint32_t turn) {
                turn &= BMDS_GAME_TURN - 1;
                const uint32_t quarter = BMDS_GAME_TURN / 4;
                const int      negative = turn >= BMDS_GAME_TURN / 2;
                uint32_t       t = turn % (BMDS_GAME_TURN / 2);
                if (t > quarter) t = BMDS_GAME_TURN / 2 - t;
                const double u = double(t) / quarter, u2 = u * u;
                const double v =
                        u * (1.5707963267948966 +
                             u2 * (-0.6459640975062462 +
                                   u2 * (0.0796926262461670 + u2 * (-0.0046817541353187 + u2 * 0.0001604411847874))));
                const int32_t r = int32_t(v * 65536.0 + 0.5);
                return negative ? -r : r;
        }

        void ref_rotated(Picture &p, int32_t cx, int32_t cy, uint32_t turn, uint32_t scale,
                         const bmds_game_image_t &image, const bmds_rect_t &src, const bmds_rect_t &clip) {
                const int32_t sw = int32_t(src.width), sh = int32_t(src.height);
                const int32_t sn = ref_sine16(turn), cs = ref_sine16(turn + BMDS_GAME_TURN / 4);
                const int64_t asn = sn < 0 ? -sn : sn, acs = cs < 0 ? -cs : cs;
                const int32_t ex = int32_t(((acs * sw + asn * sh) * scale / BMDS_GAME_SCALE) >> 17) + 1;
                const int32_t ey = int32_t(((asn * sw + acs * sh) * scale / BMDS_GAME_SCALE) >> 17) + 1;
                const int32_t x0 = std::max(cx - ex, clip.x), y0 = std::max(cy - ey, clip.y);
                const int32_t x1 = std::min(cx + ex + 1, clip.x + int32_t(clip.width));
                const int32_t y1 = std::min(cy + ey + 1, clip.y + int32_t(clip.height));
                const int64_t ux = int64_t(cs) * BMDS_GAME_SCALE / scale, vx = -int64_t(sn) * BMDS_GAME_SCALE / scale;
                const int64_t uy = int64_t(sn) * BMDS_GAME_SCALE / scale, vy = int64_t(cs) * BMDS_GAME_SCALE / scale;
                const int64_t half_w = int64_t(sw) << 15, half_h = int64_t(sh) << 15;
                for (int32_t row = y0; row < y1; ++row) {
                        const int64_t dy2 = 2 * int64_t(row - cy) + 1, dx2 = 2 * int64_t(x0 - cx) + 1;
                        int64_t       u = ((dx2 * ux + dy2 * uy) >> 1) + half_w;
                        int64_t       v = ((dx2 * vx + dy2 * vy) >> 1) + half_h;
                        for (int32_t col = x0; col < x1; ++col, u += ux, v += vx) {
                                if (u < 0 || v < 0) continue;
                                const int32_t sx = int32_t(u >> 16), sy = int32_t(v >> 16);
                                if (sx >= sw || sy >= sh) continue;
                                uint16_t px;
                                std::memcpy(&px,
                                            image.pixels + size_t(src.y + sy) * image.stride + size_t(src.x + sx) * 2,
                                            2);
                                if (px == uint16_t(image.key)) continue;
                                std::memcpy(p.pixels.data() + size_t(row) * p.canvas.stride + size_t(col) * 2, &px, 2);
                        }
                }
        }

} // namespace

TEST_CASE("Game canvas: turned blits draw exactly what a pixel at a time would") {
        // A keyed RGB565 sheet of distinct pixels, a tile of it turned at many angles and sizes.
        const int32_t         sw = 37, sh = 29;
        std::vector<uint16_t> sheet(size_t(sw) * sh);
        for (size_t i = 0; i < sheet.size(); ++i) sheet[i] = uint16_t(i * 2654435761u >> 7) | 1u;
        sheet[5] = sheet[100] = sheet[300] = 0; // keyed pixels
        bmds_game_image_t image = {reinterpret_cast<const uint8_t *>(sheet.data()),
                                   uint32_t(sw) * 2,
                                   sw,
                                   sh,
                                   BMDS_PIXEL_RGB565,
                                   BMDS_GAME_IMAGE_KEYED,
                                   0};
        const bmds_rect_t src = {3, 2, 31, 23};
        uint32_t          seed = 12345;
        auto              next = [&seed] {
                return seed = seed * 1103515245u + 12345u, seed >> 8;
        };
        size_t drawn = 0;
        for (int round = 0; round < 400; ++round) {
                const uint32_t    turn = next() % BMDS_GAME_TURN, scale = 40 + next() % 600;
                const int32_t     cx = int32_t(next() % 80) - 10, cy = int32_t(next() % 70) - 10;
                Picture           fast(64, 56, BMDS_PIXEL_RGB565), slow(64, 56, BMDS_PIXEL_RGB565);
                const bmds_rect_t clip = {int32_t(next() % 20), int32_t(next() % 20), 30 + next() % 34,
                                          30 + next() % 26};
                bmds_canvas_set_clip(&fast.canvas, &clip);
                const bmds_rect_t kept = fast.canvas.clip; // as the canvas clipped it to itself
                bmds_canvas_blit_rotated(&fast.canvas, cx, cy, turn, scale, &image, &src);
                ref_rotated(slow, cx, cy, turn, scale, image, src, kept);
                REQUIRE(fast.pixels == slow.pixels);
                for (uint8_t b : fast.pixels) drawn += b != 0;
        }
        CHECK(drawn > 100000); // most rounds drew something
}

TEST_CASE("Game canvas: rotated blits") {
        // A 4x2 sprite, its left half 1 and right half 2.
        const uint8_t     sheet[] = {1, 1, 2, 2, 1, 1, 2, 2};
        bmds_game_image_t image = {sheet, 4, 4, 2, BMDS_PIXEL_L8, 0, 0};
        Picture           p(8, 8, BMDS_PIXEL_L8);
        // Unturned at its size: as a plain blit, centred on (4, 4).
        bmds_canvas_blit_rotated(&p.canvas, 4, 4, 0, BMDS_GAME_SCALE, &image, nullptr);
        CHECK(p.rows() == "........\n........\n........\n..####..\n..####..\n........\n........\n........\n");
        CHECK(p.at(2, 3) == 1);
        CHECK(p.at(5, 4) == 2);
        // A quarter turn clockwise: standing up, the left half at the top.
        bmds_canvas_clear(&p.canvas, 0);
        bmds_canvas_damage_clear(&p.canvas);
        bmds_canvas_blit_rotated(&p.canvas, 4, 4, BMDS_GAME_TURN / 4, BMDS_GAME_SCALE, &image, nullptr);
        CHECK(p.rows() == "........\n........\n...##...\n...##...\n...##...\n...##...\n........\n........\n");
        CHECK(p.at(3, 2) == 1);
        CHECK(p.at(4, 5) == 2);
        CHECK(p.canvas.damage_count == 1);
        // A half turn: the halves swapped.
        bmds_canvas_clear(&p.canvas, 0);
        bmds_canvas_blit_rotated(&p.canvas, 4, 4, BMDS_GAME_TURN / 2, BMDS_GAME_SCALE, &image, nullptr);
        CHECK(p.at(2, 3) == 2);
        CHECK(p.at(5, 4) == 1);
        // Twice the size.
        bmds_canvas_clear(&p.canvas, 0);
        bmds_canvas_blit_rotated(&p.canvas, 4, 4, 0, 2 * BMDS_GAME_SCALE, &image, nullptr);
        CHECK(p.rows() == "........\n........\n########\n########\n########\n########\n........\n........\n");
        // An eighth: inside the bounds, and the damage covers what was drawn.
        Picture r(16, 16, BMDS_PIXEL_L8);
        bmds_canvas_blit_rotated(&r.canvas, 8, 8, BMDS_GAME_TURN / 8, 2 * BMDS_GAME_SCALE, &image, nullptr);
        REQUIRE(r.canvas.damage_count == 1);
        const bmds_rect_t &d = r.canvas.damage[0];
        int                drawn = 0;
        for (int32_t y = 0; y < 16; ++y)
                for (int32_t x = 0; x < 16; ++x)
                        if (r.at(x, y) != 0) {
                                ++drawn;
                                CHECK((x >= d.x && y >= d.y && x < d.x + int32_t(d.width) &&
                                       y < d.y + int32_t(d.height)));
                        }
        CHECK(drawn > 24); // about 32 pixels' area
        CHECK(drawn < 40);
}

TEST_CASE("Game canvas: shades and translucent fills") {
        Picture p(4, 2, BMDS_PIXEL_RGB565);
        bmds_canvas_clear(&p.canvas, bmds_canvas_pixel(&p.canvas, 0xffffff));
        bmds_canvas_damage_clear(&p.canvas);
        bmds_canvas_shade(&p.canvas, 0, 0, 2, 1, BMDS_GAME_OPAQUE / 2);
        CHECK(p.at(0, 0) == ((15u << 11) | (31u << 5) | 15u));
        CHECK(p.at(2, 0) == 0xffffu);
        CHECK(p.damage() == "0,0 2x1;");
        bmds_canvas_shade(&p.canvas, 0, 1, 1, 1, 0);
        CHECK(p.at(0, 1) == 0);
        bmds_canvas_shade(&p.canvas, 1, 1, 1, 1, BMDS_GAME_OPAQUE);
        CHECK(p.at(1, 1) == 0xffffu);
        // A shape's shadow: only where the sprite isn't its key.
        const uint16_t    sprite[] = {1, 0xf81f, 1, 1};
        bmds_game_image_t image = {
                reinterpret_cast<const uint8_t *>(sprite), 4, 2, 2, BMDS_PIXEL_RGB565, BMDS_GAME_IMAGE_KEYED, 0xf81f};
        Picture q(2, 2, BMDS_PIXEL_RGB565);
        bmds_canvas_clear(&q.canvas, 0xffff);
        bmds_canvas_shade_image(&q.canvas, 0, 0, &image, nullptr, 0);
        CHECK(q.rows() == ".#\n..\n");
        // Blends: half white over black, and a BGR24 quarter.
        Picture b(2, 1, BMDS_PIXEL_RGB565);
        bmds_canvas_fill_blend(&b.canvas, 0, 0, 1, 1, 0xffffff, BMDS_GAME_OPAQUE / 2);
        CHECK(b.at(0, 0) == ((15u << 11) | (31u << 5) | 15u));
        CHECK(b.at(1, 0) == 0);
        Picture c(1, 1, BMDS_PIXEL_BGR24);
        bmds_canvas_fill_blend(&c.canvas, 0, 0, 1, 1, 0x804020, BMDS_GAME_OPAQUE / 4);
        CHECK(c.at(0, 0) == 0x201008u);
        // L8: an index where at least half.
        Picture l(2, 1, BMDS_PIXEL_L8);
        bmds_canvas_fill_blend(&l.canvas, 0, 0, 1, 1, 5, BMDS_GAME_OPAQUE / 2);
        bmds_canvas_fill_blend(&l.canvas, 1, 0, 1, 1, 5, BMDS_GAME_OPAQUE / 4);
        CHECK(l.rows() == "#.\n");
}

TEST_CASE("Game canvas: copy between buffers, no damage") {
        Picture a(4, 2, BMDS_PIXEL_RGB565), b(4, 2, BMDS_PIXEL_RGB565);
        bmds_canvas_clear(&a.canvas, 5);
        const bmds_rect_t rects[] = {{1, 0, 2, 1}, {3, 1, 9, 9}};
        bmds_canvas_copy(&b.canvas, &a.canvas, rects, 2);
        CHECK(b.rows() == ".##.\n...#\n");
        CHECK(b.damage().empty());
}

TEST_CASE("Game text: glyphs, widths, Latin-1, others as ?") {
        CHECK(bmds_game_text_width("abc", 1) == 24);
        CHECK(bmds_game_text_width("ab\nabcd", 2) == 64);
        CHECK(bmds_game_text_width("\xc3\xa9t\xc3\xa9", 1) == 24); // été: three characters
        Picture p(16, 16, BMDS_PIXEL_L8);
        CHECK(bmds_canvas_text(&p.canvas, 0, 0, "A", 1, 1) == 8);
        // Each row as the glyph's bits.
        const unsigned char *a = bmds_game_font16['A' - 0x20];
        for (int32_t y = 0; y < 16; ++y)
                for (int32_t x = 0; x < 8; ++x) CHECK((p.at(x, y) != 0) == ((a[y] & (0x80 >> x)) != 0));
        CHECK(p.damage() == "0,0 8x16;");
        // Not in the font: '?'. Latin-1's: its own.
        Picture q(8, 16, BMDS_PIXEL_L8), r(8, 16, BMDS_PIXEL_L8);
        bmds_canvas_text(&q.canvas, 0, 0, "\xe2\x82\xac", 1, 1); // the euro sign
        bmds_canvas_text(&r.canvas, 0, 0, "?", 1, 1);
        CHECK(q.pixels == r.pixels);
        Picture s(8, 16, BMDS_PIXEL_L8);
        bmds_canvas_text(&s.canvas, 0, 0, "\xc3\xa9", 1, 1);
        const unsigned char *e = bmds_game_font16[0xe9 - 0xa0 + 95];
        CHECK((s.at(0, 0) != 0) == ((e[0] & 0x80) != 0));
        CHECK(s.pixels != r.pixels);
        // Scaled: each pixel 2x2; clipped at the edge, the damage too.
        Picture t(12, 40, BMDS_PIXEL_RGB565);
        bmds_canvas_text(&t.canvas, 0, 0, "A", 0xffff, 2);
        for (int32_t y = 0; y < 32; ++y) CHECK((t.at(5, y) != 0) == ((a[y / 2] & (0x80 >> 2)) != 0));
        CHECK(t.damage() == "0,0 12x32;");
}

TEST_CASE("Game images: BGRA32 converted in place, alpha and a colour as the key") {
        // B, G, R, A: red, half-transparent green, magenta (opaque), clear blue.
        const uint8_t         source[] = {0, 0, 255, 255, 0, 255, 0, 127, 255, 0, 255, 255, 255, 0, 0, 0};
        bmds_game_load_t      how = {sizeof(how), BMDS_PIXEL_RGB565, 0, 0, nullptr, 0, 0, 0};
        bmds_game_converter_t c;
        REQUIRE(bmds_game_converter_init(&c, &how) == 0);
        uint8_t buf[sizeof(source)];
        std::memcpy(buf, source, sizeof(buf));
        CHECK(bmds_game_convert(&c, buf, buf, 4) == 2); // in place
        uint16_t v[4];
        std::memcpy(v, buf, sizeof(v));
        CHECK(v[0] == 0xf800);
        CHECK(v[1] == BMDS_GAME_KEY_RGB565);
        CHECK(v[2] == (BMDS_GAME_KEY_RGB565 | 0x20)); // opaque magenta: moved off the key
        CHECK(v[3] == BMDS_GAME_KEY_RGB565);
        // BGR24, red the key colour.
        how.format = BMDS_PIXEL_BGR24;
        how.flags = BMDS_GAME_LOAD_KEY_RGB;
        how.key_rgb = 0xff0000;
        REQUIRE(bmds_game_converter_init(&c, &how) == 0);
        std::memcpy(buf, source, sizeof(buf));
        CHECK(bmds_game_convert(&c, buf, buf, 4) == 3);
        CHECK(Picture::bgr(buf, 0) == BMDS_GAME_KEY_BGR24);
        CHECK(Picture::bgr(buf, 2) == 0xff01ff);
        // L8: the nearest of the palette, the key index never a colour's.
        const uint32_t   palette[] = {0xff00ff, 0x000000, 0xf00000, 0x00ff00, 0x0000ff};
        bmds_game_load_t l8 = {sizeof(l8), BMDS_PIXEL_L8, 0, 0, palette, 5, 0, 0};
        REQUIRE(bmds_game_converter_init(&c, &l8) == 0);
        uint8_t out[4];
        CHECK(bmds_game_convert(&c, source, out, 4) == 2);
        CHECK(out[0] == 2); // red: 0xf00000
        CHECK(out[1] == 0); // transparent: the key
        CHECK(out[2] != 0); // magenta: not the key, though palette[0] matches it
        CHECK(out[3] == 0);
        // Refused: L8 without a palette, an unknown format, an old size.
        l8.palette = nullptr;
        CHECK(bmds_game_converter_init(&c, &l8) == -1);
        how.format = 9;
        CHECK(bmds_game_converter_init(&c, &how) == -1);
        how.format = BMDS_PIXEL_RGB565;
        how.size = 4;
        CHECK(bmds_game_converter_init(&c, &how) == -1);
}

TEST_CASE("Game sound: the mixer's voices, gains, loops, stops, stealing") {
        bmds_game_mixer_t m;
        bmds_game_mixer_init(&m);
        const int16_t     ones[4] = {16384, 16384, 16384, 16384}; // 0.5 each
        bmds_game_sound_t mono = {ones, 4, 1};
        float             out[8 * 2];
        // Panned hard left: the right silent; it plays its 4 frames, then ends.
        const int32_t v = bmds_game_mixer_play(&m, &mono, 1.0f, -1.0f, 0);
        REQUIRE(v >= 0);
        uint32_t g = 0;
        CHECK(bmds_game_mixer_voice(&m, v, &g) >= 0);
        bmds_game_mixer_run(&m, out, 8, 2);
        CHECK(out[0] == doctest::Approx(0.5f));
        CHECK(out[1] == 0.0f);
        CHECK(out[6] == doctest::Approx(0.5f));
        CHECK(out[8] == 0.0f);                         // past its end
        CHECK(bmds_game_mixer_voice(&m, v, &g) == -1); // ended
        // Looping: on through the period; stopped, it fades over the next and ends.
        const int32_t loop = bmds_game_mixer_play(&m, &mono, 0.5f, 0.0f, BMDS_GAME_PLAY_LOOP);
        bmds_game_mixer_run(&m, out, 8, 2);
        CHECK(out[14] == doctest::Approx(0.25f));
        CHECK(out[15] == doctest::Approx(0.25f));
        uint32_t            lg = 0;
        const int32_t       slot = bmds_game_mixer_voice(&m, loop, &lg);
        bmds_game_command_t stop = {BMDS_GAME_COMMAND_STOP, uint32_t(slot), lg, nullptr, 0, 0, 0, 0, 0};
        REQUIRE(bmds_game_mixer_push(&m, &stop) == 0);
        bmds_game_mixer_run(&m, out, 8, 2);
        CHECK(out[0] < 0.25f);
        CHECK(out[0] > out[14]); // falling
        CHECK(out[14] < 0.05f);
        CHECK(bmds_game_mixer_voice(&m, loop, &lg) == -1);
        // Mono out: the two sides averaged.
        bmds_game_mixer_play(&m, &mono, 1.0f, 0.0f, 0);
        bmds_game_mixer_run(&m, out, 2, 1);
        CHECK(out[0] == doctest::Approx(0.5f));
        // All busy: the oldest that doesn't loop taken; all looping: refused.
        bmds_game_mixer_init(&m);
        int32_t first = -1;
        for (uint32_t i = 0; i < BMDS_GAME_VOICES; ++i) {
                const int32_t x = bmds_game_mixer_play(&m, &mono, 1.0f, 0.0f, i == 0 ? 0 : BMDS_GAME_PLAY_LOOP);
                if (i == 0) first = x;
        }
        bmds_game_mixer_run(&m, out, 1, 2); // the queue taken
        const int32_t stolen = bmds_game_mixer_play(&m, &mono, 1.0f, 0.0f, BMDS_GAME_PLAY_LOOP);
        CHECK((stolen & 15) == (first & 15));
        CHECK(stolen != first);
        CHECK(bmds_game_mixer_voice(&m, first, &g) == -1);
        CHECK(bmds_game_mixer_play(&m, &mono, 1.0f, 0.0f, 0) == -BMDS_EBUSY);
        // Paused: silence, nothing moves.
        bmds_game_command_t pause = {BMDS_GAME_COMMAND_PAUSE, 0, 0, nullptr, 0, 0, 0, 1.0f, 0};
        bmds_game_mixer_push(&m, &pause);
        bmds_game_mixer_run(&m, out, 8, 2);
        CHECK(out[0] == 0.0f);
        // The queue full: refused.
        bmds_game_mixer_init(&m);
        for (uint32_t i = 0; i < BMDS_GAME_COMMANDS; ++i) bmds_game_mixer_push(&m, &pause);
        CHECK(bmds_game_mixer_play(&m, &mono, 1.0f, 0.0f, 0) == -BMDS_EAGAIN);
}

TEST_CASE("Game sound: resampling") {
        CHECK(bmds_game_resampled(44100, 44100, 48000) == 48000);
        CHECK(bmds_game_resampled(100, 8000, 48000) == 600);
        CHECK(bmds_game_resampled(5, 48000, 48000) == 5);
        const int16_t in[] = {0, 1000, 600, 0}; // two stereo frames
        int16_t       out[2 * 4];
        bmds_game_resample(in, 2, 2, 24000, 48000, out);
        CHECK(out[0] == 0);
        CHECK(out[1] == 1000);
        CHECK(out[2] == 300); // halfway
        CHECK(out[3] == 500);
        CHECK(out[4] == 600);
        CHECK(out[6] == 600); // the last held
}

TEST_CASE("Game canvas: copies but for some rectangles") {
        // Against a pixel at a time: in a copied rectangle and in none left out.
        uint32_t seed = 99;
        auto     next = [&seed] {
                seed = seed * 1103515245u + 12345u;
                return seed >> 8;
        };
        for (int round = 0; round < 300; ++round) {
                Picture from(48, 40, BMDS_PIXEL_RGB565), to(48, 40, BMDS_PIXEL_RGB565), want(48, 40, BMDS_PIXEL_RGB565);
                for (size_t i = 0; i < from.pixels.size(); ++i) from.pixels[i] = uint8_t(i * 7 + 1);
                auto random_rect = [&next] {
                        return bmds_rect_t{int32_t(next() % 60) - 6, int32_t(next() % 50) - 6, next() % 30,
                                           next() % 26};
                };
                bmds_rect_t    rects[6], except[BMDS_GAME_REPAINTED_MAX];
                const uint32_t count = 1 + next() % 6, excepts = next() % BMDS_GAME_REPAINTED_MAX;
                for (uint32_t i = 0; i < count; ++i) rects[i] = random_rect();
                for (uint32_t i = 0; i < excepts; ++i) except[i] = random_rect();
                bmds_game_copy_except(&to.canvas, &from.canvas, rects, count, except, excepts);
                auto inside = [](const bmds_rect_t &r, int32_t x, int32_t y) {
                        return x >= r.x && y >= r.y && x < r.x + int32_t(r.width) && y < r.y + int32_t(r.height);
                };
                for (int32_t y = 0; y < 40; ++y)
                        for (int32_t x = 0; x < 48; ++x) {
                                bool copy = false, kept = false;
                                for (uint32_t i = 0; i < count; ++i) copy |= inside(rects[i], x, y);
                                for (uint32_t i = 0; i < excepts; ++i) kept |= inside(except[i], x, y);
                                if (copy && !kept)
                                        std::memcpy(&want.pixels[size_t(y) * 96 + size_t(x) * 2],
                                                    &from.pixels[size_t(y) * 96 + size_t(x) * 2], 2);
                        }
                REQUIRE(to.pixels == want.pixels);
        }
}

TEST_CASE("Game canvas: scaled RGB565 blits draw exactly what a pixel at a time would") {
        const int32_t         sw = 41, sh = 33;
        std::vector<uint16_t> sheet(size_t(sw) * sh);
        for (size_t i = 0; i < sheet.size(); ++i) sheet[i] = uint16_t(i * 2654435761u >> 9) | 1u;
        sheet[7] = sheet[90] = 0; // keyed
        bmds_game_image_t image = {reinterpret_cast<const uint8_t *>(sheet.data()),
                                   uint32_t(sw) * 2,
                                   sw,
                                   sh,
                                   BMDS_PIXEL_RGB565,
                                   BMDS_GAME_IMAGE_KEYED,
                                   0};
        const bmds_rect_t src = {2, 3, 35, 27};
        uint32_t          seed = 7;
        auto              next = [&seed] {
                seed = seed * 1103515245u + 12345u;
                return seed >> 8;
        };
        for (int round = 0; round < 400; ++round) {
                Picture       fast(64, 56, BMDS_PIXEL_RGB565), slow(64, 56, BMDS_PIXEL_RGB565);
                const int32_t x = int32_t(next() % 70) - 10, y = int32_t(next() % 60) - 10;
                int32_t       w = 1 + int32_t(next() % 90), h = 1 + int32_t(next() % 80);
                if (next() % 3 == 0) w = -w; // mirrored
                if (next() % 4 == 0) h = -h;
                const bmds_rect_t clip = {int32_t(next() % 20), int32_t(next() % 20), 30 + next() % 34,
                                          30 + next() % 26};
                bmds_canvas_set_clip(&fast.canvas, &clip);
                const bmds_rect_t k = fast.canvas.clip;
                bmds_canvas_blit_scaled(&fast.canvas, x, y, w, h, &image, &src);
                // The reference: each destination pixel's centre mapped into the source.
                const int32_t aw = w < 0 ? -w : w, ah = h < 0 ? -h : h, ssw = int32_t(src.width),
                              ssh = int32_t(src.height);
                const int64_t step_x = (int64_t(ssw) << 16) / aw, step_y = (int64_t(ssh) << 16) / ah;
                for (int32_t row = std::max(y, k.y); row < std::min(y + ah, k.y + int32_t(k.height)); ++row) {
                        int32_t sy = int32_t((int64_t(row - y) * step_y + step_y / 2) >> 16);
                        if (sy >= ssh) sy = ssh - 1;
                        if (h < 0) sy = ssh - 1 - sy;
                        for (int32_t col = std::max(x, k.x); col < std::min(x + aw, k.x + int32_t(k.width)); ++col) {
                                int32_t sx = int32_t((int64_t(col - x) * step_x + step_x / 2) >> 16);
                                if (sx >= ssw) sx = ssw - 1;
                                if (w < 0) sx = ssw - 1 - sx;
                                const uint16_t px = sheet[size_t(src.y + sy) * sw + size_t(src.x + sx)];
                                if (px != 0) std::memcpy(&slow.pixels[size_t(row) * 128 + size_t(col) * 2], &px, 2);
                        }
                }
                REQUIRE(fast.pixels == slow.pixels);
        }
}
