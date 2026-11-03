/**
 * @file      deck.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The deck parser (devplan/preso.md §4): slides, directives, notes, images' keywords, fragments,
 * script regions and `angel run` blocks, and what `preso check` reports.
 */

#include <doctest.h>

#include <stdlib.h>
#include <string.h>
#include "core/deck.h"

using preso::Block;
namespace BlockFlag = preso::BlockFlag;
using preso::BlockType;
using preso::Deck;
using preso::Diagnostic;
using preso::Error;
using preso::Image;
using preso::ImageFit;
namespace ImageFlag = preso::ImageFlag;
using preso::ImageSide;
using preso::Inline;
namespace InlineStyle = preso::InlineStyle;
using preso::InlineType;
using preso::Slide;
using preso::StringView;

namespace {

        Error parse(Deck &deck, const char *text) {
                size_t n = strlen(text);
                char  *copy = static_cast<char *>(malloc(n + 1));
                memcpy(copy, text, n + 1);
                return deck.parse(copy, uint32_t(n));
        }

        bool is(StringView view, const char *text) {
                return view.equals(StringView(text));
        }

        // Every diagnostic's message, one a line, for checking what was reported.
        bool reported(const Deck &deck, const char *part, uint32_t line = 0) {
                for (const Diagnostic *d = deck.diagnostics(); d != nullptr; d = d->next) {
                        if (d->message.indexOf(StringView(part)) >= 0 && (line == 0 || d->line == line)) return true;
                }
                return false;
        }

        // The text of a block's inlines, soft breaks as spaces.
        void text(const Block *b, char *out, size_t size) {
                size_t n = 0;
                out[0] = '\0';
                for (const Inline *i = b->inlines; i != nullptr; i = i->next) {
                        StringView t = i->type == InlineType::SoftBreak   ? StringView(" ")
                                       : i->type == InlineType::LineBreak ? StringView("|")
                                                                          : i->text;
                        for (uint32_t k = 0; k < t.length() && n + 1 < size; k++) out[n++] = t[k];
                }
                out[n] = '\0';
                return;
        }

        uint32_t count(const Block *b) {
                uint32_t n = 0;
                for (; b != nullptr; b = b->next) n++;
                return n;
        }

} // namespace

TEST_CASE("Deck: slides are split by --- and the front matter is the deck's") {
        Deck deck;
        REQUIRE(parse(deck, "---\n"
                            "theme: vga\n"
                            "paginate: true\n"
                            "footer: \"BMDS · 2026\"\n"
                            "duration: 25m\n"
                            "---\n"
                            "\n"
                            "# BMDS\n"
                            "\n"
                            "---\n"
                            "\n"
                            "# Second\n"
                            "\n"
                            "text\n") == Error::Ok);
        CHECK(deck.slideCount() == 2);
        CHECK(is(deck.settings().theme, "vga"));
        CHECK(deck.settings().durationSeconds == 25 * 60);
        const Slide *one = deck.slide(1);
        const Slide *two = deck.slide(2);
        REQUIRE(one != nullptr);
        REQUIRE(two != nullptr);
        CHECK(deck.slide(3) == nullptr);
        CHECK(is(one->title, "BMDS"));
        CHECK(one->line == 8);
        CHECK(is(two->title, "Second"));
        CHECK(two->line == 12);
        CHECK(one->settings.paginate);
        CHECK(two->settings.paginate);
        CHECK(is(two->settings.footer, "BMDS · 2026"));
        CHECK(deck.diagnosticCount() == 0);
}

TEST_CASE("Deck: an empty deck is one empty slide; a trailing --- makes another") {
        Deck deck;
        REQUIRE(parse(deck, "") == Error::Ok);
        CHECK(deck.slideCount() == 1);
        CHECK(deck.slide(1)->blocks == nullptr);
        Deck two;
        REQUIRE(parse(two, "# A\n\n---\n") == Error::Ok);
        CHECK(two.slideCount() == 2);
}

TEST_CASE("Deck: front matter without its closing fence is a slide break") {
        Deck deck;
        REQUIRE(parse(deck, "---\ntheme: vga\n\n# A\n") == Error::Ok);
        CHECK(reported(deck, "no closing", 1));
        CHECK(deck.settings().theme.isEmpty());
}

