/**
 * @file      themes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Preso's themes (devplan/preso.md §5, step 4): the INI format, the built-in themes, class
 * variants, where theme files are found, their decorations in layout, and the effects.
 */

#include <doctest.h>

#include <stdlib.h>
#include <string.h>
#include "core/effects.h"
#include "core/layout.h"
#include "core/presenter.h"
#include "core/theme.h"
#include "headless/headless.h"

using preso::Arena;
using preso::Canvas;
using preso::Color;
using preso::Deck;
using preso::DrawOp;
using preso::Error;
using preso::FontCache;
using preso::FontKey;
using preso::Frame;
using preso::Layouter;
using preso::OpType;
using preso::PixelFormat;
using preso::SlideLayout;
using preso::StringView;
using preso::Theme;
using preso::ThemeSet;
using preso::Underline;

namespace {

        // A platform whose files are given: a path, its text.
        class FilePlatform : public preso::HeadlessPlatform {
                public:
                        struct File {
                                        const char *path;
                                        const char *text;
                        };
                        const File *files = nullptr;
                        uint32_t    count = 0;
                        bool        sharp = false;
                        int32_t     width = 0, height = 0;

                        Error readFile(const char *path, uint32_t, char **data, uint32_t *length) override {
                                for (uint32_t i = 0; i < count; i++) {
                                        if (strcmp(files[i].path, path) != 0) continue;
                                        size_t n = strlen(files[i].text);
                                        *data = static_cast<char *>(malloc(n + 1));
                                        memcpy(*data, files[i].text, n + 1);
                                        *length = uint32_t(n);
                                        return Error::Ok;
                                }
                                return Error::NotFound;
                        }
                        const char *dataFolder() override { return "/data"; }
                        Error       openScreen(int32_t w, int32_t h, const char *, bool s) override {
                                width = w;
                                height = h;
                                sharp = s;
                                return Error::Ok;
                        }
                        Error watch(const char *) override { return Error::Ok; }
                        Error run(preso::App &) override { return Error::Ok; }
        };

        Error parse(Deck &deck, const char *text) {
                size_t n = strlen(text);
                char  *copy = static_cast<char *>(malloc(n + 1));
                memcpy(copy, text, n + 1);
                return deck.parse(copy, uint32_t(n));
        }

        bool reported(const Deck &deck, const char *part) {
                for (const preso::Diagnostic *d = deck.diagnostics(); d != nullptr; d = d->next) {
                        if (d->message.indexOf(StringView(part)) >= 0) return true;
                }
                return false;
        }

        void printDiagnostics(const Deck &deck) {
                for (const preso::Diagnostic *d = deck.diagnostics(); d != nullptr; d = d->next) {
                        MESSAGE(d->message.data()); // arena copies: NUL-terminated
                }
        }

        struct Laid {
                        FilePlatform platform;
                        FontCache    fonts{platform};
                        Deck         deck;
                        Arena        arena;
                        ThemeSet     themes;
                        Layouter    *layouter = nullptr;

                        explicit Laid(const char *text) {
                                REQUIRE(parse(deck, text) == Error::Ok);
                                REQUIRE(themes.load(platform, deck.settings().theme, "talk.md", nullptr, deck) ==
                                        Error::Ok);
                                layouter = new Layouter(deck, themes, fonts, platform, nullptr);
                        }
                        ~Laid() { delete layouter; }
                        SlideLayout *slide(uint32_t n) { return layouter->layout(*deck.slide(n), arena, true); }
        };

        uint32_t count(const SlideLayout *l, OpType type) {
                uint32_t n = 0;
                for (const DrawOp *o = l->ops; o != nullptr; o = o->next) n += o->type == type;
                return n;
        }

        const DrawOp *textOp(const SlideLayout *l, const char *text) {
                for (const DrawOp *o = l->ops; o != nullptr; o = o->next) {
                        if (o->type == OpType::Text && o->text.equals(StringView(text))) return o;
                }
                return nullptr;
        }

} // namespace

