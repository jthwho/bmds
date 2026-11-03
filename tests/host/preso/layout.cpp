/**
 * @file      layout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Laying out and drawing slides, and presenting them (devplan/preso.md §6), on the headless
 * platform's stand-in fonts (each character half its size wide).
 */

#include <doctest.h>

#include <stdlib.h>
#include <string.h>
#include "core/layout.h"
#include "core/presenter.h"
#include "headless/headless.h"

using preso::Action;
using preso::Arena;
using preso::Canvas;
using preso::Deck;
using preso::DrawOp;
using preso::Error;
using preso::FontCache;
using preso::Layouter;
using preso::OpType;
using preso::PixelFormat;
using preso::Presenter;
using preso::SlideLayout;
using preso::StringView;
using preso::Theme;

namespace {

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

        // A deck laid out in paper on the stand-in fonts.
        struct Laid {
                        preso::HeadlessPlatform platform;
                        FontCache               fonts{platform};
                        Deck                    deck;
                        Arena                   arena;
                        preso::ThemeSet         themes;
                        Layouter               *layouter = nullptr;

                        explicit Laid(const char *text) {
                                REQUIRE(parse(deck, text) == Error::Ok);
                                REQUIRE(themes.load(platform, deck.settings().theme, "talk.md", nullptr, deck) ==
                                        Error::Ok);
                                layouter = new Layouter(deck, themes, fonts, platform, nullptr);
                        }
                        ~Laid() { delete layouter; }

                        SlideLayout *slide(uint32_t n) { return layouter->layout(*deck.slide(n), arena, true); }
        };

        const DrawOp *textOp(const SlideLayout *l, const char *text) {
                for (const DrawOp *o = l->ops; o != nullptr; o = o->next) {
                        if (o->type == OpType::Text && o->text.equals(StringView(text))) return o;
                }
                return nullptr;
        }

        uint32_t count(const SlideLayout *l, OpType type) {
                uint32_t n = 0;
                for (const DrawOp *o = l->ops; o != nullptr; o = o->next) n += o->type == type;
                return n;
        }

        // A platform with a screen that only counts what it is shown, and the deck in memory.
        class TestPlatform : public preso::HeadlessPlatform {
                public:
                        const char *text = "";
                        bool        readable = true;
                        uint32_t    presents = 0;
                        int32_t     width = 0;

                        Error readFile(const char *, uint32_t, char **data, uint32_t *length) override {
                                if (!readable) return Error::NotFound;
                                size_t n = strlen(text);
                                *data = static_cast<char *>(malloc(n + 1));
                                memcpy(*data, text, n + 1);
                                *length = uint32_t(n);
                                return Error::Ok;
                        }
                        PixelFormat pixelFormat() const override { return PixelFormat::Xrgb8888; }
                        Error       openScreen(int32_t w, int32_t, const char *, bool) override {
                                width = w;
                                return Error::Ok;
                        }
                        void  present(const Canvas &) override { presents++; }
                        Error watch(const char *) override { return Error::Ok; }
                        Error run(preso::App &) override { return Error::Ok; }
        };

} // namespace