TEST_CASE("Deck: local directives carry on, spot directives don't, globals are the deck's") {
        Deck deck;
        REQUIRE(parse(deck, "<!-- class: lead -->\n# One\n\n---\n"
                            "<!-- _class: title -->\n<!-- _backgroundColor: \"#000\" -->\n# Two\n\n---\n"
                            "# Three\n<!-- theme: amber -->\n\n---\n"
                            "<!--\nclass: plain\npaginate: true\n-->\n# Four\n") == Error::Ok);
        REQUIRE(deck.slideCount() == 4);
        CHECK(is(deck.slide(1)->settings.className, "lead"));
        CHECK(is(deck.slide(2)->settings.className, "title"));
        CHECK(is(deck.slide(2)->settings.backgroundColor, "#000"));
        CHECK(is(deck.slide(3)->settings.className, "lead"));
        CHECK(deck.slide(3)->settings.backgroundColor.isEmpty());
        CHECK(is(deck.slide(4)->settings.className, "plain"));
        CHECK(deck.slide(4)->settings.paginate);
        CHECK(!deck.slide(3)->settings.paginate);
        CHECK(is(deck.settings().theme, "amber"));
        CHECK(deck.diagnosticCount() == 0);
        // Directives aren't notes.
        for (uint32_t i = 1; i <= 4; i++) CHECK(deck.slide(i)->notes == nullptr);
}

TEST_CASE("Deck: run, sound and interactive are their slide's only") {
        Deck deck;
        REQUIRE(parse(deck, "<!-- run: luckyduck -->\n<!-- sound: ding.wav -->\n<!-- interactive -->\n# Demo\n\n---\n"
                            "# After\n") == Error::Ok);
        CHECK(is(deck.slide(1)->run, "luckyduck"));
        CHECK(is(deck.slide(1)->sound, "ding.wav"));
        CHECK(deck.slide(1)->interactive);
        CHECK(deck.slide(2)->run.isEmpty());
        CHECK(deck.slide(2)->sound.isEmpty());
        CHECK(!deck.slide(2)->interactive);
        CHECK(deck.diagnosticCount() == 0);
}

TEST_CASE("Deck: a comment that isn't all directives is a speaker note") {
        Deck deck;
        REQUIRE(parse(deck, "# Title\n\n<!-- Say hello.\nThen the demo. -->\n\n"
                            "* item <!-- inline note -->\n\n<!-- Note: smile -->\n") == Error::Ok);
        const Slide *s = deck.slide(1);
        REQUIRE(s->notes != nullptr);
        CHECK(is(s->notes->text, "Say hello.\nThen the demo."));
        REQUIRE(s->notes->next != nullptr);
        CHECK(is(s->notes->next->text, "inline note"));
        REQUIRE(s->notes->next->next != nullptr);
        CHECK(is(s->notes->next->next->text, "Note: smile"));
        // `Note:` isn't a directive's shape (lower case), so nothing is reported.
        CHECK(deck.diagnosticCount() == 0);
        // The list item's comment is gone from its text.
        const Block *list = s->blocks->next;
        REQUIRE(list != nullptr);
        REQUIRE(list->type == BlockType::List);
        char t[64];
        text(list->children->children, t, sizeof t);
        CHECK(strcmp(t, "item ") == 0);
}

TEST_CASE("Deck: a misspelt directive is a note, and reported") {
        Deck deck;
        REQUIRE(parse(deck, "<!-- transtion: wipe -->\n# A\n") == Error::Ok);
        CHECK(reported(deck, "unknown directive `transtion`", 1));
        REQUIRE(deck.slide(1)->notes != nullptr);
        CHECK(deck.slide(1)->settings.transition.isEmpty());
}

TEST_CASE("Deck: values are checked where they're written") {
        Deck deck;
        REQUIRE(parse(deck, "---\nsize: 3x3\nduration: soon\nheadingDivider: 9\nwhatever: 1\n---\n"
                            "<!-- transition: fade -->\n<!-- paginate: maybe -->\n<!-- align: bottom -->\n"
                            "<!-- auto: 10s -->\n<!-- style: x -->\n# A\n") == Error::Ok);
        CHECK(reported(deck, "size `3x3`", 2));
        CHECK(reported(deck, "duration `soon`", 3));
        CHECK(reported(deck, "headingDivider `9`", 4));
        CHECK(reported(deck, "unknown directive `whatever`", 5));
        CHECK(reported(deck, "transition `fade`", 7));
        CHECK(reported(deck, "paginate: true or false", 8));
        CHECK(reported(deck, "align `bottom`", 9));
        CHECK(reported(deck, "`style` isn't supported", 11));
        CHECK(deck.slide(1)->settings.autoMs == 10000);
        CHECK(deck.diagnosticCount() == 8);
}