TEST_CASE("Themes: fonts as themes write them") {
        Arena   arena;
        FontKey f;
        CHECK(preso::parseFont("unscii 32", arena, &f));
        CHECK(strcmp(f.family, "unscii") == 0);
        CHECK(f.size == 32);
        CHECK(f.style == 0);
        CHECK(preso::parseFont("IBM Plex Sans 30 bold italic", arena, &f));
        CHECK(strcmp(f.family, "IBM Plex Sans") == 0);
        CHECK(f.size == 30);
        CHECK(f.style == (FontKey::Bold | FontKey::Italic));
        // A family alone keeps the size; a size alone keeps the family.
        CHECK(preso::parseFont("IBM Plex Mono", arena, &f));
        CHECK(strcmp(f.family, "IBM Plex Mono") == 0);
        CHECK(f.size == 30);
        CHECK(preso::parseFont("20 regular", arena, &f));
        CHECK(strcmp(f.family, "IBM Plex Mono") == 0);
        CHECK(f.size == 20);
        CHECK(f.style == 0);
        CHECK(!preso::parseFont("unscii 999", arena, &f));
        CHECK(!preso::parseFont("", arena, &f));
}

TEST_CASE("Themes: every built-in theme reads without a complaint") {
        for (const char *const *name = ThemeSet::builtInNames(); *name != nullptr; name++) {
                FilePlatform platform;
                Deck         deck;
                REQUIRE(parse(deck, "# A\n") == Error::Ok);
                ThemeSet themes;
                REQUIRE(themes.load(platform, StringView(*name), "talk.md", nullptr, deck) == Error::Ok);
                INFO(*name);
                printDiagnostics(deck);
                CHECK(deck.diagnosticCount() == 0);
        }
}

TEST_CASE("Themes: vga is small, sharp, EGA-coloured and framed") {
        FilePlatform platform;
        Deck         deck;
        REQUIRE(parse(deck, "# A\n") == Error::Ok);
        ThemeSet themes;
        REQUIRE(themes.load(platform, "vga", "talk.md", nullptr, deck) == Error::Ok);
        const Theme &t = themes.base();
        CHECK(t.width == 640);
        CHECK(t.height == 360);
        CHECK(t.nearest);
        CHECK(t.palette.count == 16);
        CHECK(t.background == 0x0000AA);
        CHECK(strcmp(t.body.font.family, "unscii") == 0);
        CHECK(t.body.font.size == 16);
        CHECK(t.heading[0].frame == Frame::Double);
        CHECK(t.heading[1].underline == Underline::Line);
        CHECK(t.headerBackground == 0xAAAAAA);
        CHECK(t.syntax[1] == 0xFFFF55);
        // Its title class: in the middle, centred, bigger.
        const Theme &title = themes.forClasses("title");
        CHECK(title.middle);
        CHECK(title.center);
        CHECK(title.heading[0].text.font.size == 48);
        CHECK(&themes.forClasses("nothing") == &t);
        CHECK(&themes.forClasses("") == &t);
}

TEST_CASE("Themes: green extends amber: its effects and fonts, its own colours") {
        FilePlatform platform;
        Deck         deck;
        REQUIRE(parse(deck, "# A\n") == Error::Ok);
        ThemeSet themes;
        REQUIRE(themes.load(platform, "green", "talk.md", nullptr, deck) == Error::Ok);
        const Theme &t = themes.base();
        CHECK(t.background == 0x001200);
        CHECK(t.body.color == 0x33FF33);
        CHECK(t.body.font.size == 32);
        CHECK(t.scanlines > 0);
        CHECK(t.palette.count == 8);
        CHECK(themes.forClasses("title").heading[0].text.font.size == 96); // amber's class, inherited
}

