/**
 * @file      ui.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop's pure parts: rectangles, regions, the compose plan, fonts.
 */

#include <doctest.h>

#include <cstring>
#include <vector>
#include <bmds/font.h>
#include <bmds/rect.h>
#include <bmds/region.h>

using bmds::ComposePiece;
using bmds::Font;
using bmds::Glyph;
using bmds::Rect;
using bmds::Region;

namespace {

        constexpr int Width = 64;
        constexpr int Height = 48;

        // A small deterministic generator.
        struct Random {
                        uint32_t state;
                        uint32_t next(uint32_t limit) {
                                state = state * 1103515245u + 12345u;
                                return (state >> 8) % limit;
                        }
                        Rect rect(bool even) {
                                uint16_t x = uint16_t(next(Width - 1));
                                uint16_t w = uint16_t(1 + next(uint32_t(Width - x)));
                                if (even) {
                                        x &= ~1u;
                                        w = uint16_t((w + 1) & ~1u);
                                        if (x + w > Width) w = uint16_t(Width - x);
                                }
                                const uint16_t y = uint16_t(next(Height - 1));
                                const uint16_t h = uint16_t(1 + next(uint32_t(Height - y)));
                                return Rect{x, y, w, h};
                        }
        };

        // A coverage count per pixel.
        struct Bitmap {
                        std::vector<int> pixels = std::vector<int>(Width * Height, 0);
                        void             add(const Rect &r, int v = 1) {
                                for (uint32_t y = r.y; y < r.bottom(); ++y) {
                                        for (uint32_t x = r.x; x < r.right(); ++x) pixels[y * Width + x] += v;
                                }
                        }
                        int at(int x, int y) const { return pixels[y * Width + x]; }
        };

        bool inside(const Rect &r, int x, int y) {
                return x >= r.x && uint32_t(x) < r.right() && y >= r.y && uint32_t(y) < r.bottom();
        }

        void checkDisjoint(const Region &region) {
                Bitmap b;
                for (const Rect &r : region.rects()) {
                        CHECK(!r.isEmpty());
                        b.add(r);
                }
                bool ok = true;
                for (int v : b.pixels) ok &= v <= 1;
                REQUIRE(ok);
        }

} // namespace

TEST_CASE("Rect: intersection, union, containment, alignment") {
        const Rect a{10, 10, 20, 10};
        const Rect b{25, 5, 10, 10};
        CHECK(a.intersects(b));
        CHECK(a.intersected(b) == Rect{25, 10, 5, 5});
        CHECK(a.united(b) == Rect{10, 5, 25, 15});
        CHECK(!a.intersects(Rect{30, 10, 5, 5})); // touching is not sharing
        CHECK(a.intersected(Rect{30, 10, 5, 5}).isEmpty());
        CHECK(a.contains(Rect{10, 10, 20, 10}));
        CHECK(!a.contains(b));
        CHECK(a.contains(Rect{}));
        CHECK(Rect{}.united(a) == a);
        CHECK(Rect{3, 1, 4, 2}.alignedX(2) == Rect{2, 1, 6, 2});
        CHECK(Rect{4, 1, 4, 2}.alignedX(2) == Rect{4, 1, 4, 2});
        CHECK(Rect{5, 0, 1, 1}.alignedX(8) == Rect{0, 0, 8, 1});
}

TEST_CASE("subtract: the pieces cover exactly a minus b") {
        Random rnd{7};
        for (int t = 0; t < 2000; ++t) {
                const Rect   a = rnd.rect(false);
                const Rect   b = rnd.rect(false);
                Rect         out[4];
                const size_t n = bmds::subtract(a, b, out);
                Bitmap       got;
                for (size_t i = 0; i < n; ++i) got.add(out[i]);
                bool ok = true;
                for (int y = 0; y < Height; ++y) {
                        for (int x = 0; x < Width; ++x)
                                ok &= got.at(x, y) == (inside(a, x, y) && !inside(b, x, y) ? 1 : 0);
                }
                REQUIRE(ok);
        }
}

TEST_CASE("Region: adding keeps it disjoint and exact") {
        Random rnd{11};
        for (int t = 0; t < 500; ++t) {
                Region region;
                Bitmap want;
                for (int i = 0; i < 5; ++i) {
                        const Rect r = rnd.rect(false);
                        region.add(r);
                        want.add(r);
                }
                checkDisjoint(region);
                const bool collapsed = region.rects().size() == 1 && region.area() > 0;
                bool       ok = true;
                for (int y = 0; y < Height; ++y) {
                        for (int x = 0; x < Width; ++x) {
                                bool covered = false;
                                for (const Rect &r : region.rects()) covered |= inside(r, x, y);
                                ok &= want.at(x, y) == 0 || covered;
                                ok &= collapsed || covered == (want.at(x, y) > 0);
                        }
                }
                REQUIRE(ok);
        }
}

