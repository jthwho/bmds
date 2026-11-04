/**
 * @file      scripts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Script regions (devplan/preso.md §7, step 6): smooth shapes, beats as steps, fast-forward
 * for Previous, determinism, the runaway stop, errors, the sandbox, and the presenter's frames
 * for a region that moves; decks and their scripts from files the test gives.
 */

#include <doctest.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/presenter.h"
#include "core/raster.h"
#include "headless/headless.h"
#include "script/runtime.h"

using preso::Action;
using preso::Canvas;
using preso::Color;
using preso::Error;
using preso::PixelFormat;
using preso::Presenter;
using preso::Rect;
using preso::StringView;

namespace {

        constexpr int64_t Ms = 1000000;

        // Files by name, a screen, the clock the test's (or the real one), animation recorded.
        class ScriptPlatform : public preso::HeadlessPlatform {
                public:
                        struct File {
                                        const char *path;
                                        const char *text;
                        };
                        const File *files = nullptr;
                        uint32_t    fileCount = 0;
                        int64_t     clock = 0;
                        bool        realClock = false;
                        bool        animating = false;
                        int32_t     width = 0, height = 0;
                        uint32_t   *screen = nullptr;
                        Canvas      last;
                        uint32_t    presents = 0;
                        uint32_t    areas = 0;
                        Rect        lastArea;

                        ~ScriptPlatform() override { free(screen); }
                        Error readFile(const char *path, uint32_t, char **data, uint32_t *length) override {
                                for (uint32_t i = 0; i < fileCount; i++) {
                                        if (strcmp(files[i].path, path) != 0) continue;
                                        size_t n = strlen(files[i].text);
                                        *data = static_cast<char *>(malloc(n + 1));
                                        memcpy(*data, files[i].text, n + 1);
                                        *length = uint32_t(n);
                                        return Error::Ok;
                                }
                                return Error::NotFound;
                        }
                        PixelFormat pixelFormat() const override { return PixelFormat::Xrgb8888; }
                        Error       openScreen(int32_t w, int32_t h, const char *, bool) override {
                                width = w;
                                height = h;
                                free(screen); // a second presenter on the same platform
                                screen = static_cast<uint32_t *>(calloc(size_t(w) * size_t(h), 4));
                                last.attach(screen, uint32_t(w) * 4, w, h, PixelFormat::Xrgb8888);
                                return Error::Ok;
                        }
                        void present(const Canvas &c) override {
                                last.copyFrom(c);
                                presents++;
                        }
                        void presentAreas(const Canvas &c, const Rect *list, uint32_t count) override {
                                last.copyFrom(c);
                                areas++;
                                lastArea = Rect{};
                                for (uint32_t i = 0; i < count; i++) lastArea = preso::uniteRects(lastArea, list[i]);
                        }
                        void animate(bool on) override { animating = on; }
                        // What step 7 asks of a platform: the keys' set, the pointer, sounds.
                        preso::InputMode mode = preso::InputMode::Deck;
                        bool             pointerShown = false;
                        uint32_t         soundsOpened = 0;
                        uint32_t         soundsPlayed = 0;
                        void             setInputMode(preso::InputMode m) override { mode = m; }
                        void             setPointerVisible(bool v) override { pointerShown = v; }
                        bool             fileExists(const char *path) override {
                                for (uint32_t i = 0; i < fileCount; i++) {
                                        if (strcmp(files[i].path, path) == 0) return true;
                                }
                                return false;
                        }
                        int32_t openSound(const char *) override { return int32_t(soundsOpened++); }
                        void    playSound(int32_t, float) override { soundsPlayed++; }
                        int64_t now() override {
                                if (!realClock) return clock;
                                struct timespec t;
                                clock_gettime(CLOCK_MONOTONIC, &t);
                                return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
                        }
                        Error watch(const char *) override { return Error::Ok; }
                        Error run(preso::App &) override { return Error::Ok; }
        };

        // What `preso check` says of a deck, and its status.
        struct Checked {
                        char text[4096] = {};
                        int  status = 0;
        };