TEST_CASE("Deck: headingDivider starts a slide at each heading that level or above") {
        Deck deck;
        REQUIRE(parse(deck, "---\nheadingDivider: 2\n---\n# One\ntext\n## Two\n### Three stays\n## Four\n") ==
                Error::Ok);
        REQUIRE(deck.slideCount() == 3);
        CHECK(is(deck.slide(1)->title, "One"));
        CHECK(is(deck.slide(2)->title, "Two"));
        CHECK(count(deck.slide(2)->blocks) == 2);
        CHECK(is(deck.slide(3)->title, "Four"));
}

TEST_CASE("Deck: headingDivider in a comment is reported") {
        Deck deck;
        REQUIRE(parse(deck, "<!-- headingDivider: 2 -->\n# A\n## B\n") == Error::Ok);
        CHECK(deck.slideCount() == 1);
        CHECK(reported(deck, "front matter only", 1));
}

TEST_CASE("Deck: text then --- is a heading, and check says so") {
        Deck deck;
        REQUIRE(parse(deck, "# A\n\nsome text\n---\n\n# B\n") == Error::Ok);
        CHECK(deck.slideCount() == 1);
        CHECK(reported(deck, "put a blank line before it", 4));
}

TEST_CASE("Deck: * lists and 1) lists are fragments, - and 1. lists aren't") {
        Deck deck;
        REQUIRE(parse(deck, "* a\n* b\n  * b1\n  - b2\n\n- c\n- d\n\n1) e\n2) f\n\n1. g\n") == Error::Ok);
        const Slide *s = deck.slide(1);
        CHECK(s->fragments == 5); // a, b, b1, e, f
        const Block *star = s->blocks;
        REQUIRE(star->type == BlockType::List);
        CHECK(star->has(BlockFlag::Fragmented));
        CHECK(star->has(BlockFlag::Tight));
        CHECK(star->children->fragment == 1);
        const Block *b = star->children->next;
        CHECK(b->fragment == 2);
        // b's paragraph, then its nested lists.
        const Block *nested = b->children->next;
        REQUIRE(nested != nullptr);
        CHECK(nested->type == BlockType::List);
        CHECK(nested->level == 2);
        CHECK(nested->children->fragment == 3);
        CHECK(nested->next->children->fragment == 0);
        const Block *dash = star->next;
        CHECK(!dash->has(BlockFlag::Fragmented));
        CHECK(dash->children->fragment == 0);
        const Block *paren = dash->next;
        CHECK(paren->has(BlockFlag::Ordered));
        CHECK(paren->has(BlockFlag::Fragmented));
        CHECK(paren->children->next->fragment == 5);
        const Block *dot = paren->next;
        CHECK(!dot->has(BlockFlag::Fragmented));
}

TEST_CASE("Deck: images' keywords") {
        Deck deck;
        REQUIRE(parse(deck, "# Pictures\n\n"
                            "![bg right:40% contain](images/desktop.png)\n"
                            "![bg](a.png) ![bg sepia](b.png)\n\n"
                            "Inline ![A duck w:200 h:100px](duck.png) and ![50%](half.png) here.\n\n"
                            "![bg vertical grayscale blur:4px](c.png)\n") == Error::Ok);
        const Slide *s = deck.slide(1);
        const Image *bg = s->backgrounds;
        REQUIRE(bg != nullptr);
        CHECK(is(bg->path, "images/desktop.png"));
        CHECK(bg->isBackground());
        CHECK(bg->side == ImageSide::Right);
        CHECK(bg->share == 40);
        CHECK(bg->fit == ImageFit::Contain);
        CHECK(bg->line == 3);
        REQUIRE(bg->next != nullptr);
        CHECK(is(bg->next->path, "a.png"));
        REQUIRE(bg->next->next != nullptr);
        CHECK((bg->next->next->flags & ImageFlag::Sepia) != 0);
        const Image *last = bg->next->next->next;
        REQUIRE(last != nullptr);
        CHECK((last->flags & ImageFlag::Vertical) != 0);
        CHECK((last->flags & ImageFlag::Grayscale) != 0);
        CHECK(reported(deck, "image filter `blur:4px`", 8));
        // The paragraphs of only backgrounds are gone: the heading, then the inline one.
        REQUIRE(count(s->blocks) == 2);
        const Block  *p = s->blocks->next;
        const Inline *i = p->inlines;
        while (i != nullptr && i->type != InlineType::Image) i = i->next;
        REQUIRE(i != nullptr);
        CHECK(is(i->image->alt, "A duck"));
        CHECK(i->image->width == 200);
        CHECK(i->image->height == 100);
        i = i->next;
        while (i != nullptr && i->type != InlineType::Image) i = i->next;
        REQUIRE(i != nullptr);
        CHECK(i->image->fit == ImageFit::Scale);
        CHECK(i->image->scale == 50);
        CHECK(i->image->alt.isEmpty());
}