TEST_CASE("Region: neighbors merge; overflow collapses to the bounds") {
        Region region;
        region.add(Rect{0, 0, 10, 16});
        region.add(Rect{10, 0, 10, 16});
        region.add(Rect{0, 16, 20, 16});
        REQUIRE(region.rects().size() == 1);
        CHECK(region.rects()[0] == Rect{0, 0, 20, 32});
        region.add(Rect{5, 5, 3, 3}); // already covered
        CHECK(region.rects().size() == 1);
        region.add(Rect{0, 0, 30, 40}); // holds it
        CHECK(region.rects().size() == 1);
        CHECK(region.rects()[0] == Rect{0, 0, 30, 40});

        Region scattered;
        for (uint16_t i = 0; i < 40; ++i) scattered.add(Rect{uint16_t(i * 3), uint16_t(i % 7 * 5), 1, 1});
        CHECK(scattered.rects().size() <= Region::MaxRects);
        for (uint16_t i = 0; i < 40; ++i) {
                bool covered = false;
                for (const Rect &r : scattered.rects()) covered |= inside(r, i * 3, i % 7 * 5);
                CHECK(covered);
        }
        scattered.clear();
        CHECK(scattered.isEmpty());
}

TEST_CASE("planCompose: each damaged pixel once, from the topmost layer") {
        Random rnd{23};
        for (int t = 0; t < 400; ++t) {
                const bool        even = t % 2 == 0;
                std::vector<Rect> layers;
                const int         windows = int(rnd.next(6));
                for (int i = 0; i < windows; ++i) layers.push_back(rnd.rect(even));
                if (t % 3 != 0) layers.push_back(Rect{0, 0, Width, Height}); // the desktop
                Region damage;
                for (int i = 0, n = 1 + int(rnd.next(4)); i < n; ++i) damage.add(rnd.rect(even));

                ComposePiece pieces[256];
                const auto   planned = bmds::planCompose(bmds::Span<const Rect>(layers.data(), layers.size()), damage,
                                                         bmds::Span<ComposePiece>(pieces, 256));
                REQUIRE(planned.isOk());
                std::vector<int> from(Width * Height, -1);
                bool             ok = true;
                for (size_t i = 0; i < planned.value(); ++i) {
                        const ComposePiece &p = pieces[i];
                        REQUIRE(p.layer < layers.size());
                        REQUIRE(layers[p.layer].contains(p.rect));
                        if (even) REQUIRE(((p.rect.x | p.rect.width) & 1) == 0);
                        for (uint32_t y = p.rect.y; y < p.rect.bottom(); ++y) {
                                for (uint32_t x = p.rect.x; x < p.rect.right(); ++x) {
                                        ok &= from[y * Width + x] == -1; // drawn once
                                        from[y * Width + x] = p.layer;
                                }
                        }
                }
                for (int y = 0; y < Height; ++y) {
                        for (int x = 0; x < Width; ++x) {
                                bool damaged = false;
                                for (const Rect &r : damage.rects()) damaged |= inside(r, x, y);
                                int top = -1;
                                for (size_t l = 0; l < layers.size() && top < 0; ++l) {
                                        if (inside(layers[l], x, y)) top = int(l);
                                }
                                ok &= from[y * Width + x] == (damaged ? top : -1);
                        }
                }
                REQUIRE(ok);
        }
}

TEST_CASE("planCompose: too many pieces is Full") {
        const Rect layers[] = {{10, 10, 4, 4}, {0, 0, Width, Height}};
        Region     damage;
        damage.add(Rect{0, 0, Width, Height});
        ComposePiece pieces[2];
        const auto   planned = bmds::planCompose(layers, damage, bmds::Span<ComposePiece>(pieces, 2));
        CHECK(planned.error() == bmds::Error::Full);
}

TEST_CASE("Font: unscii-16") {
        const Font &font = bmds::Fonts::unscii16;
        CHECK(font.width() == 8);
        CHECK(font.height() == 16);
        CHECK(font.glyphCount() > 2000);

        const uint8_t a[16] = {0x00, 0x00, 0x18, 0x3c, 0x66, 0x66, 0x66, 0x7e,
                               0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00};
        Glyph         g = font.glyph('A');
        CHECK(g.width == 8);
        CHECK(std::memcmp(g.rows, a, 16) == 0);
        g = font.glyph(0x2500); // ─
        CHECK(g.rows[7] == 0xff);
        CHECK(g.rows[8] == 0xff);
        CHECK(g.rows[6] == 0x00);

        g = font.glyph(0xff21); // fullwidth A: two cells
        CHECK(g.width == 16);
        CHECK(g.rows[4] == 0x03);
        CHECK(g.rows[5] == 0xc0);

        CHECK(!font.hasGlyph(0x01));   // controls are left out
        CHECK(!font.hasGlyph(0xe000)); // and the private use area
        CHECK(!font.hasGlyph(0x10fffd));
        CHECK(font.glyph(0x10fffd).rows == font.glyph(0x25a1).rows); // the replacement: □
        // Every printable ASCII character has its own glyph.
        for (uint32_t c = 0x20; c < 0x7f; ++c) CHECK(font.hasGlyph(c));
}
