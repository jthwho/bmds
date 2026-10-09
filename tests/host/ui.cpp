/**
 * @file      ui.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop's pure parts: rectangles, regions, the compose plan, the
 * window stack, fonts.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/rect.h>
#include <bmds/compose.h>
#include <bmds/region.h>
#include <bmds/servicequeue.h>
#include <bmds/terminalscreen.h>
#include <bmds/terminalview.h>
#include <bmds/windowpalette.h>
#include <bmds/windowstack.h>

using bmds::ComposePiece;
using bmds::Font;
using bmds::Glyph;
using bmds::Rect;
using bmds::Region;
using bmds::WindowFrame;
using bmds::WindowPalette;
using bmds::WindowStack;

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

TEST_CASE("Region: past its rectangles, the cheapest pairs merge, not everything") {
        // Sixteen squares, each where it was and where it moved (an animation's
        // damage): far more pieces than MaxRects.
        Region region;
        Bitmap want;
        for (int i = 0; i < 16; ++i) {
                const uint16_t x = uint16_t((i * 37) % 56), y = uint16_t((i * 23) % 40);
                const Rect     was{x, y, 6, 6};
                const Rect     now{uint16_t(x + 2), uint16_t(y + 1), 6, 6};
                for (const Rect &r : {was, now}) {
                        region.add(r);
                        want.add(r);
                }
        }
        checkDisjoint(region);
        CHECK(region.rects().size() <= Region::MaxRects);
        uint32_t wanted = 0;
        bool     ok = true;
        for (int y = 0; y < Height; ++y) {
                for (int x = 0; x < Width; ++x) {
                        bool covered = false;
                        for (const Rect &r : region.rects()) covered |= inside(r, x, y);
                        ok &= want.at(x, y) == 0 || covered;
                        wanted += want.at(x, y) > 0 ? 1 : 0;
                }
        }
        REQUIRE(ok);
        // Close to what was added: well under the bounding box.
        CHECK(region.area() < wanted * 2);
        CHECK(region.area() < region.bounds().area() / 2);
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
                // The desktop, when there is one, as the bottom layer.
                const size_t windowCount = size_t(windows);
                const Rect   bottom = layers.size() > windowCount ? layers.back() : Rect();
                const auto   planned = bmds::planCompose(bmds::Span<const Rect>(layers.data(), windowCount), bottom,
                                                         damage, bmds::Span<ComposePiece>(pieces, 256));
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
        const Rect layers[] = {{10, 10, 4, 4}};
        Region     damage;
        damage.add(Rect{0, 0, Width, Height});
        ComposePiece pieces[2];
        const auto   planned =
                bmds::planCompose(layers, Rect{0, 0, Width, Height}, damage, bmds::Span<ComposePiece>(pieces, 2));
        CHECK(planned.error() == bmds::Error::Full);
}

namespace {

        // Who shows each pixel: the topmost window's number, 0 for the desktop.
        std::vector<int> owners(const WindowStack &stack) {
                std::vector<int> o(Width * Height, 0);
                for (size_t z = stack.count(); z-- > 0;) {
                        const Rect r = stack.rectAt(z);
                        for (uint32_t y = r.y; y < r.bottom(); ++y)
                                for (uint32_t x = r.x; x < r.right(); ++x) o[y * Width + x] = stack.idAt(z);
                }
                return o;
        }

        std::vector<int> covered(const Region &region) {
                std::vector<int> c(Width * Height, 0);
                for (const Rect &r : region.rects())
                        for (uint32_t y = r.y; y < r.bottom(); ++y)
                                for (uint32_t x = r.x; x < r.right(); ++x) c[y * Width + x] = 1;
                return c;
        }

        // A placeable rectangle: x and width on 4-pixel columns.
        Rect windowRect(Random &rnd) {
                Rect r = rnd.rect(false);
                r.x = uint16_t(r.x & ~3u);
                r.width = uint16_t((r.width + 3) & ~3u);
                if (r.right() > Width) r.width = uint16_t(Width - r.x);
                return r;
        }

} // namespace

TEST_CASE("WindowStack: numbers, order, refusals") {
        WindowStack stack(Width, Height);
        CHECK(stack.damage().area() == uint32_t(Width * Height));
        stack.clearDamage();
        CHECK(stack.open(Rect{1, 0, 8, 8}).error() == bmds::Error::Invalid);
        CHECK(stack.open(Rect{0, 0, 6, 8}).error() == bmds::Error::Invalid);
        CHECK(stack.open(Rect{60, 0, 8, 8}).error() == bmds::Error::Invalid);
        CHECK(stack.open(Rect{0, 0, 8, 8}).value() == 1);
        CHECK(stack.open(Rect{4, 4, 8, 8}).value() == 2);
        CHECK(stack.open(Rect{8, 8, 8, 8}).value() == 3);
        CHECK(stack.idAt(0) == 3);
        CHECK(stack.close(2).isOk());
        CHECK(stack.close(2) == bmds::Error::NotFound);
        CHECK(stack.open(Rect{0, 0, 8, 8}).value() == 2); // the lowest free number
        CHECK(stack.raise(1).isOk());
        CHECK(stack.idAt(0) == 1);
        CHECK(stack.idAt(1) == 2);
        CHECK(stack.idAt(2) == 3);
        CHECK(stack.layerOf(3).value() == 2);
        // Hidden: in no layer, its number kept, shown again on top.
        CHECK(stack.hide(2).isOk());
        CHECK(stack.count() == 2);
        CHECK(stack.hiddenCount() == 1);
        CHECK(stack.hiddenIdAt(0) == 2);
        CHECK(stack.isHidden(2));
        CHECK(stack.layerOf(2).error() == bmds::Error::NotFound);
        CHECK(stack.raise(2) == bmds::Error::NotFound);
        CHECK(stack.hide(2) == bmds::Error::NotFound);
        CHECK(stack.rectOf(2).value() == Rect{0, 0, 8, 8});
        CHECK(stack.move(2, Rect{4, 0, 8, 8}).isOk());
        CHECK(stack.open(Rect{0, 0, 8, 8}).value() == 4); // 2 is still taken
        CHECK(stack.close(4).isOk());
        CHECK(stack.show(2).isOk());
        CHECK(stack.show(2) == bmds::Error::NotFound);
        CHECK(stack.idAt(0) == 2);
        CHECK(stack.rectAt(0) == Rect{4, 0, 8, 8});
        CHECK(stack.hiddenCount() == 0);
        CHECK(stack.hide(3).isOk());
        CHECK(stack.close(3).isOk()); // hidden, closed
        CHECK(stack.hiddenCount() == 0);
        CHECK(stack.open(Rect{8, 8, 8, 8}).value() == 3);
        CHECK(stack.raise(1).isOk());
        CHECK(stack.raise(2).isOk());
        CHECK(stack.raise(3).isOk());
        CHECK(stack.raise(1).isOk());
        for (size_t i = stack.count(); i < WindowStack::MaxWindows; ++i) REQUIRE(stack.open(Rect{0, 0, 4, 4}).isOk());
        CHECK(stack.open(Rect{0, 0, 4, 4}).error() == bmds::Error::Full);
}

TEST_CASE("WindowStack: levels keep panels and popups above, a desktop window below") {
        using Level = WindowStack::Level;
        WindowStack   stack(Width, Height);
        const uint8_t desk = stack.open(Rect{0, 0, Width, Height}, Level::Desktop).value();
        const uint8_t panel = stack.open(Rect{0, 0, Width, 8}, Level::Panel).value();
        const uint8_t a = stack.open(Rect{0, 0, 16, 16}).value();
        const uint8_t b = stack.open(Rect{8, 8, 16, 16}).value();
        auto          order = [&] {
                std::vector<int> ids;
                for (size_t z = 0; z < stack.count(); ++z) ids.push_back(stack.idAt(z));
                return ids;
        };
        CHECK(order() == std::vector<int>{panel, b, a, desk});
        CHECK(stack.raise(desk).isOk());
        CHECK(stack.raise(a).isOk());
        CHECK(order() == std::vector<int>{panel, a, b, desk});
        const uint8_t menu = stack.open(Rect{4, 4, 8, 8}, Level::Popup).value();
        CHECK(order() == std::vector<int>{menu, panel, a, b, desk});
        CHECK(stack.levelOf(menu) == Level::Popup);
        // A tooltip goes above even a menu; a menu opened after stays under it.
        const uint8_t tip = stack.open(Rect{4, 4, 8, 4}, Level::Tooltip).value();
        const uint8_t menu2 = stack.open(Rect{8, 4, 8, 8}, Level::Popup).value();
        CHECK(order() == std::vector<int>{tip, menu2, menu, panel, a, b, desk});
        CHECK(stack.close(tip).isOk());
        CHECK(stack.close(menu2).isOk());
        CHECK(stack.hide(a).isOk());
        CHECK(stack.raise(b).isOk());
        CHECK(stack.show(a).isOk());
        CHECK(order() == std::vector<int>{menu, panel, a, b, desk});
        // A raise under a higher level damages only what it uncovers: not what the panel covers.
        stack.clearDamage();
        CHECK(stack.close(menu).isOk());
        stack.clearDamage();
        CHECK(stack.raise(b).isOk());
        const std::vector<int> d = covered(stack.damage());
        for (int y = 0; y < 8; ++y)
                for (int x = 0; x < Width; ++x) CHECK(d[y * Width + x] == 0);
        CHECK(d[10 * Width + 10] == 1); // a covered b there
        CHECK(stack.levelOf(99) == Level::Normal);
        // Always on top: above every normal window, under the panel; back down, on top of the normal ones.
        CHECK(stack.setLevel(a, Level::Above).isOk());
        CHECK(stack.raise(b).isOk());
        CHECK(order() == std::vector<int>{panel, a, b, desk});
        CHECK(WindowStack::isApplication(stack.levelOf(a)));
        CHECK(!WindowStack::isApplication(Level::Panel));
        CHECK(stack.setLevel(a, Level::Normal).isOk());
        CHECK(order() == std::vector<int>{panel, a, b, desk});
        CHECK(stack.raise(b).isOk());
        CHECK(order() == std::vector<int>{panel, b, a, desk});
        // Hidden: keeps its new level, shown on top of it.
        CHECK(stack.hide(a).isOk());
        CHECK(stack.setLevel(a, Level::Above).isOk());
        CHECK(stack.show(a).isOk());
        CHECK(order() == std::vector<int>{panel, a, b, desk});
        CHECK(stack.setLevel(99, Level::Above) == bmds::Error::NotFound);
}

TEST_CASE("WindowStack: each change damages what changed, a raise and an invalidation no more") {
        Random rnd{77};
        for (int t = 0; t < 300; ++t) {
                WindowStack stack(Width, Height);
                // Every other run on random levels.
                const bool levels = t % 2 == 1;
                auto       level = [&] {
                        return levels ? static_cast<WindowStack::Level>(rnd.next(4)) : WindowStack::Level::Normal;
                };
                for (int i = 0, n = 1 + int(rnd.next(6)); i < n; ++i)
                        REQUIRE(stack.open(windowRect(rnd), level()).isOk());
                for (int step = 0; step < 6; ++step) {
                        if (stack.count() == 0) REQUIRE(stack.show(stack.hiddenIdAt(0)).isOk());
                        stack.clearDamage();
                        const std::vector<int> before = owners(stack);
                        const uint8_t          id = stack.idAt(rnd.next(uint32_t(stack.count())));
                        const Rect             was = stack.rectAt(stack.layerOf(id).value());
                        const int              op = int(rnd.next(7));
                        Rect                   bound = was; // where damage may be
                        Rect                   asked;
                        if (op == 5) {
                                REQUIRE(stack.hide(id).isOk());
                        } else if (op == 6) {
                                if (stack.hiddenCount() == 0) continue;
                                const uint8_t hidden = stack.hiddenIdAt(rnd.next(uint32_t(stack.hiddenCount())));
                                bound = stack.rectOf(hidden).value();
                                REQUIRE(stack.show(hidden).isOk());
                                const size_t z = stack.layerOf(hidden).value();
                                REQUIRE((z == 0 || stack.levelOf(stack.idAt(z - 1)) > stack.levelOf(hidden)));
                        } else if (op == 0) {
                                const Rect r = windowRect(rnd);
                                const auto opened = stack.open(r, level());
                                if (opened.isError()) continue;
                                bound = r;
                        } else if (op == 1 && stack.count() > 1) {
                                REQUIRE(stack.close(id).isOk());
                        } else if (op == 2) {
                                const Rect r = windowRect(rnd);
                                REQUIRE(stack.move(id, r).isOk());
                                bound = was.united(r);
                        } else if (op == 3) {
                                REQUIRE(stack.raise(id).isOk());
                        } else {
                                asked = rnd.rect(false);
                                REQUIRE(stack.invalidate(id, asked).isOk());
                        }
                        // The levels in order, the highest on top.
                        for (size_t z = 1; z < stack.count(); ++z)
                                REQUIRE(stack.levelOf(stack.idAt(z - 1)) >= stack.levelOf(stack.idAt(z)));
                        const std::vector<int> after = owners(stack);
                        const std::vector<int> damaged = covered(stack.damage());
                        const bool             exact = stack.damage().rects().size() < Region::MaxRects / 2;
                        bool                   ok = true;
                        for (int y = 0; y < Height; ++y) {
                                for (int x = 0; x < Width; ++x) {
                                        const int  i = y * Width + x;
                                        const bool d = damaged[i] != 0;
                                        ok &= before[i] == after[i] || d; // a change is damage
                                        ok &= !d || inside(bound, x, y);  // nothing far away
                                        if (op == 3 && exact) ok &= d == (before[i] != after[i]);
                                        if (op == 4 && exact) ok &= d == (inside(asked, x, y) && after[i] == id);
                                }
                        }
                        REQUIRE(ok);
                }
        }
}

TEST_CASE("WindowStack: up to 255 windows, numbers reused, changes still damage what changed") {
        Random rnd{255};
        for (int t = 0; t < 12; ++t) {
                WindowStack stack(Width, Height);
                const int   n = 64 + int(rnd.next(WindowStack::MaxWindows - 64 + 1));
                for (int i = 0; i < n; ++i) REQUIRE(stack.open(windowRect(rnd)).isOk());
                for (int step = 0; step < 20; ++step) {
                        stack.clearDamage();
                        const std::vector<int> before = owners(stack);
                        const uint8_t          id = stack.idAt(rnd.next(uint32_t(stack.count())));
                        const int              op = int(rnd.next(4));
                        if (op == 0) {
                                REQUIRE(stack.close(id).isOk());
                                REQUIRE(stack.open(windowRect(rnd)).value() == id); // the lowest free
                        } else if (op == 1) {
                                REQUIRE(stack.move(id, windowRect(rnd)).isOk());
                        } else if (op == 2) {
                                REQUIRE(stack.raise(id).isOk());
                        } else {
                                REQUIRE(stack.hide(id).isOk());
                                REQUIRE(stack.show(id).isOk());
                        }
                        const std::vector<int> after = owners(stack);
                        const std::vector<int> damaged = covered(stack.damage());
                        bool                   ok = true;
                        for (int i = 0; i < Width * Height; ++i) ok &= before[i] == after[i] || damaged[i] != 0;
                        REQUIRE(ok);
                }
                if (n == int(WindowStack::MaxWindows)) CHECK(stack.open(Rect{0, 0, 4, 4}).error() == bmds::Error::Full);
        }
}

TEST_CASE("WindowStack: the plan gives each piece to the window that shows it") {
        Random rnd{5};
        for (int t = 0; t < 200; ++t) {
                WindowStack stack(Width, Height);
                for (int i = 0, n = int(rnd.next(7)); i < n; ++i) (void)stack.open(windowRect(rnd));
                Region damage;
                for (int i = 0, n = 1 + int(rnd.next(4)); i < n; ++i) damage.add(rnd.rect(false));
                ComposePiece pieces[256];
                const auto   planned = stack.plan(damage, bmds::Span<ComposePiece>(pieces, 256));
                REQUIRE(planned.isOk());
                const std::vector<int> o = owners(stack);
                std::vector<int>       drawn(Width * Height, 0);
                bool                   ok = true;
                for (size_t i = 0; i < planned.value(); ++i) {
                        const ComposePiece &p = pieces[i];
                        const int           id = p.layer == stack.count() ? 0 : stack.idAt(p.layer);
                        for (uint32_t y = p.rect.y; y < p.rect.bottom(); ++y)
                                for (uint32_t x = p.rect.x; x < p.rect.right(); ++x) {
                                        ok &= o[y * Width + x] == id;
                                        ++drawn[y * Width + x];
                                }
                }
                const std::vector<int> d = covered(damage);
                for (int i = 0; i < Width * Height; ++i) ok &= drawn[i] == d[i];
                REQUIRE(ok);
        }
}

TEST_CASE("WindowFrame: for every set of decorations, the parts tile the window, on 4-pixel columns") {
        using Part = WindowFrame::Part;
        const Rect outer{100, 50, 200, 120};
        int        valid = 0;
        for (unsigned flags = 0; flags < 256; ++flags) {
                const auto d = static_cast<uint8_t>(flags);
                if (!WindowFrame::isValid(d)) {
                        CHECK(((d & ~WindowFrame::Standard) != 0 || (d & WindowFrame::TitleBar) == 0 ||
                               ((d & WindowFrame::Resizable) != 0 && (d & WindowFrame::Border) == 0)));
                        continue;
                }
                ++valid;
                const WindowFrame f(outer, d);
                Rect              edges[4];
                const size_t      n = f.edges(edges);
                Region            parts;
                uint32_t          area = f.titleBar().area() + f.client().area();
                parts.add(f.titleBar());
                parts.add(f.client());
                for (size_t i = 0; i < n; ++i) {
                        parts.add(edges[i]);
                        area += edges[i].area();
                }
                CHECK(parts.area() == outer.area()); // the parts cover it
                CHECK(area == outer.area());         // and don't overlap
                CHECK(f.client().x % 4 == 0);
                CHECK(f.client().width % 4 == 0);
                CHECK(f.client().width == outer.width - (WindowFrame::outerWidth(0, d)));
                CHECK(f.client().height == outer.height - (WindowFrame::outerHeight(0, d)));
                if (d == WindowFrame::None) CHECK(f.client() == outer);
                // Buttons on the title bar, apart from each other and the title.
                Rect   buttons[4];
                size_t nb = 0;
                for (const Part p : {Part::Menu, Part::Minimize, Part::Maximize, Part::Close}) {
                        const Rect r = f.button(p);
                        if (r.isEmpty()) continue;
                        CHECK(f.titleBar().contains(r));
                        CHECK(r.x % 4 == 0);
                        CHECK(r.width % 4 == 0);
                        for (size_t i = 0; i < nb; ++i) CHECK(!buttons[i].intersects(r));
                        CHECK(!r.intersects(f.title()));
                        buttons[nb++] = r;
                        CHECK(f.partAt(r.x, r.y) == p);
                        CHECK(f.partAt(uint16_t(r.right() - 1), uint16_t(r.bottom() - 1)) == p);
                }
                CHECK(f.button(Part::Close).isEmpty() == ((d & WindowFrame::CloseButton) == 0));
                CHECK(f.button(Part::Menu).isEmpty() == ((d & WindowFrame::MenuButton) == 0));
                CHECK(f.titleBar().contains(f.title()));
                CHECK(f.title().x % 4 == 0);
                // The close button is the rightmost.
                if (!f.button(Part::Close).isEmpty()) CHECK(f.button(Part::Close).right() == outer.right() - 4);
                CHECK(f.partAt(outer.x, uint16_t(outer.bottom())) == Part::Outside);
                CHECK(f.partAt(uint16_t(outer.x + outer.width / 2), uint16_t(outer.y + outer.height / 2)) ==
                      Part::Client);
                if (n > 0)
                        CHECK(f.partAt(edges[n - 1].x, edges[n - 1].y) ==
                              (f.has(WindowFrame::Resizable) ? Part::ResizeBottomLeft : Part::Edge));
                if (f.has(WindowFrame::TitleBar))
                        CHECK(f.partAt(f.title().x, uint16_t(f.title().y + 8)) == Part::TitleBar);
                CHECK(WindowFrame::minWidth(d) >= WindowFrame::outerWidth(WindowFrame::MinClient, d));
                CHECK(WindowFrame(Rect{0, 0, WindowFrame::minWidth(d), WindowFrame::minHeight(d)}, d).title().width >=
                      (f.has(WindowFrame::TitleBar) ? 8u : 0u));
        }
        // None, the border (resizable or not), and the title bar with any buttons and no border, a
        // border, or a resizable border.
        CHECK(valid == 3 + 3 * 16);
        CHECK(!WindowFrame::isValid(WindowFrame::Resizable));
        CHECK(!WindowFrame::isValid(WindowFrame::TitleBar | WindowFrame::Resizable));
        CHECK(!WindowFrame::isValid(WindowFrame::CloseButton));
        CHECK(WindowFrame::isValid(WindowFrame::Standard));
        Rect outline[4];
        WindowFrame::outline(outer, outline);
        Region ring;
        for (const Rect &r : outline) ring.add(r);
        CHECK(ring.area() == outer.area() - uint32_t(outer.width - 8) * (outer.height - 8));
}

TEST_CASE("WindowFrame: resize handles around the ring, corners near the corners, buttons first") {
        using Part = WindowFrame::Part;
        const Rect        outer{100, 50, 200, 120};
        const WindowFrame f(outer, WindowFrame::Standard);
        const uint16_t    l = 100, r = 299, t = 50, b = 169;
        CHECK(f.partAt(l, 100) == Part::ResizeLeft);
        CHECK(f.partAt(l + 3, 100) == Part::ResizeLeft);
        CHECK(f.partAt(l + 4, 100) == Part::Client);
        CHECK(f.partAt(r, 100) == Part::ResizeRight);
        CHECK(f.partAt(200, b) == Part::ResizeBottom);
        CHECK(f.partAt(200, t) == Part::ResizeTop); // the title bar's top rows
        CHECK(f.partAt(200, t + 4) == Part::TitleBar);
        CHECK(f.partAt(l, t) == Part::ResizeTopLeft);
        CHECK(f.partAt(l + 15, t) == Part::ResizeTopLeft); // along the top, near the corner
        CHECK(f.partAt(l + 16, t) == Part::ResizeTop);
        CHECK(f.partAt(l, t + 15) == Part::ResizeTopLeft); // down the side
        CHECK(f.partAt(l, t + 16) == Part::ResizeLeft);
        CHECK(f.partAt(r, t) == Part::ResizeTopRight);
        CHECK(f.partAt(l, b) == Part::ResizeBottomLeft);
        CHECK(f.partAt(r, b) == Part::ResizeBottomRight);
        CHECK(f.partAt(r - 15, b) == Part::ResizeBottomRight);
        CHECK(f.partAt(r, b - 15) == Part::ResizeBottomRight);
        const Rect close = f.button(Part::Close);
        CHECK(f.partAt(close.x, close.y) == Part::Close); // over the ring's top rows: the button
        CHECK(f.partAt(close.x, uint16_t(close.y - 1)) == Part::ResizeTop);
        // Not resizable: the border is just the border.
        const WindowFrame fixed(outer, WindowFrame::Standard & ~WindowFrame::Resizable);
        CHECK(fixed.partAt(l, 100) == Part::Edge);
        CHECK(fixed.partAt(200, t) == Part::TitleBar);
        CHECK(WindowFrame::edgesOf(Part::ResizeTopRight) == (WindowFrame::TopEdge | WindowFrame::RightEdge));
        CHECK(WindowFrame::edgesOf(Part::Close) == 0);
}

TEST_CASE("WindowFrame: a resize keeps the other edges, the screen, the least size and the steps") {
        using Part = WindowFrame::Part;
        Random         rnd{91};
        const Rect     screen{0, 0, 640, 480};
        const Part     handles[] = {Part::ResizeLeft,       Part::ResizeRight,      Part::ResizeTop,
                                    Part::ResizeBottom,     Part::ResizeTopLeft,    Part::ResizeTopRight,
                                    Part::ResizeBottomLeft, Part::ResizeBottomRight};
        const uint8_t  decorations[] = {WindowFrame::Standard, WindowFrame::Border | WindowFrame::Resizable,
                                        WindowFrame::Border | WindowFrame::TitleBar | WindowFrame::Resizable |
                                                WindowFrame::CloseButton};
        const uint16_t steps[][2] = {{1, 1}, {8, 16}, {3, 5}, {4, 1}};
        for (int i = 0; i < 20000; ++i) {
                const uint8_t  d = decorations[rnd.next(3)];
                const uint16_t ws = steps[rnd.next(4)][0], hs = steps[rnd.next(4)][1];
                const uint16_t minW = WindowFrame::minWidth(d), minH = WindowFrame::minHeight(d);
                Rect           start;
                start.width = uint16_t((minW + rnd.next(400)) & ~3u);
                if (start.width < minW) start.width = uint16_t(start.width + 4);
                start.height = uint16_t(minH + rnd.next(300));
                start.x = uint16_t(rnd.next(screen.width - start.width + 1) & ~3u);
                start.y = uint16_t(rnd.next(screen.height - start.height + 1));
                const uint8_t edges = WindowFrame::edgesOf(handles[rnd.next(8)]);
                const int     dx = int(rnd.next(1400)) - 700, dy = int(rnd.next(1000)) - 500;
                const Rect    r = WindowFrame::resizedRect(start, edges, dx, dy, d, ws, hs, screen);
                REQUIRE(screen.contains(r));
                REQUIRE(r.x % 4 == 0);
                REQUIRE(r.width % 4 == 0);
                REQUIRE(r.width >= minW);
                REQUIRE(r.height >= minH);
                const Rect client = WindowFrame(r, d).client();
                int        wstep = ws;
                while (wstep % 4 != 0) wstep += ws;
                const bool sizingW = (edges & (WindowFrame::LeftEdge | WindowFrame::RightEdge)) != 0;
                const bool sizingH = (edges & (WindowFrame::TopEdge | WindowFrame::BottomEdge)) != 0;
                if (sizingW) REQUIRE(client.width % wstep == 0);
                if (sizingH) REQUIRE(client.height % hs == 0);
                if (!sizingW) REQUIRE(r.width == start.width);
                if (!sizingH) REQUIRE(r.height == start.height);
                // The least size: the decorations' least client area, up to whole steps.
                const int extraW = WindowFrame::outerWidth(0, d), extraH = WindowFrame::outerHeight(0, d);
                const int leastW = extraW + (minW - extraW + wstep - 1) / wstep * wstep;
                const int leastH = extraH + (minH - extraH + hs - 1) / hs * hs;
                // An edge not held stays, unless the least size pushed the window off the screen.
                if ((edges & WindowFrame::LeftEdge) == 0 && r.width <= start.width) REQUIRE(r.x == start.x);
                if ((edges & WindowFrame::RightEdge) == 0 && r.width <= start.width)
                        REQUIRE(r.right() == start.right());
                if ((edges & WindowFrame::TopEdge) == 0 && r.height <= start.height) REQUIRE(r.y == start.y);
                if ((edges & WindowFrame::BottomEdge) == 0 && r.height <= start.height)
                        REQUIRE(r.bottom() == start.bottom());
                // A held edge goes where the pointer took it, to within a step (and the screen).
                if ((edges & WindowFrame::RightEdge) != 0 && r.width > leastW) {
                        const int want = std::clamp(int(start.right()) + dx, 0, int(screen.right()));
                        REQUIRE(int(r.right()) <= want);
                        REQUIRE(int(r.right()) > want - wstep);
                }
                if ((edges & WindowFrame::TopEdge) != 0 && r.height > leastH) {
                        const int want = std::clamp(int(start.y) + dy, 0, int(screen.bottom()));
                        REQUIRE(int(r.y) >= want);
                        REQUIRE(int(r.y) < want + hs);
                }
        }
        // Not moved: unchanged when the size is already in steps.
        const Rect start{40, 30, 216, 200};
        CHECK(WindowFrame::resizedRect(start, WindowFrame::RightEdge | WindowFrame::BottomEdge, 0, 0,
                                       WindowFrame::Standard, 1, 1, screen) == start);
}

TEST_CASE("WindowPalette: every role named, found by its name, colors kept to 24 bits") {
        for (size_t i = 0; i < WindowPalette::RoleCount; ++i) {
                const auto  role = static_cast<WindowPalette::Role>(i);
                const char *name = WindowPalette::name(role);
                REQUIRE(name != nullptr);
                CHECK(WindowPalette::find(name).value() == role);
                for (size_t k = 0; k < i; ++k)
                        CHECK(std::strcmp(name, WindowPalette::name(static_cast<WindowPalette::Role>(k))) != 0);
        }
        CHECK(WindowPalette::name(static_cast<WindowPalette::Role>(WindowPalette::RoleCount)) == nullptr);
        CHECK(WindowPalette::find("active").error() == bmds::Error::NotFound);
        CHECK(WindowPalette::find("desktop").value() == WindowPalette::Role::Desktop);
        CHECK(WindowPalette::find("outline").value() == WindowPalette::Role::Outline);
        CHECK(WindowPalette::find("terminal-cursor").error() == bmds::Error::NotFound); // the terminal's own
        CHECK(WindowPalette::find("face").value() == WindowPalette::Role::Face);
        CHECK(WindowPalette::find("cursor-fill").value() == WindowPalette::Role::CursorFill);
        WindowPalette p;
        CHECK(p.color(WindowPalette::Role::Desktop) == 0x404040);
        p.setColor(WindowPalette::Role::Client, 0xff123456);
        CHECK(p.color(WindowPalette::Role::Client) == 0x123456);
        CHECK(!(p == WindowPalette()));
}

TEST_CASE("GlyphRenderer: the scalar kernel draws each glyph's bits, bold and underlined") {
        using bmds::GlyphRenderer;
        using bmds::TextCell;
        const Font &font = bmds::Fonts::unscii16;
        CHECK(GlyphRenderer::isSupported(GlyphRenderer::Kernel::Scalar));
        GlyphRenderer      renderer(GlyphRenderer::Kernel::Scalar);
        constexpr size_t   Stride = 64 * 3; // 64 pixels a row
        alignas(8) uint8_t out[Stride * 16];
        const uint32_t     codepoints[] = {'A', 'g', 0x2591, 0x263a, 0xff21, 0x4dc0, '|', ' '};
        const uint32_t     colors[][2] = {{0xffffff, 0x000000}, {0x123456, 0xfedcba}, {0x00ff00, 0x00ff00}};
        bool               ok = true;
        for (const uint32_t cp : codepoints) {
                for (const auto &c : colors) {
                        for (uint8_t flags = 0; flags < 4; ++flags) {
                                const Glyph g = font.glyph(cp);
                                std::memset(out, 0xaa, sizeof(out));
                                renderer.draw(g, font.height(), flags, c[0], c[1], out + 8 * 3, Stride);
                                for (unsigned y = 0; y < font.height(); ++y) {
                                        for (unsigned x = 0; x < 64; ++x) {
                                                const uint8_t *p = out + y * Stride + x * 3;
                                                const uint32_t got = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                                                if (x < 8 || x >= 8u + g.width) {
                                                        ok &= got == 0xaaaaaa; // nothing outside
                                                        continue;
                                                }
                                                const unsigned col = x - 8, bpr = g.width / 8u;
                                                auto           bit = [&](unsigned cx) {
                                                        return (g.rows[y * bpr + cx / 8] & (0x80u >> (cx % 8))) != 0;
                                                };
                                                bool on = bit(col);
                                                if ((flags & TextCell::Bold) != 0 && col > 0) on |= bit(col - 1);
                                                if ((flags & TextCell::Underline) != 0 && y == font.height() - 1u)
                                                        on = true;
                                                ok &= got == (on ? c[0] : c[1]);
                                        }
                                }
                        }
                }
        }
        CHECK(ok);
        CHECK(font.glyph('A').rows == font.glyph(0x41).rows);
}

TEST_CASE("GlyphRenderer: runs of cells and text match glyph by glyph, cut at the width") {
        using bmds::GlyphRenderer;
        using bmds::TextCell;
        const Font        &font = bmds::Fonts::unscii16;
        GlyphRenderer      renderer(GlyphRenderer::Kernel::Scalar);
        constexpr size_t   Pixels = 128, Stride = Pixels * 3;
        alignas(8) uint8_t runOut[Stride * 16], oneOut[Stride * 16];
        TextCell           cells[6];
        const uint32_t     cps[6] = {'a', 0xff21, 'b', 'b', 0x2591, 'c'}; // a wide one
        for (int i = 0; i < 6; ++i) {
                cells[i].codepoint = cps[i];
                cells[i].fg = 0x102030u * uint32_t(i + 1);
                cells[i].bg = i % 2 != 0 ? 0x000000 : 0xffffff;
                cells[i].flags = static_cast<uint8_t>(i % 4);
        }
        std::memset(runOut, 0, sizeof(runOut));
        std::memset(oneOut, 0, sizeof(oneOut));
        size_t         used = 0;
        const uint16_t w = renderer.drawCells(bmds::Span<const TextCell>(cells, 6), font, runOut, Stride, 50, used);
        CHECK(w == 48); // 8 + 16 + 8 + 8 + 8, then 'c' (8) past 50
        CHECK(used == 5);
        uint16_t x = 0;
        for (size_t i = 0; i < used; ++i) {
                const Glyph g = font.glyph(cells[i].codepoint);
                renderer.draw(g, font.height(), cells[i].flags, cells[i].fg, cells[i].bg, oneOut + x * 3, Stride);
                x = static_cast<uint16_t>(x + g.width);
        }
        CHECK(std::memcmp(runOut, oneOut, sizeof(runOut)) == 0);
        size_t         bytes = 0;
        const uint16_t tw = renderer.drawText("ab\xe2\x96\x91"
                                              "cd",
                                              font, 0xffffff, 0, runOut, Stride, 28, bytes);
        CHECK(tw == 24); // a, b, the shade; 'c' would pass 28
        CHECK(bytes == 5);
}

TEST_CASE("TerminalView: cells with defaults, attributes, wide characters and the cursor") {
        using bmds::TerminalScreen;
        using bmds::TextCell;
        TerminalScreen screen(bmds::MemSpace::Psram);
        REQUIRE(screen.resize(10, 2).isOk());
        // Plain, bold red, reverse, dim, invisible + underline, a wide character, RGB on blue.
        screen.write("a\x1b[1;31mb\x1b[0;7mc\x1b[0;2md\x1b[0;8;4me\x1b[0m\xef\xbc\xa1\x1b[38;2;1;2;3;44mf");
        bmds::TerminalColors p;
        p.text = 0xc0c0c0;
        p.background = 0x000010;
        p.cursor = 0x00ff00;
        TextCell     out[10];
        const size_t n = bmds::TerminalView::row(screen, 0, p, 1, out, 10);
        CHECK(n == 9); // 10 columns, the wide one's tail left out
        CHECK(out[0].codepoint == 'a');
        CHECK(out[0].fg == 0xc0c0c0);
        CHECK(out[0].bg == 0x000010);
        CHECK(out[0].flags == 0);
        CHECK(out[1].flags == TextCell::Bold); // the cursor's cell
        CHECK(out[1].bg == 0x00ff00);
        CHECK(out[1].fg == 0x000010);
        CHECK(out[2].fg == 0x000010); // reverse
        CHECK(out[2].bg == 0xc0c0c0);
        CHECK(out[3].fg == 0x606060);  // dim
        CHECK(out[4].fg == out[4].bg); // invisible
        CHECK(out[4].flags == TextCell::Underline);
        CHECK(out[5].codepoint == 0xff21);
        CHECK(out[6].codepoint == 'f');
        CHECK(out[6].fg == 0x010203);
        CHECK(out[6].bg == 0x0000ee); // xterm's blue
        CHECK(out[7].codepoint == ' ');
        // Without a cursor, the bold red keeps its colors.
        bmds::TerminalView::row(screen, 0, p, -1, out, 10);
        CHECK(out[1].fg == 0xcd0000);
        CHECK(out[1].bg == 0x000010);
        // The ANSI colors set; the 256-color palette's others fixed.
        p.ansi[1] = 0xff5555;
        bmds::TerminalView::row(screen, 0, p, -1, out, 10);
        CHECK(out[1].fg == 0xff5555);
        CHECK(p.rgb(bmds::Color::indexed(196), 0) == 0xff0000);
        CHECK(p.rgb(bmds::Color(), 0x123456) == 0x123456);
        // The cursor on a wide character's tail is on the character.
        bmds::TerminalView::row(screen, 0, p, 6, out, 10);
        CHECK(out[5].bg == 0x00ff00);
        CHECK(out[6].bg != 0x00ff00);
}

TEST_CASE("TerminalColors: colors by name, as the terminal's file and resources set them") {
        using bmds::TerminalColors;
        TerminalColors c;
        CHECK(c == TerminalColors());
        CHECK(c.text == 0xc0c0c0);
        CHECK(c.ansi[4] == 0x0000ee);
        CHECK(TerminalColors::parse("1a2B3c").value() == 0x1a2b3c);
        CHECK(TerminalColors::parse(" #ffffff ").value() == 0xffffff);
        CHECK(TerminalColors::parse("fff").error() == bmds::Error::Invalid);
        CHECK(TerminalColors::parse("12345g").error() == bmds::Error::Invalid);
        CHECK(TerminalColors::parse("").error() == bmds::Error::Invalid);
        CHECK(c.set("Text", "102030").isOk());
        CHECK(c.text == 0x102030);
        CHECK(c.set("background", "#010203").isOk());
        CHECK(c.background == 0x010203);
        CHECK(c.set("CURSOR", "00ff00").isOk());
        CHECK(c.cursor == 0x00ff00);
        CHECK(c.set("Color0", "111111").isOk());
        CHECK(c.ansi[0] == 0x111111);
        CHECK(c.set("color15", "eeeeee").isOk());
        CHECK(c.ansi[15] == 0xeeeeee);
        CHECK(c.find("Color16") == nullptr);
        CHECK(c.find("Color01") == nullptr);
        CHECK(c.find("Color") == nullptr);
        CHECK(c.find("Color1a") == nullptr);
        CHECK(c.find("Colour1") == nullptr);
        CHECK(c.find("Color9") == &c.ansi[9]);
        CHECK(c.set("Foreground", "000000") == bmds::Error::NotFound);
        CHECK(c.set("Text", "nope") == bmds::Error::Invalid);
        CHECK(c.text == 0x102030); // kept
}

TEST_CASE("TerminalView: a view scrolled back shows history lines, then the screen's rows") {
        using bmds::TerminalScreen;
        using bmds::TerminalView::viewCursorRow;
        using bmds::TerminalView::viewRow;
        TerminalScreen screen(bmds::MemSpace::Psram);
        REQUIRE(screen.resize(3, 2).isOk());
        REQUIRE(screen.setHistory(10).isOk());
        const char *text = "a\r\nb\r\nc\r\nd"; // a and b scroll off; c, d shown
        screen.write(bmds::StringView(text));
        REQUIRE(screen.historyCount() == 2);
        auto first = [&](size_t offset, uint16_t row) {
                uint16_t                  length = 0;
                const bmds::TerminalCell *line = viewRow(screen, offset, row, length);
                return length > 0 ? char(line[0].codepoint) : ' ';
        };
        CHECK(first(0, 0) == 'c');
        CHECK(first(0, 1) == 'd');
        CHECK(first(1, 0) == 'b');
        CHECK(first(1, 1) == 'c');
        CHECK(first(2, 0) == 'a');
        CHECK(first(2, 1) == 'b');
        CHECK(first(9, 0) == 'a'); // clamped to the history
        CHECK(viewCursorRow(screen, 0) == 1);
        CHECK(viewCursorRow(screen, 1) == -1); // scrolled below the view
        screen.write(bmds::StringView("\x1b[H"));
        CHECK(viewCursorRow(screen, 1) == 1);
        screen.write(bmds::StringView("\x1b[?25l"));
        CHECK(viewCursorRow(screen, 0) == -1);
        // A history line padded to the width; a wide character cut at the edge blanked.
        uint16_t                  length = 0;
        const bmds::TerminalCell *line = viewRow(screen, 2, 0, length);
        CHECK(length == 1);
        bmds::TextCell       out[8];
        bmds::TerminalColors p;
        CHECK(bmds::TerminalView::cells(line, length, 3, p, -1, out, 8) == 3);
        CHECK(out[0].codepoint == 'a');
        CHECK(out[2].codepoint == ' ');
        bmds::TerminalCell wide[3];
        wide[0].codepoint = 'x';
        wide[1].codepoint = 0xff21;
        wide[1].wide = 1;
        wide[2].tail = 1;
        CHECK(bmds::TerminalView::cells(wide, 3, 3, p, -1, out, 8) == 2); // x, then the wide one
        CHECK(out[1].codepoint == 0xff21);
        CHECK(bmds::TerminalView::cells(wide, 3, 2, p, -1, out, 8) == 2); // cut: x and a blank
        CHECK(out[1].codepoint == ' ');
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

TEST_CASE("Font: the icons, 16 by 16 in the private use area") {
        const Font &icons = bmds::Fonts::icons16;
        CHECK(icons.glyphCount() == 8); // seven icons and a blank
        for (uint32_t cp = bmds::IconGlyph::Menu; cp <= bmds::IconGlyph::Submenu; ++cp) {
                REQUIRE(icons.hasGlyph(cp));
                const Glyph g = icons.glyph(cp);
                CHECK(g.width == 16);
                int bits = 0;
                for (int i = 0; i < 32; ++i) bits += __builtin_popcount(g.rows[i]);
                CHECK(bits > 0);
                if (cp > bmds::IconGlyph::Menu) CHECK(g.rows != icons.glyph(cp - 1).rows);
        }
        // The close icon is an ×: symmetric both ways.
        const Glyph x = icons.glyph(bmds::IconGlyph::Close);
        for (int r = 0; r < 16; ++r)
                for (int c = 0; c < 16; ++c) {
                        auto bit = [&](int rr, int cc) {
                                return (x.rows[rr * 2 + cc / 8] >> (7 - cc % 8)) & 1;
                        };
                        CHECK(bit(r, c) == bit(r, 15 - c));
                        CHECK(bit(r, c) == bit(15 - r, c));
                }
}

TEST_CASE("ServiceQueue: messages in order, refused when empty, too long or too many, kept when too big") {
        bmds::ServiceQueue q;
        uint8_t            out[bmds::ServiceQueue::MaxMessage];
        auto               take = [&]() {
                return q.take(bmds::WritableByteSpan(out, sizeof(out)));
        };
        CHECK(q.isEmpty());
        CHECK(take().value() == 0);
        const uint8_t a[] = {'a', 0, 'b'};
        const uint8_t b[] = {'z'};
        CHECK(q.push(bmds::ByteSpan()) == bmds::Error::Invalid);
        std::vector<uint8_t> big(bmds::ServiceQueue::MaxMessage + 1, 'x');
        CHECK(q.push(bmds::ByteSpan(big.data(), big.size())) == bmds::Error::Invalid);
        REQUIRE(q.push(bmds::ByteSpan(a, sizeof(a))).isOk());
        REQUIRE(q.push(bmds::ByteSpan(b, sizeof(b))).isOk());
        CHECK(q.pendingBytes() == 2 + 3 + 2 + 1);
        // Too small a buffer: kept.
        uint8_t small[2];
        CHECK(q.take(bmds::WritableByteSpan(small, sizeof(small))).error() == bmds::Error::BufferTooSmall);
        auto first = take();
        REQUIRE(first.value() == 3);
        CHECK(std::memcmp(out, a, 3) == 0);
        auto second = take();
        REQUIRE(second.value() == 1);
        CHECK(out[0] == 'z');
        CHECK(q.isEmpty());
        // Full past MaxPending: four of the longest fit less their lengths.
        big.resize(bmds::ServiceQueue::MaxMessage);
        size_t pushed = 0;
        while (q.push(bmds::ByteSpan(big.data(), big.size())).isOk()) ++pushed;
        CHECK(pushed == bmds::ServiceQueue::MaxPending / (bmds::ServiceQueue::MaxMessage + 2));
        CHECK(q.push(bmds::ByteSpan(big.data(), big.size())) == bmds::Error::Full);
        CHECK(take().value() == bmds::ServiceQueue::MaxMessage);
        CHECK(q.push(bmds::ByteSpan(big.data(), big.size())).isOk()); // room again
        q.clear();
        CHECK(q.isEmpty());
}