        Checked check(ScriptPlatform &platform, const char *path) {
                Checked c;
                FILE   *out = fmemopen(c.text, sizeof c.text - 1, "w");
                c.status = preso::checkDeck(platform, path, true, out, preso::makeAngelScripts);
                fclose(out);
                return c;
        }

        uint32_t count(const Canvas &c, Color color, const Rect &r) {
                uint32_t n = 0;
                for (int32_t y = r.y; y < r.y + r.height; y++) {
                        for (int32_t x = r.x; x < r.x + r.width; x++) n += c.colorAt(x, y) == color;
                }
                return n;
        }

        // The region's top left on a 1280 × 720 paper slide with only it: the content's box's.
        constexpr int32_t RegionX = 80;

} // namespace

TEST_CASE("Raster: shapes are filled with each pixel's coverage") {
        uint32_t pixels[32 * 32];
        Canvas   c;
        c.attach(pixels, 128, 32, 32, PixelFormat::Xrgb8888);
        c.fill(Rect{0, 0, 32, 32}, 0x000000);
        preso::Path path;
        // A square on whole pixels: inside full, outside nothing.
        path.addRect(4, 4, 8, 8);
        REQUIRE(preso::fillPath(c, path, 0xFFFFFF, 255) == Error::Ok);
        CHECK(c.colorAt(4, 4) == 0xFFFFFF);
        CHECK(c.colorAt(11, 11) == 0xFFFFFF);
        CHECK(c.colorAt(12, 12) == 0x000000);
        CHECK(c.colorAt(3, 8) == 0x000000);
        // Half a pixel over its edge: half covered.
        c.fill(Rect{0, 0, 32, 32}, 0x000000);
        path.clear();
        path.addRect(4.5f, 4, 8, 8);
        (void)preso::fillPath(c, path, 0xFFFFFF, 255);
        Color edge = c.colorAt(4, 8);
        CHECK(((edge >> 8) & 0xFF) > 0x70);
        CHECK(((edge >> 8) & 0xFF) < 0x90);
        CHECK(c.colorAt(12, 8) == edge);
        // A circle: its middle full, its corners' outside empty, its edge between.
        c.fill(Rect{0, 0, 32, 32}, 0x000000);
        path.clear();
        path.addEllipse(16, 16, 10, 10);
        (void)preso::fillPath(c, path, 0xFFFFFF, 255);
        CHECK(c.colorAt(16, 16) == 0xFFFFFF);
        CHECK(c.colorAt(8, 8) == 0x000000);
        bool between = false;
        for (int32_t x = 0; x < 32; x++) {
                Color p = c.colorAt(x, 16);
                if (p != 0 && p != 0xFFFFFF) between = true;
        }
        CHECK(between);
        // Clipped: nothing outside the clip.
        c.fill(Rect{0, 0, 32, 32}, 0x000000);
        c.setClip(Rect{0, 0, 16, 32});
        (void)preso::fillPath(c, path, 0xFFFFFF, 255);
        CHECK(c.colorAt(20, 16) == 0x000000);
        CHECK(c.colorAt(10, 16) == 0xFFFFFF);
        // A transform and its inverse undo one another.
        preso::Affine t = preso::Affine::translation(5, 7).after(preso::Affine::rotation(30)), back;
        REQUIRE(t.inverted(&back));
        preso::Point p = back.apply(t.apply(preso::Point{3, 4}));
        CHECK(p.x == doctest::Approx(3).epsilon(0.001));
        CHECK(p.y == doctest::Approx(4).epsilon(0.001));
}

TEST_CASE("Scripts: beats are steps, before the fragments after them") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# One\n\n* first\n\n![script w:400 h:300](a.as)\n\n* second\n* third\n"},
                {"a.as",
                 "auto r = Rect(100, 50).at(200, 150);\nadd(r);\npause();\nplay(move(r, 50, 50), 1);\npause();\n"
                 "play(fadeOut(r));\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Checked c = check(platform, "deck.md");
        CHECK(c.status == 0);
        // Three fragments and two beats: six steps.
        CHECK(strstr(c.text, "One  (6 steps)") != nullptr);
}