TEST_CASE("Deck: script regions, and the poster form") {
        Deck deck;
        REQUIRE(parse(deck, "![script w:800 h:450](anim/sort.as)\n\n"
                            "![bg right:45% script:anim/orbit.as](anim/orbit.as.png)\n\n"
                            "![script](notes.txt)\n\n"
                            "![right](x.png)\n") == Error::Ok);
        const Slide *s = deck.slide(1);
        const Image *inl = s->blocks->inlines->image;
        REQUIRE(inl != nullptr);
        CHECK(inl->isScript());
        CHECK(is(inl->script, "anim/sort.as"));
        CHECK(inl->width == 800);
        const Image *bg = s->backgrounds;
        REQUIRE(bg != nullptr);
        CHECK(bg->isScript());
        CHECK(is(bg->script, "anim/orbit.as"));
        CHECK(is(bg->path, "anim/orbit.as.png"));
        CHECK(reported(deck, "AngelScript file (.as): notes.txt", 5));
        CHECK(reported(deck, "for a `bg` image only", 7));
}

TEST_CASE("Deck: code blocks, and angel run") {
        Deck deck;
        REQUIRE(parse(deck, "```c\nint x;\n```\n\n```angel run\nprintln(1);\nprintln(2);\n```\n\n"
                            "```angel run below\nprint(3);\n```\n\n```angel\nshown();\n```\n") == Error::Ok);
        const Block *c = deck.slide(1)->blocks;
        REQUIRE(c->type == BlockType::Code);
        CHECK(is(c->info, "c"));
        CHECK(is(c->text, "int x;"));
        CHECK(!c->has(BlockFlag::Run));
        CHECK(c->line == 2);
        const Block *run = c->next;
        CHECK(is(run->info, "angel"));
        CHECK(run->has(BlockFlag::Run));
        CHECK(!run->has(BlockFlag::RunBelow));
        CHECK(is(run->text, "println(1);\nprintln(2);"));
        CHECK(run->next->has(BlockFlag::RunBelow));
        CHECK(!run->next->next->has(BlockFlag::Run));
}

TEST_CASE("Deck: inline styles, entities, breaks and HTML") {
        Deck deck;
        REQUIRE(parse(deck, "# <!-- fit --> Big &amp; bold\n\n"
                            "**bold** *it* ~~gone~~ `code` [link](http://x) a<br>b &copy; &bogus;\n\n"
                            "<div>shown</div>\n") == Error::Ok);
        const Slide *s = deck.slide(1);
        const Block *h = s->blocks;
        CHECK(h->has(BlockFlag::Fit));
        CHECK(is(s->title, "Big & bold"));
        const Block  *p = h->next;
        const Inline *i = p->inlines;
        CHECK(i->style == InlineStyle::Bold);
        CHECK(is(i->text, "bold"));
        while (i != nullptr && !is(i->text, "it")) i = i->next;
        REQUIRE(i != nullptr);
        CHECK(i->style == InlineStyle::Italic);
        while (i != nullptr && !is(i->text, "gone")) i = i->next;
        REQUIRE(i != nullptr);
        CHECK(i->style == InlineStyle::Strike);
        while (i != nullptr && i->type != InlineType::Code) i = i->next;
        REQUIRE(i != nullptr);
        CHECK(is(i->text, "code"));
        while (i != nullptr && !is(i->text, "link")) i = i->next;
        REQUIRE(i != nullptr);
        CHECK(i->style == InlineStyle::Link);
        char t[128];
        text(p, t, sizeof t);
        CHECK(strstr(t, "a|b \xC2\xA9 &bogus;") != nullptr);
        const Block *html = p->next;
        REQUIRE(html != nullptr);
        CHECK(html->has(BlockFlag::Literal));
        text(html, t, sizeof t);
        CHECK(strcmp(t, "<div>shown</div>") == 0);
        CHECK(deck.diagnosticCount() == 0);
}