TEST_CASE("Themes: a theme file beside the deck, over a built-in one, its mistakes reported") {
        static const FilePlatform::File files[] = {
                {"talks/mine.theme", "[Theme]\nName=Mine\nExtends=vga\nScale=wobbly\n"
                                     "[Slide]\nBackground=#102030 ; a comment\nPattern=tile.png\nMargin=10 20\n"
                                     "[H1]\nFrame=sunken\nColour=#fff\n"
                                     "[Nonsense]\nA=1\n"
                                     "[Class big]\nH1.Font=unscii 64\nCenter=true\nH2.Wrong=1\n"
                                     "loose line\n"},
                {"/data/themes/shared.theme", "[Slide]\nBackground=#010203\nPattern=dots.png\n"},
                {"talks/themes/sub.theme", "[Slide]\nPattern=p.png\n"},
        };
        FilePlatform platform;
        platform.files = files;
        platform.count = 3;
        Deck deck;
        REQUIRE(parse(deck, "# A\n") == Error::Ok);
        ThemeSet themes;
        REQUIRE(themes.load(platform, "mine", "talks/talk.md", "/data", deck) == Error::Ok);
        const Theme &t = themes.base();
        CHECK(strcmp(t.name, "Mine") == 0);
        CHECK(t.width == 640);           // from vga
        CHECK(t.background == 0x102030); // its own
        CHECK(t.heading[0].frame == Frame::Sunken);
        CHECK(t.marginTop == 10);
        CHECK(t.marginRight == 20);
        CHECK(t.marginBottom == 10);
        CHECK(t.marginLeft == 20);
        CHECK(strcmp(t.pattern, "tile.png") == 0); // the theme is in the deck's folder
        CHECK(reported(deck, "talks/mine.theme:4: key `Scale` is smooth or nearest"));
        CHECK(reported(deck, "talks/mine.theme:11: key `Colour` isn't a key of its section"));
        CHECK(reported(deck, "talks/mine.theme:13: key `A` is in a section themes don't have"));
        CHECK(reported(deck, "talks/mine.theme:17: key `Wrong`"));
        CHECK(reported(deck, "talks/mine.theme:18: line `loose line` isn't KEY=VALUE"));
        CHECK(themes.forClasses("big").heading[0].text.font.size == 64);
        CHECK(themes.forClasses("big").center);
        // In the data folder: its pattern named from there.
        Deck deck2;
        REQUIRE(parse(deck2, "# A\n") == Error::Ok);
        ThemeSet shared;
        REQUIRE(shared.load(platform, "shared", "talks/talk.md", "/data", deck2) == Error::Ok);
        CHECK(shared.base().background == 0x010203);
        CHECK(strcmp(shared.base().pattern, "/data/themes/dots.png") == 0);
        CHECK(deck2.diagnosticCount() == 0);
        // A path from the deck's folder.
        Deck deck3;
        REQUIRE(parse(deck3, "# A\n") == Error::Ok);
        ThemeSet sub;
        REQUIRE(sub.load(platform, "themes/sub.theme", "talks/talk.md", "/data", deck3) == Error::Ok);
        CHECK(strcmp(sub.base().pattern, "themes/p.png") == 0);
        // Unknown: paper, said.
        Deck deck4;
        REQUIRE(parse(deck4, "# A\n") == Error::Ok);
        ThemeSet none;
        REQUIRE(none.load(platform, "gaia", "talks/talk.md", "/data", deck4) == Error::Ok);
        CHECK(reported(deck4, "theme `gaia` isn't known"));
        CHECK(none.base().background == Theme::paper().background);
}

TEST_CASE("Themes: palettes by name or as colours") {
        ThemeSet themes;
        REQUIRE(themes.loadText("[Theme]\nPalette=#000000 #ff0000 #00ff00\n", "t.theme", nullptr) == Error::Ok);
        CHECK(themes.base().palette.count == 3);
        CHECK(themes.base().palette.colors[1] == 0xFF0000);
        ThemeSet cga;
        REQUIRE(cga.loadText("[Theme]\nPalette=cga\n", "t.theme", nullptr) == Error::Ok);
        CHECK(cga.base().palette.count == 4);
}

