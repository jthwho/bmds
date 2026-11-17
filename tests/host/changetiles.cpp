/**
 * @file      changetiles.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ChangeTiles (devplan/vnc.md §5b): a screen watcher's tiles, and the rectangles they become.
 */

#include <doctest.h>

#include <vector>
#include <bmds/changetiles.h>

using bmds::ChangeTiles;
using bmds::Rect;
using bmds::Span;

namespace {

        // Every rectangle taken, with no pixel budget to speak of.
        std::vector<Rect> takeAll(ChangeTiles &t) {
                std::vector<Rect> all;
                Rect              out[64];
                for (;;) {
                        const size_t n = t.takeRects(Span<Rect>(out, 64), UINT32_MAX);
                        all.insert(all.end(), out, out + n);
                        if (n < 64) break;
                }
                return all;
        }

        // A tiles' worth of coverage per pixel, to check rectangles never overlap and cover exactly.
        std::vector<int> coverage(const std::vector<Rect> &rects, int width, int height) {
                std::vector<int> c(size_t(width * height), 0);
                for (const Rect &r : rects)
                        for (int y = r.y; y < int(r.bottom()); ++y)
                                for (int x = r.x; x < int(r.right()); ++x) ++c[size_t(y * width + x)];
                return c;
        }

} // namespace

TEST_CASE("ChangeTiles: a new picture is all changed, as one rectangle") {
        ChangeTiles t;
        REQUIRE(t.resize(1280, 720));
        CHECK_FALSE(t.isEmpty());
        const std::vector<Rect> rects = takeAll(t);
        REQUIRE(rects.size() == 1);
        CHECK(rects[0].x == 0);
        CHECK(rects[0].y == 0);
        CHECK(rects[0].width == 1280);
        CHECK(rects[0].height == 720);
        CHECK(t.isEmpty());
}

TEST_CASE("ChangeTiles: edges are clipped to the picture") {
        ChangeTiles t;
        REQUIRE(t.resize(1920, 1080)); // 60 × 67.5 tiles
        t.clear();
        t.mark(Rect{1900, 1070, 100, 100});
        const std::vector<Rect> rects = takeAll(t);
        REQUIRE(rects.size() == 1);
        CHECK(rects[0].x == 1888);
        CHECK(rects[0].y == 1056); // row 1070 is in the tile from 1056; the next ends at 1080
        CHECK(rects[0].width == 32);
        CHECK(rects[0].height == 24);
}

TEST_CASE("ChangeTiles: too big a picture is refused") {
        ChangeTiles t;
        CHECK_FALSE(t.resize(2049, 100));
        CHECK_FALSE(t.resize(100, 1089));
        CHECK(t.resize(2048, 1088));
}

TEST_CASE("ChangeTiles: marks become disjoint rectangles covering exactly the tiles") {
        ChangeTiles t;
        REQUIRE(t.resize(640, 480));
        uint32_t seed = 7;
        for (int round = 0; round < 200; ++round) {
                t.clear();
                std::vector<int> want(size_t(640 * 480), 0);
                const int        marks = 1 + int(seed % 6);
                for (int m = 0; m < marks; ++m) {
                        seed = seed * 1103515245u + 12345u;
                        const uint16_t x = uint16_t((seed >> 8) % 640);
                        seed = seed * 1103515245u + 12345u;
                        const uint16_t y = uint16_t((seed >> 8) % 480);
                        seed = seed * 1103515245u + 12345u;
                        const uint16_t w = uint16_t(1 + (seed >> 8) % 200);
                        seed = seed * 1103515245u + 12345u;
                        const uint16_t h = uint16_t(1 + (seed >> 8) % 120);
                        t.mark(Rect{x, y, w, h});
                        // The tiles it touches, as pixels.
                        const int right = x + w < 640 ? x + w : 640, bottom = y + h < 480 ? y + h : 480;
                        for (int ty = y / 16 * 16; ty < bottom; ty += 16)
                                for (int tx = x / 32 * 32; tx < right; tx += 32)
                                        for (int py = ty; py < ty + 16 && py < 480; ++py)
                                                for (int px = tx; px < tx + 32 && px < 640; ++px)
                                                        want[size_t(py * 640 + px)] = 1;
                }
                const std::vector<Rect> rects = takeAll(t);
                const std::vector<int>  got = coverage(rects, 640, 480);
                REQUIRE(got == want);
                CHECK(t.isEmpty());
        }
}