TEST_CASE("Scripts: errors say where, and the region shows them") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# A\n\n![script w:400 h:200](bad.as)\n\n---\n\n![script](none.as)\n\n---\n\n"
                            "![script](../out.as)\n\n---\n\n![script](throws.as)\n\n---\n\n![script](image.as)\n"},
                {"bad.as", "int x = 5\nprint(x);\n"},
                {"throws.as", "wait(1);\nint[] a = {1};\nprint(a[3]);\n"},
                {"image.as", "auto i = Image(\"../secret.png\");\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 4;
        Checked c = check(platform, "deck.md");
        CHECK(c.status == 1);
        CHECK(strstr(c.text, "deck.md:3: error: slide 1: bad.as:2: ") != nullptr);
        CHECK(strstr(c.text, "slide 2: script `none.as` isn't there") != nullptr);
        CHECK(strstr(c.text, "slide 3: script `../out.as` isn't in the deck's folder") != nullptr);
        CHECK(strstr(c.text, "slide 4: throws.as:3: Index out of bounds") != nullptr);
        CHECK(strstr(c.text, "slide 5: image.as:1: Image `../secret.png` isn't in the deck's folder") != nullptr);
}

TEST_CASE("Scripts: a loop that never yields is stopped; one that waits runs on") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# Loops\n\n![script w:400 h:200](spin.as)\n![script w:400 h:200](ticks.as)\n"},
                {"spin.as", "int n = 0;\nwhile (true) n++;\n"},
                {"ticks.as", "auto c = Circle(20).at(50, 50);\nadd(c);\nwhile (true) play(shift(c, 1, 0), 0.5);\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 3;
        platform.realClock = true;
        Checked c = check(platform, "deck.md");
        CHECK(strstr(c.text, "spin.as:2: ran 100 ms without play(), wait() or pause()") != nullptr);
        CHECK(strstr(c.text, "ticks.as") == nullptr);
}