TEST_CASE("Layout: a paragraph breaks into lines inside the content's box") {
        // 30 px text, 15 px a character: the box (1280 - 160) holds 74 characters a line.
        Laid         l("# Title\n\n"
                               "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        const DrawOp *title = textOp(s, "Title");
        REQUIRE(title != nullptr);
        CHECK(title->font.size == 60);
        CHECK(title->x == 80);
        CHECK(title->y >= 56);
        // The paragraph's words, merged into runs a line each: two lines.
        uint32_t lines = 0;
        int32_t  lastY = -1;
        for (const DrawOp *o = s->ops; o != nullptr; o = o->next) {
                if (o->type != OpType::Text || o == title) continue;
                CHECK(o->x >= 80);
                CHECK(o->x + o->width <= 1280 - 80);
                if (o->y != lastY) lines++;
                lastY = o->y;
        }
        CHECK(lines == 2);
        CHECK(!s->overflows);
        CHECK(l.deck.diagnosticCount() == 0);
}

TEST_CASE("Layout: a word wider than the line is broken at characters") {
        char text[200];
        memset(text, 'x', 150);
        text[150] = '\n';
        text[151] = '\0';
        Laid         l(text);
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        CHECK(count(s, OpType::Text) == 3); // 74 + 74 + 2
        for (const DrawOp *o = s->ops; o != nullptr; o = o->next) {
                if (o->type == OpType::Text) CHECK(o->x + o->width <= 1280 - 80);
        }
}

TEST_CASE("Layout: inline styles choose fonts; links and strikes are lined; code is boxed") {
        Laid         l("plain **bold** *it* [link](x) ~~gone~~ `code`\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        CHECK(textOp(s, "bold")->font.style == preso::FontKey::Bold);
        CHECK(textOp(s, "it")->font.style == preso::FontKey::Italic);
        CHECK(textOp(s, "link")->color == Theme::paper().link);
        const DrawOp *code = textOp(s, "code");
        REQUIRE(code != nullptr);
        CHECK(strcmp(code->font.family, "IBM Plex Mono") == 0);
        // Fills: the link's underline, the strike, the code's box.
        CHECK(count(s, OpType::Fill) == 3);
}

TEST_CASE("Layout: a link's underline runs over its spaces; code's box is part of its width") {
        Laid         l("[two words](x) `c`, next\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        const DrawOp *two = textOp(s, "two"), *words = textOp(s, "words");
        REQUIRE(two != nullptr);
        REQUIRE(words != nullptr);
        uint32_t lines = 0;
        for (const DrawOp *o = s->ops; o != nullptr; o = o->next) {
                if (o->type != OpType::Fill || o->color != Theme::paper().link) continue;
                lines++;
                CHECK(o->x == two->x);
                CHECK(o->x + o->width == words->x + words->width);
        }
        CHECK(lines == 1);
        // The box (code at 27 px: 5 px of padding a side) ends before the comma.
        const DrawOp *c = textOp(s, "c"), *comma = textOp(s, ", next"); // the comma, the word after it merged
        REQUIRE(c != nullptr);
        REQUIRE(comma != nullptr);
        CHECK(comma->x >= c->x + c->width + 5);
}

TEST_CASE("Layout: fragments are drawn from their step on") {
        Laid         l("* one\n* two\n\n- always\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        CHECK(textOp(s, "one")->fragment == 1);
        CHECK(textOp(s, "two")->fragment == 2);
        CHECK(textOp(s, "always")->fragment == 0);
        // Drawn: at step 0 the first item's line is blank, at step 1 it isn't.
        int32_t  w = l.layouter->width(), h = l.layouter->height();
        uint8_t *pixels = static_cast<uint8_t *>(calloc(size_t(w) * size_t(h), 4));
        Canvas   canvas;
        canvas.attach(pixels, uint32_t(w) * 4, w, h, PixelFormat::Xrgb8888);
        const DrawOp *one = textOp(s, "one");
        auto          inked = [&]() {
                for (int32_t x = one->x; x < one->x + one->width; x++) {
                        for (int32_t y = one->y; y < one->y + one->height; y++) {
                                if (canvas.colorAt(x, y) != Theme::paper().background) return true;
                        }
                }
                return false;
        };
        preso::drawSlide(*s, 0, canvas, l.fonts, nullptr);
        CHECK(!inked());
        preso::drawSlide(*s, 1, canvas, l.fonts, nullptr);
        CHECK(inked());
        free(pixels);
}

TEST_CASE("Layout: lists' markers, numbers and nesting") {
        Laid         l("3. three\n4. four\n   - inner\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        const DrawOp *three = textOp(s, "3.");
        const DrawOp *four = textOp(s, "4.");
        REQUIRE(three != nullptr);
        REQUIRE(four != nullptr);
        CHECK(three->x + three->width < textOp(s, "three")->x);
        CHECK(four->y > three->y);
        const DrawOp *inner = textOp(s, "inner");
        REQUIRE(inner != nullptr);
        CHECK(inner->x > textOp(s, "four")->x);
        CHECK(textOp(s, "\xE2\x80\x93") != nullptr); // the second level's bullet, an en dash
}

TEST_CASE("Layout: fit scales a heading to the line") {
        Laid         l("# <!-- fit --> Big\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        const DrawOp *big = textOp(s, "Big");
        REQUIRE(big != nullptr);
        // 3 characters of half the size: as wide as the box would be 746 px; at most 255.
        CHECK(big->font.size == 255);
}

TEST_CASE("Layout: a title slide's content is in the middle, its lines centred") {
        Laid         l("<!-- _class: title -->\n# Hi\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        const DrawOp *hi = textOp(s, "Hi");
        REQUIRE(hi != nullptr);
        CHECK(hi->x == 80 + (1120 - 60) / 2);
        CHECK(hi->y > 250);
}

TEST_CASE("Layout: code blocks are boxed, coloured lines with tabs to stops") {
        // (md4c expands a tab in a line's indentation itself: the tab here is mid-line.)
        Laid         l("```\nint a;\nx\tb;\n```\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        const DrawOp *box = s->ops;
        REQUIRE(box->type == OpType::Fill);
        CHECK(box->color == Theme::paper().codeBackground);
        const DrawOp *a = textOp(s, "int a;");
        const DrawOp *b = textOp(s, "b;");
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(b->x == a->x + 4 * 12); // x, then a tab to the stop four of the code's 12 px characters in
        CHECK(b->y > a->y);
        CHECK(box->height == 2 * (24 * 5 / 4) + 2 * Theme::paper().codePadding);
}

TEST_CASE("Layout: tables share out the width; the header is bold on its own background") {
        Laid         l("| Name | Value |\n|---|--:|\n| a | 1 |\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        CHECK(textOp(s, "Name")->font.style == preso::FontKey::Bold);
        CHECK(textOp(s, "a")->font.style == 0);
        // The right-aligned column's 1 ends where its header's text would.
        const DrawOp *value = textOp(s, "Value"), *one = textOp(s, "1");
        CHECK(one->x + one->width == value->x + value->width);
        CHECK(one->y > value->y);
}

TEST_CASE("Layout: too much for the slide is reported") {
        char text[4096] = {};
        for (int i = 0; i < 30; i++) strcat(text, "line\n\n");
        Laid         l(text);
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        CHECK(s->overflows);
        CHECK(reported(l.deck, "past the bottom margin"));
}

TEST_CASE("Layout: a split background narrows the content; header, footer and page number") {
        Laid         l("---\npaginate: true\nfooter: Foot\n---\n![bg right:40%](a.png)\n\nword\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        bool framed = false;
        for (const DrawOp *o = s->ops; o != nullptr; o = o->next) {
                if (o->type == OpType::Frame && o->x == 1280 - 512 && o->width == 512) framed = true;
        }
        CHECK(framed);
        CHECK(textOp(s, "word")->x == 80);
        CHECK(textOp(s, "Foot") != nullptr);
        const DrawOp *page = textOp(s, "1");
        REQUIRE(page != nullptr);
        CHECK(page->x + page->width == 1280 - 512 - 80); // on the text's side, not over the picture
}

TEST_CASE("Layout: a 4:3 deck, and colours from directives") {
        Laid l("---\nsize: 4:3\n---\n<!-- _backgroundColor: navy -->\n<!-- _color: \"#fff\" -->\n# A\n\n---\n"
               "<!-- _color: nope -->\nB\n");
        CHECK(l.layouter->width() == 960);
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        CHECK(s->background == 0x000080);
        CHECK(textOp(s, "A")->color == 0xFFFFFF);
        REQUIRE(l.slide(2) != nullptr);
        CHECK(reported(l.deck, "color `nope`"));
}

TEST_CASE("Presenter: steps through fragments and slides, and back") {
        TestPlatform platform;
        platform.text = "# One\n\n---\n\n# Two\n\n* a\n* b\n\n---\n\n# Three\n";
        Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        CHECK(platform.width == 1280);
        CHECK(platform.presents == 1);
        struct Step {
                        Action   action;
                        uint32_t slide, step;
        };
        const Step steps[] = {{Action::Next, 2, 0},     {Action::Next, 2, 1},  {Action::Next, 2, 2},
                              {Action::Next, 3, 0},     {Action::Next, 3, 0},  {Action::Previous, 2, 2},
                              {Action::Previous, 2, 1}, {Action::First, 1, 0}, {Action::Previous, 1, 0},
                              {Action::Last, 3, 0}};
        uint32_t   presents = 1;
        for (const Step &s : steps) {
                uint32_t slide = presenter.slideNumber(), step = presenter.step();
                presenter.act(s.action);
                CHECK(presenter.slideNumber() == s.slide);
                CHECK(presenter.step() == s.step);
                // Shown again only when something moved.
                if (s.slide != slide || s.step != step) presents++;
                CHECK(platform.presents == presents);
        }
        CHECK(platform.openFonts() <= FontCache::MaxOpen);
}

TEST_CASE("Presenter: a reload keeps the slide, by its title when slides move") {
        TestPlatform platform;
        platform.text = "# A\n\n---\n\n# B\n\n---\n\n# C\n";
        Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(3) == Error::Ok);
        CHECK(presenter.slideNumber() == 3);
        // A slide put in front: C is now the fourth.
        platform.text = "# New\n\n---\n\n# A\n\n---\n\n# B\n\n---\n\n# C\n";
        CHECK(presenter.reload());
        CHECK(presenter.slideNumber() == 4);
        // Retitled: the same number.
        platform.text = "# New\n\n---\n\n# A\n\n---\n\n# B\n\n---\n\n# D\n";
        CHECK(presenter.reload());
        CHECK(presenter.slideNumber() == 4);
        // Shorter: the last.
        platform.text = "# Only\n";
        CHECK(presenter.reload());
        CHECK(presenter.slideNumber() == 1);
        // Unreadable: the deck as it was, said so.
        platform.readable = false;
        CHECK(!presenter.reload());
        CHECK(presenter.deck()->slideCount() == 1);
}