TEST_CASE("Deck: fit outside a heading is reported") {
        Deck deck;
        REQUIRE(parse(deck, "text <!-- fit -->\n") == Error::Ok);
        CHECK(reported(deck, "belongs in a heading", 1));
}

TEST_CASE("Deck: tables keep their header and alignment") {
        Deck deck;
        REQUIRE(parse(deck, "| a | b |\n|:--|--:|\n| 1 | 2 |\n| 3 | 4 |\n") == Error::Ok);
        const Block *t = deck.slide(1)->blocks;
        REQUIRE(t->type == BlockType::Table);
        CHECK(count(t->children) == 3);
        CHECK(t->children->has(BlockFlag::Header));
        CHECK(!t->children->next->has(BlockFlag::Header));
        const Block *cell = t->children->next->children;
        CHECK(cell->align == preso::Align::Left);
        CHECK(cell->next->align == preso::Align::Right);
        char text1[16];
        text(cell->next, text1, sizeof text1);
        CHECK(strcmp(text1, "2") == 0);
}

TEST_CASE("Deck: quotes and loose lists hold paragraphs") {
        Deck deck;
        REQUIRE(parse(deck, "> quoted\n> more\n\n* one\n\n* two\n") == Error::Ok);
        const Block *q = deck.slide(1)->blocks;
        REQUIRE(q->type == BlockType::Quote);
        REQUIRE(q->children != nullptr);
        CHECK(q->children->type == BlockType::Paragraph);
        const Block *list = q->next;
        CHECK(!list->has(BlockFlag::Tight));
        CHECK(list->children->children->type == BlockType::Paragraph);
}

TEST_CASE("Deck: blocks nested past the limit are flattened, not lost") {
        char deep[256] = {};
        for (int i = 0; i < 40; i++) deep[i] = '>';
        strcat(deep, " deep\n");
        Deck deck;
        REQUIRE(parse(deck, deep) == Error::Ok);
        CHECK(reported(deck, "flattened"));
        const Block *b = deck.slide(1)->blocks;
        uint32_t     depth = 0;
        while (b != nullptr && b->children != nullptr) {
                b = b->children;
                depth++;
        }
        REQUIRE(b != nullptr);
        CHECK(depth < 40);
        char t[16];
        text(b, t, sizeof t);
        CHECK(strcmp(t, "deep") == 0);
}

TEST_CASE("Deck: a deck past the limit isn't parsed") {
        Deck  deck;
        char *big = static_cast<char *>(malloc(16));
        CHECK(deck.parse(big, Deck::MaxSource + 1) == Error::TooBig);
        CHECK(deck.slideCount() == 0);
}

TEST_CASE("Deck: times") {
        uint32_t ms = 0;
        CHECK(preso::parseDuration("25m", 1000, &ms));
        CHECK(ms == 25 * 60000);
        CHECK(preso::parseDuration("1h30m", 1000, &ms));
        CHECK(ms == 90 * 60000);
        CHECK(preso::parseDuration("1.5s", 1000, &ms));
        CHECK(ms == 1500);
        CHECK(preso::parseDuration("500ms", 1000, &ms));
        CHECK(ms == 500);
        CHECK(preso::parseDuration("12", 60000, &ms));
        CHECK(ms == 12 * 60000);
        CHECK(!preso::parseDuration("soon", 1000, &ms));
        CHECK(!preso::parseDuration("5x", 1000, &ms));
        CHECK(!preso::parseDuration("", 1000, &ms));
}

TEST_CASE("Deck: entities") {
        char utf8[8];
        CHECK(is(preso::translateEntity("&amp;", utf8), "&"));
        CHECK(is(preso::translateEntity("&#65;", utf8), "A"));
        CHECK(is(preso::translateEntity("&#x2192;", utf8), "\xE2\x86\x92"));
        CHECK(is(preso::translateEntity("&#0;", utf8), "\xEF\xBF\xBD"));
        CHECK(preso::translateEntity("&nope;", utf8).isEmpty());
        CHECK(preso::translateEntity("amp", utf8).isEmpty());
}