TEST_CASE("Scripts: drawn smooth, deterministic, and stepped by the presenter") {
        // A filled square that moves right over a beat; then a pause, then it turns red.
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# Moves\n\n![script w:400 h:200](move.as)\n\n---\n\n# Next\n"},
                {"move.as", "auto r = Rect(40, 40).at(40, 100);\nr.fill = 0x0000FF;\nr.lineWidth = 0;\nadd(r);\n"
                            "play(move(r, 240, 100).ease(Ease::Linear), 1);\npause();\n"
                            "play(fillTo(r, 0xFF0000), 0.5);\nprintln(millis());\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        const Canvas &screen = platform.last;
        // The region's top: the content box's, under the heading. Found by the square's blue.
        int32_t top = -1;
        for (int32_t y = 0; y < 720 && top < 0; y++) {
                if (screen.colorAt(RegionX + 40, y) == 0x0000FF) top = y - 80;
        }
        REQUIRE(top > 0);
        // At its start, the square at x 20...60 in the region; the region plays.
        CHECK(count(screen, 0x0000FF, Rect{RegionX + 20, top + 80, 40, 40}) == 1600);
        CHECK(platform.animating);
        // Half a second: halfway (x 120...160), only the region's part presented.
        platform.clock += 500 * Ms;
        presenter.frame(platform.clock);
        CHECK(platform.areas == 1);
        CHECK(count(screen, 0x0000FF, Rect{RegionX + 120, top + 80, 40, 40}) == 1600);
        CHECK(count(screen, 0x0000FF, Rect{RegionX + 20, top + 80, 40, 40}) == 0);
        CHECK(platform.lastArea.width < 400);
        // Its end: at the pause, still.
        platform.clock += 600 * Ms;
        presenter.frame(platform.clock);
        CHECK(count(screen, 0x0000FF, Rect{RegionX + 220, top + 80, 40, 40}) == 1600);
        CHECK(!platform.animating);
        // Next: the beat after the pause plays (the fill turning red), then ends.
        presenter.act(Action::Next);
        CHECK(presenter.slideNumber() == 1);
        CHECK(presenter.step() == 1);
        CHECK(platform.animating);
        platform.clock += 600 * Ms;
        presenter.frame(platform.clock);
        CHECK(count(screen, 0xFF0000, Rect{RegionX + 220, top + 80, 40, 40}) == 1600);
        // Its time is its own: 1.5 s played, whatever the clock said.
        bool printed = false;
        for (int32_t x = RegionX; x < RegionX + 100 && !printed; x++) {
                for (int32_t y = top; y < top + 30; y++)
                        printed |= screen.colorAt(x, y) == preso::Theme::paper().body.color;
        }
        CHECK(printed);
        // Previous: back to beat 0 as it ends (fast-forward): the square blue, at the right.
        presenter.act(Action::Previous);
        CHECK(presenter.step() == 0);
        CHECK(!platform.animating);
        CHECK(count(screen, 0x0000FF, Rect{RegionX + 220, top + 80, 40, 40}) == 1600);
        // Next during a beat ends it at once and plays the next.
        presenter.act(Action::Next);
        presenter.act(Action::Previous);
        presenter.act(Action::Next);
        CHECK(presenter.step() == 1);
        platform.clock += 100 * Ms;
        presenter.frame(platform.clock);
        presenter.act(Action::Next); // past its last beat: the next slide
        CHECK(presenter.slideNumber() == 2);
        // Back onto it: its last beat, played.
        presenter.act(Action::Previous);
        CHECK(presenter.slideNumber() == 1);
        CHECK(presenter.step() == 1);
        CHECK(count(screen, 0xFF0000, Rect{RegionX + 220, top + 80, 40, 40}) == 1600);
}

TEST_CASE("Scripts: random numbers are the same each time a region runs") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# R\n\n![script w:200 h:100](r.as)\n![script w:200 h:100](r.as)\n"},
                {"r.as", "auto r = Rect(10, 10).at(random(10, 190), 50);\nr.fill = 0;\nadd(r);\npause();\n"
                         "play(shift(r, 0, random(-30, 30)), 1);\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        preso::Deck deck;
        char       *source = nullptr;
        uint32_t    length = 0;
        REQUIRE(platform.readFile("deck.md", 0, &source, &length) == Error::Ok);
        REQUIRE(deck.parse(source, length) == Error::Ok);
        preso::ThemeSet themes;
        REQUIRE(themes.load(platform, StringView(), "deck.md", nullptr, deck) == Error::Ok);
        preso::FontCache   fonts(platform);
        preso::ImageCache  images(platform, "deck.md");
        preso::ScriptSetup setup;
        setup.platform = &platform;
        setup.deck = &deck;
        setup.themes = &themes;
        setup.fonts = &fonts;
        setup.images = &images;
        setup.deckPath = "deck.md";
        preso::Scripts    *scripts = preso::makeAngelScripts(setup);
        preso::ScriptPlace place;
        place.slide = deck.slide(1);
        place.image = deck.slide(1)->blocks->next->inlines->image;
        place.theme = &themes.base();
        place.width = 200;
        place.height = 100;
        place.index = 0;
        preso::ScriptRegion *a = scripts->open(place);
        place.index = 1;
        preso::ScriptRegion *b = scripts->open(place);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(!a->hasFailed());
        CHECK(a->beats() == 1);
        // Drawn twice from its start, the same; another region of the slide, its own numbers.
        uint32_t pixels[3][200 * 100];
        auto     draw = [&](preso::ScriptRegion *r, uint32_t *out) {
                Canvas c;
                c.attach(out, 800, 200, 100, PixelFormat::Xrgb8888);
                c.fill(Rect{0, 0, 200, 100}, 0xFFFFFF);
                r->seek(1, true);
                r->draw(c, 0, 0);
        };
        draw(a, pixels[0]);
        a->seek(0, true);
        draw(a, pixels[1]);
        draw(b, pixels[2]);
        CHECK(memcmp(pixels[0], pixels[1], sizeof pixels[0]) == 0);
        CHECK(memcmp(pixels[0], pixels[2], sizeof pixels[0]) != 0);
        scripts->~Scripts();
        free(scripts);
}

TEST_CASE("Scripts: a part of a slide drawn again is as the whole slide drawn (effects too)") {
        // Amber's glow reaches round what is drawn; its scanlines are every other row.
        static const ScriptPlatform::File files[] = {
                {"deck.md", "---\ntheme: amber\n---\n\n# Glowing\n\nText under and beside the part drawn again,\n"
                            "on rows of their own.\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 1;
        preso::Deck deck;
        char       *source = nullptr;
        uint32_t    length = 0;
        REQUIRE(platform.readFile("deck.md", 0, &source, &length) == Error::Ok);
        REQUIRE(deck.parse(source, length) == Error::Ok);
        preso::ThemeSet themes;
        REQUIRE(themes.load(platform, deck.settings().theme, "deck.md", nullptr, deck) == Error::Ok);
        REQUIRE(themes.base().glow > 0);
        preso::FontCache    fonts(platform);
        preso::ImageCache   images(platform, "deck.md");
        preso::Layouter     layouter(deck, themes, fonts, platform, &images);
        preso::Arena        arena;
        preso::SlideLayout *layout = layouter.layout(*deck.slide(1), arena, false);
        REQUIRE(layout != nullptr);
        int32_t   w = layouter.width(), h = layouter.height();
        uint32_t *whole = static_cast<uint32_t *>(calloc(size_t(w) * size_t(h), 4));
        uint32_t *again = static_cast<uint32_t *>(calloc(size_t(w) * size_t(h), 4));
        Canvas    a, b;
        a.attach(whole, uint32_t(w) * 4, w, h, PixelFormat::Xrgb8888);
        b.attach(again, uint32_t(w) * 4, w, h, PixelFormat::Xrgb8888);
        preso::drawSlide(*layout, 0, a, fonts, &images);
        preso::drawSlide(*layout, 0, b, fonts, &images);
        // An area at odd places, over the text: drawn again, it is as it was.
        Rect area{101, 157, 333, 91};
        preso::drawSlide(*layout, 0, b, fonts, &images, UINT32_MAX, &area);
        uint32_t differ = 0;
        for (int32_t i = 0; i < w * h; i++) differ += whole[i] != again[i];
        CHECK(differ == 0);
        free(whole);
        free(again);
}

TEST_CASE("Raster: RGB565 is filled and mixed as XRGB8888 is, near enough") {
        uint16_t pixels[32 * 32];
        Canvas   c;
        c.attach(pixels, 64, 32, 32, PixelFormat::Rgb565);
        c.fill(Rect{0, 0, 32, 32}, 0x000000);
        preso::Path path;
        path.addRect(4.5f, 4, 8, 8);
        (void)preso::fillPath(c, path, 0xFFFFFF, 255);
        CHECK(c.colorAt(8, 8) == 0xFFFFFF);
        CHECK(c.colorAt(20, 8) == 0x000000);
        Color edge = c.colorAt(4, 8);
        CHECK(((edge >> 8) & 0xFF) > 0x70);
        CHECK(((edge >> 8) & 0xFF) < 0x90);
        // Half opaque over black: half the colour, inside and at the rim alike.
        c.fill(Rect{0, 0, 32, 32}, 0x000000);
        path.clear();
        path.addEllipse(16, 16, 10, 10);
        (void)preso::fillPath(c, path, 0xFF0000, 128);
        Color middle = c.colorAt(16, 16);
        CHECK(((middle >> 16) & 0xFF) > 0x70);
        CHECK(((middle >> 16) & 0xFF) < 0x90);
        CHECK((middle & 0xFFFF) == 0);
}

TEST_CASE("Scripts: a frame redraws what moved, not what stayed") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# Parts\n\n![script w:1000 h:400](parts.as)\n"},
                {"parts.as", "auto still = Rect(800, 40).at(500, 30);\nstill.fill = 0x00FF00;\nadd(still);\n"
                             "auto dot = Circle(10).at(50, 300);\nadd(dot);\n"
                             "play(move(dot, 950, 300).ease(Ease::Linear), 1);\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        platform.clock += 100 * Ms;
        presenter.frame(platform.clock);
        // The dot's old place and new: a few dozen pixels high, nowhere near the bar at its top.
        REQUIRE(platform.areas == 1);
        CHECK(platform.lastArea.height < 40);
        CHECK(platform.lastArea.width < 200);
}

TEST_CASE("Scripts: under a glow, a region at rest is as the slide drawn afresh") {
        // Moving frames leave the glow out; the region at rest gets it back, exactly.
        static const ScriptPlatform::File files[] = {
                {"deck.md", "---\ntheme: amber\n---\n\n# Glow\n\n![script w:600 h:300](glow.as)\n\nText beside it.\n\n"
                            "---\n\n# After\n"},
                {"glow.as", "auto r = Rect(60, 60).at(50, 150);\nr.fill = theme.color(\"Script.Highlight\");\nadd(r);\n"
                            "play(move(r, 400, 150), 0.5);\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        for (int i = 0; i < 12 && platform.animating; i++) {
                platform.clock += 50 * Ms;
                presenter.frame(platform.clock);
        }
        REQUIRE(!platform.animating);
        int32_t   w = platform.width, h = platform.height;
        uint32_t *before = static_cast<uint32_t *>(malloc(size_t(w) * size_t(h) * 4));
        memcpy(before, platform.screen, size_t(w) * size_t(h) * 4);
        // On and back: the slide drawn whole, its region sought to where its beat ends.
        presenter.act(Action::Next);
        presenter.act(Action::Previous);
        REQUIRE(presenter.slideNumber() == 1);
        uint32_t differ = 0;
        int32_t  x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int32_t i = 0; i < w * h; i++) {
                if (before[i] == platform.screen[i]) continue;
                differ++;
                int32_t x = i % w, y = i / w;
                x0 = x < x0 ? x : x0;
                y0 = y < y0 ? y : y0;
                x1 = x > x1 ? x : x1;
                y1 = y > y1 ? y : y1;
        }
        CAPTURE(x0);
        CAPTURE(y0);
        CAPTURE(x1);
        CAPTURE(y1);
        CHECK(differ == 0);
        free(before);
}


namespace {

        preso::Event keyDown(uint32_t usage, char c = 0) {
                preso::Event e;
                e.type = preso::Event::Type::Key;
                e.key = 0x70000 | usage;
                e.character = uint32_t(c);
                return e;
        }

        preso::Event pointerAt(int32_t x, int32_t y, uint32_t buttons) {
                preso::Event e;
                e.type = preso::Event::Type::Pointer;
                e.x = x;
                e.y = y;
                e.buttons = buttons;
                return e;
        }

        // Where a colour first is on the screen (its top left), or x -1.
        preso::Point find(const Canvas &c, Color color) {
                for (int32_t y = 0; y < c.height(); y++) {
                        for (int32_t x = 0; x < c.width(); x++) {
                                if (c.colorAt(x, y) == color) return preso::Point{float(x), float(y)};
                        }
                }
                return preso::Point{-1, -1};
        }

} // namespace

TEST_CASE("Scripts: an angel run block runs on the step after its code, its output in its pane") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# Squares\n\n```angel run\nfor (int i = 1; i <= 3; i++)\n        println(i * i);\n"
                            "pause();\nprintln(\"done\");\n```\n\n---\n\n# Broken\n\n```angel run below\nint x = 1;\n"
                            "print(y);\n```\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 1;
        Checked c = check(platform, "deck.md");
        // Its gate and its pause: two steps after the first. The broken one's error at the deck's line.
        CHECK(strstr(c.text, "Squares  (3 steps)") != nullptr);
        CHECK(strstr(c.text, "deck.md:16: error: slide 2: line 16: ") != nullptr);
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        const Canvas &screen = platform.last;
        // The pane is empty at first (only the code's own colour on its background, in the code box).
        Color   code = preso::Theme::paper().code.color;
        int32_t before = 0, after = 0;
        for (int32_t y = 0; y < 720; y++) {
                for (int32_t x = 700; x < 1200; x++) before += screen.colorAt(x, y) == code;
        }
        presenter.act(Action::Next);
        CHECK(presenter.step() == 1);
        for (int32_t y = 0; y < 720; y++) {
                for (int32_t x = 700; x < 1200; x++) after += screen.colorAt(x, y) == code;
        }
        CHECK(before == 0);
        CHECK(after > 0);
        // Its three lines one under another, from the pane's top: as tall as three lines.
        int32_t top = 720, bottom = -1;
        for (int32_t y = 0; y < 720; y++) {
                for (int32_t x = 700; x < 1200; x++) {
                        if (screen.colorAt(x, y) != code) continue;
                        top = y < top ? y : top;
                        bottom = y > bottom ? y : bottom;
                }
        }
        preso::FontMetrics m = platform.fontMetrics(platform.openFont("IBM Plex Mono 24"));
        CHECK(bottom - top > 2 * m.lineHeight - m.lineHeight / 2);
        CHECK(bottom - top < 3 * m.lineHeight);
}

TEST_CASE("Scripts: an interactive region takes keys when entered, and gives them back") {
        // A square that moves right on each Right key, and a tick that counts.
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# Keys\n\n![script w:400 h:200](keys.as)\n\n---\n\n# After\n"},
                {"keys.as", "auto r = Rect(20, 20).at(30, 100);\nr.fill = 0x00FF00;\nr.lineWidth = 0;\nadd(r);\n"
                            "int ticks = 0;\nvoid event(Event@ e) {\n        if (e.type == EventType::KeyDown && e.key "
                            "== \"Right\")"
                            "\n                r.x += 50;\n}\nvoid tick(float dt) { ticks++; }\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        CHECK(platform.pointerShown);
        preso::Point at = find(platform.last, 0x00FF00);
        REQUIRE(at.x > 0);
        // Not entered: a key is the deck's (Right isn't a key here: actions come as actions).
        presenter.event(keyDown(0x4F));
        CHECK(find(platform.last, 0x00FF00).x == at.x);
        presenter.act(Action::Into);
        CHECK(presenter.regionEntered());
        CHECK(platform.mode == preso::InputMode::Script);
        CHECK(platform.animating); // its tick() runs
        presenter.event(keyDown(0x4F));
        CHECK(find(platform.last, 0x00FF00).x == at.x + 50);
        // Leave: the keys back, no more ticks.
        presenter.act(Action::Leave);
        CHECK(!presenter.regionEntered());
        CHECK(platform.mode == preso::InputMode::Deck);
        CHECK(!platform.animating);
        // A click in it enters it; Next (a clicker) steps on, leaving it.
        presenter.event(pointerAt(int32_t(at.x) + 100, int32_t(at.y), 1));
        presenter.event(pointerAt(int32_t(at.x) + 100, int32_t(at.y), 0));
        CHECK(presenter.regionEntered());
        presenter.act(Action::Next);
        CHECK(presenter.slideNumber() == 2);
        CHECK(!presenter.regionEntered());
        CHECK(!platform.pointerShown);
}

TEST_CASE("Scripts: widgets change by the pointer and by keys, and call back") {
        // A slider whose callback moves a square; a toggle that shows it; a button that counts.
        static const ScriptPlatform::File files[] = {
                {"deck.md", "<!-- interactive -->\n\n# Widgets\n\n![script w:600 h:300](w.as)\n"},
                {"w.as", "auto s = Slider(0, 100, 0, 300).at(200, 50);\nadd(s);\n"
                         "auto t = Toggle(\"Show\", true).at(200, 120);\nadd(t);\n"
                         "auto r = Rect(20, 20).at(50, 250);\nr.fill = 0x00FF00;\nr.lineWidth = 0;\nadd(r);\n"
                         "s.onChange(function() { r.x = 50 + s.value * 4; });\n"
                         "t.onChange(function() { r.visible = t.on; });\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        // `interactive`: entered as it appears; the slider focused first.
        CHECK(presenter.regionEntered());
        preso::Point start = find(platform.last, 0x00FF00);
        REQUIRE(start.x > 0);
        // Right on the focused slider: 5 more (a twentieth), the square 20 pixels on.
        presenter.event(keyDown(0x4F));
        CHECK(find(platform.last, 0x00FF00).x == start.x + 20);
        // Dragged to its right end: the square at 50 + 400.
        int32_t rx = int32_t(start.x) - 40, ry = int32_t(start.y) - 240; // the region's top left
        presenter.event(pointerAt(rx + 200, ry + 50, 1));
        presenter.event(pointerAt(rx + 400, ry + 50, 1));
        presenter.event(pointerAt(rx + 400, ry + 50, 0));
        CHECK(find(platform.last, 0x00FF00).x == float(rx + 450 - 10));
        // Tab to the toggle, Space: the square hidden.
        presenter.event(keyDown(0x2B));
        presenter.event(keyDown(0x2C, ' '));
        CHECK(find(platform.last, 0x00FF00).x < 0);
}

TEST_CASE("Scripts: sounds are heard when their slide is shown, never in fast-forward") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# One\n\n---\n\n<!-- sound: ding.wav -->\n\n# Two\n\n![script w:200 h:100](s.as)\n"},
                {"s.as", "auto d = Sound(\"ding.wav\");\nd.play();\npause();\nd.play();\n"},
                {"ding.wav", "RIFF"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 3;
        Checked c = check(platform, "deck.md");
        CHECK(c.status == 0);
        CHECK(platform.soundsPlayed == 0); // checking plays nothing
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        // Slide 2 is drawn ahead (its script run to its first pause): nothing heard yet.
        CHECK(platform.soundsPlayed == 0);
        presenter.act(Action::Next);
        // Shown: the slide's sound and the script's first.
        CHECK(platform.soundsPlayed == 2);
        presenter.act(Action::Next);
        CHECK(platform.soundsPlayed == 3);
        // Back and forth: Previous re-runs in fast-forward, silent.
        presenter.act(Action::Previous);
        CHECK(platform.soundsPlayed == 3);
}

TEST_CASE("Scripts: --trust gives files and the system; check says a script needs it") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# Live\n\n![script w:200 h:100](live.as)\n"},
                {"live.as", "println(fileExists(\"/\"));\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Checked c = check(platform, "deck.md");
        CHECK(c.status == 1);
        CHECK(strstr(c.text, "live.as:1: needs --trust: ") != nullptr);
        // Trusted, it builds and runs.
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts, true);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        CHECK(presenter.deck()->diagnosticCount() == 0);
}

TEST_CASE("Presenting: a deck that runs programs asks first, unless trusted") {
        static const ScriptPlatform::File files[] = {
                {"deck.md", "# Demo\n\n---\n\n<!-- run: /bin/luckyduck --deal -->\n\n# Cards\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 1;
        {
                Presenter presenter(platform, "deck.md", nullptr);
                REQUIRE(presenter.load() == Error::Ok);
                REQUIRE(presenter.run(1) == Error::Ok);
                CHECK(presenter.asking());
                CHECK(!presenter.demosAllowed());
                presenter.act(Action::Next); // not an answer
                CHECK(presenter.asking());
                presenter.event(keyDown(0x1C, 'y'));
                CHECK(!presenter.asking());
                CHECK(presenter.demosAllowed());
                CHECK(presenter.slideNumber() == 1);
        }
        {
                Presenter presenter(platform, "deck.md", nullptr, nullptr, true);
                REQUIRE(presenter.load() == Error::Ok);
                REQUIRE(presenter.run(1) == Error::Ok);
                CHECK(!presenter.asking());
                CHECK(presenter.demosAllowed());
        }
}

TEST_CASE("Scripts: reading the deck again lets go of the region that had the keys") {
        // The entered region is the old deck's: reloading must not touch it after it is gone.
        static const ScriptPlatform::File files[] = {
                {"deck.md", "<!-- interactive -->\n\n# Keys\n\n![script w:400 h:200](k.as)\n"},
                {"k.as", "void event(Event@ e) {}\n"},
        };
        ScriptPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Presenter presenter(platform, "deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        REQUIRE(presenter.regionEntered());
        REQUIRE(presenter.reload());
        // Entered again (the slide is interactive), its new region.
        CHECK(presenter.regionEntered());
        presenter.event(keyDown(0x04, 'a'));
        presenter.act(Action::Leave);
        CHECK(!presenter.regionEntered());
}