TEST_CASE("ChangeTiles: a pixel budget leaves the rest marked") {
        ChangeTiles t;
        REQUIRE(t.resize(256, 64)); // 8 × 4 tiles
        t.clear();
        t.mark(Rect{0, 0, 32, 16});    // tile (0, 0)
        t.mark(Rect{128, 32, 32, 16}); // tile (4, 2)
        Rect   out[4];
        size_t n = t.takeRects(Span<Rect>(out, 4), 32 * 16); // room for one tile
        REQUIRE(n == 1);
        CHECK(out[0].x == 0);
        CHECK_FALSE(t.isEmpty());
        n = t.takeRects(Span<Rect>(out, 4), 32 * 16);
        REQUIRE(n == 1);
        CHECK(out[0].x == 128);
        CHECK(out[0].y == 32);
        CHECK(t.isEmpty());
        // And a full output array stops it the same way.
        t.markAll();
        n = t.takeRects(Span<Rect>(out, 0), UINT32_MAX);
        CHECK(n == 0);
        CHECK_FALSE(t.isEmpty());
}

TEST_CASE("ChangeTiles: a rectangle bigger than the budget is taken a band of tile rows at a time") {
        ChangeTiles t;
        REQUIRE(t.resize(1280, 720)); // all marked: one 1280 × 720 rectangle
        Rect     out[4];
        uint32_t rows = 0, calls = 0;
        while (!t.isEmpty()) {
                const size_t n = t.takeRects(Span<Rect>(out, 4), 1280 * 100); // 6 tile rows' worth
                REQUIRE(n == 1);
                CHECK(out[0].width == 1280);
                CHECK(out[0].y == rows);
                CHECK(out[0].height <= 100);
                rows += out[0].height;
                ++calls;
        }
        CHECK(rows == 720);
        CHECK(calls == 8); // 96 rows a call: 7 bands and the last 48
        t.markAll();
        CHECK(t.takeRects(Span<Rect>(out, 4), 1280 * 15) == 0); // less than a tile row: nothing
}

TEST_CASE("ChangeTiles: a picture changing all over each frame is taken in turn, top to bottom") {
        // A game full screen: everything marked again before each take, the budget a third of it.
        ChangeTiles t;
        REQUIRE(t.resize(1280, 720));
        Rect     out[4];
        uint32_t taken[720] = {};
        for (int frame = 0; frame < 6; ++frame) {
                t.markAll();
                const size_t n = t.takeRects(Span<Rect>(out, 4), 1280 * 240);
                REQUIRE(n == 1);
                for (uint16_t y = out[0].y; y < out[0].bottom(); ++y) ++taken[y];
        }
        for (uint32_t y = 0; y < 720; ++y) CHECK(taken[y] >= 1); // the bottom too, not the top six times
        CHECK(taken[0] <= 3);
}

TEST_CASE("ChangeTiles: a surface's bitmap ORs in, wider sources clipped") {
        ChangeTiles t;
        REQUIRE(t.resize(96, 32)); // 3 × 2 tiles
        t.clear();
        // A source two words wide: columns 0, 2 and 40 (past the picture) in row 1.
        const uint32_t src[2 * 2] = {0, 0, (1u << 0) | (1u << 2), 1u << 8};
        t.orIn(src, 2, 2);
        const std::vector<Rect> rects = takeAll(t);
        REQUIRE(rects.size() == 2);
        CHECK(rects[0].x == 0);
        CHECK(rects[0].y == 16);
        CHECK(rects[1].x == 64);
        CHECK(rects[1].width == 32);
}

TEST_CASE("ChangeTiles: another's tiles OR in") {
        ChangeTiles t, other;
        REQUIRE(t.resize(96, 32)); // 3 × 2 tiles
        REQUIRE(other.resize(96, 32));
        t.clear();
        other.clear();
        t.mark(Rect{0, 0, 1, 1});
        other.mark(Rect{64, 16, 1, 1});
        t.orIn(other);
        CHECK_FALSE(other.isEmpty()); // left as it was
        const std::vector<Rect> rects = takeAll(t);
        REQUIRE(rects.size() == 2);
        CHECK(rects[0].x == 0);
        CHECK(rects[0].y == 0);
        CHECK(rects[1].x == 64);
        CHECK(rects[1].y == 16);
}