TEST_CASE("Layout: a theme's decorations: frames, bands, title bars, dividers, status lines") {
        Laid         vga("---\ntheme: vga\nfooter: F\n---\n# Framed\n\n## Lined\n");
        SlideLayout *s = vga.slide(1);
        REQUIRE(s != nullptr);
        CHECK(s->background == 0x0000AA);
        // A double frame: two outlines; the underline: a fill; the status line: a fill across.
        CHECK(count(s, OpType::Frame) == 2);
        bool statusLine = false;
        for (const DrawOp *o = s->ops; o != nullptr; o = o->next) {
                if (o->type == OpType::Fill && o->x == 0 && o->width == 640 && o->color == 0xAAAAAA) statusLine = true;
        }
        CHECK(statusLine);
        Laid         bbs("---\ntheme: bbs\n---\n# Shaded\n");
        SlideLayout *b = bbs.slide(1);
        REQUIRE(b != nullptr);
        bool shade = false;
        for (const DrawOp *o = b->ops; o != nullptr; o = o->next) {
                if (o->type == OpType::Text && o->text.startsWith("\xE2\x96\x88") && o->text.endsWith("\xE2\x96\x91"))
                        shade = true;
        }
        CHECK(shade);
        Laid         classic("---\ntheme: classic\n---\n# Title bar\n");
        SlideLayout *c = classic.slide(1);
        REQUIRE(c != nullptr);
        // The bar is behind the title: before its text in the list, across the slide.
        const DrawOp *bar = nullptr;
        for (const DrawOp *o = c->ops; o != nullptr; o = o->next) {
                if (o->type == OpType::Fill && o->color == 0x000080 && o->width == 1280) {
                        bar = o;
                        break;
                }
                if (o->type == OpType::Text) break;
        }
        REQUIRE(bar != nullptr);
        const DrawOp *title = textOp(c, "Title bar");
        REQUIRE(title != nullptr);
        CHECK(title->color == 0xFFFFFF);
        CHECK(title->y >= bar->y);
        CHECK(title->y + title->height <= bar->y + bar->height);
        // The raised window round the slide: light top-left, dark bottom-right.
        bool light = false, dark = false;
        for (const DrawOp *o = c->ops; o != nullptr; o = o->next) {
                if (o->type == OpType::Fill && o->x == 0 && o->y == 0 && o->width == 1280 && o->color == 0xFFFFFF)
                        light = true;
                if (o->type == OpType::Fill && o->y == 718 && o->width == 1280 && o->color == 0x404040) dark = true;
        }
        CHECK(light);
        CHECK(dark);
        // Classic's title class undoes the bar.
        Laid         classicTitle("---\ntheme: classic\n---\n<!-- _class: title -->\n# Big\n");
        SlideLayout *ct = classicTitle.slide(1);
        CHECK(textOp(ct, "Big")->color == 0x000080);
        CHECK(textOp(ct, "Big")->font.size == 60);
}

TEST_CASE("Effects: scanlines darken every other row") {
        static uint32_t pixels[64 * 64];
        Canvas          c;
        c.attach(pixels, 64 * 4, 64, 64, PixelFormat::Xrgb8888);
        c.fill(preso::Rect{0, 0, 64, 64}, 0x808080);
        preso::applyScanlines(c, 128);
        CHECK(c.colorAt(5, 0) == 0x808080);
        CHECK(((c.colorAt(5, 1) >> 8) & 0xFF) < 0x50);
}

TEST_CASE("Presenter: a sharp theme opens a sharp screen at its size") {
        static const FilePlatform::File files[] = {{"talk.md", "---\ntheme: vga\n---\n# A\n"}};
        FilePlatform                    platform;
        platform.files = files;
        platform.count = 1;
        preso::Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        CHECK(platform.sharp);
        CHECK(platform.width == 640);
        CHECK(platform.height == 360);
}
