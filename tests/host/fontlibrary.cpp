/**
 * @file      fontlibrary.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The font library's names, glyph cache and faces (devplan/fonts.md §4a-§4c, §6b): FontName read
 * and written, the cache's hits, budget, pins, made-up bold, kerning and threads, and the library's
 * scan, style matching, roles, fallback, bitmap sizes, loading and letting go, and layout.
 */

#include <doctest.h>

#include <algorithm>
#include <climits>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <bmds/bitmapfont.h>
#include <bmds/font.h>
#include <bmds/fontface.h>
#include <bmds/fontlibrary.h>
#include <bmds/fontname.h>
#include <bmds/glyphcache.h>
#include <bmds/textblend.h>
#include <bmds/textdraw.h>
#include <bmds/memory.h>
#include <bmds/utf8.h>
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::Font;
using bmds::FontFace;
using bmds::FontFiles;
using bmds::FontLibrary;
using bmds::FontName;
using bmds::GlyphCache;
using bmds::Memory;
using bmds::MemSpace;
using bmds::Result;
using bmds::Span;
using bmds::StringView;

namespace {

        std::vector<uint8_t> load(const char *name) {
                std::ifstream in(std::string(BMDS_FONTS_DIR) + "/" + name, std::ios::binary);
                REQUIRE(in.good());
                return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // The host's files: a folder listed, files read whole; mapped (kept here) if @p mappable.
        class HostFiles final : public FontFiles {
                public:
                        explicit HostFiles(bool mappable) : _mappable(mappable) {}

                        Error list(StringView dir, Visit visit, void *arg) override {
                                std::error_code          ec;
                                const std::string        folder(dir.data(), dir.size());
                                std::vector<std::string> names;
                                for (const auto &e : std::filesystem::directory_iterator(folder, ec))
                                        names.push_back(e.path().string());
                                if (ec) return Error::NotExist;
                                std::sort(names.begin(), names.end());
                                for (const std::string &n : names) visit(arg, StringView(n.data(), n.size()));
                                return Error::Ok;
                        }

                        Result<ByteSpan> map(StringView path) override {
                                if (!_mappable) return bmds::makeError<ByteSpan>(Error::NotSupported);
                                _kept.push_back(read(path));
                                return bmds::makeResult(ByteSpan(_kept.back().data(), _kept.back().size()));
                        }

                        Result<Span<uint8_t>> load(StringView path) override {
                                ++loads;
                                const std::vector<uint8_t> bytes = read(path);
                                if (bytes.empty()) return bmds::makeError<Span<uint8_t>>(Error::NotExist);
                                auto *p = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes.size()));
                                if (p == nullptr) return bmds::makeError<Span<uint8_t>>(Error::NoMem);
                                std::memcpy(p, bytes.data(), bytes.size());
                                return bmds::makeResult(Span<uint8_t>(p, bytes.size()));
                        }

                        int loads = 0;

                private:
                        static std::vector<uint8_t> read(StringView path) {
                                std::ifstream in(std::string(path.data(), path.size()), std::ios::binary);
                                return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                                            std::istreambuf_iterator<char>());
                        }

                        bool                              _mappable;
                        std::vector<std::vector<uint8_t>> _kept;
        };

        std::string formatted(const FontName &name) {
                char         text[FontName::MaxText];
                const size_t n = name.format(text, sizeof(text));
                return std::string(text, n);
        }

        Font open(FontLibrary &lib, const char *name) {
                auto [font, err] = lib.open(StringView(name));
                REQUIRE(err == Error::Ok);
                return font;
        }

        std::string faceOf(const Font &font) {
                const FontLibrary::FaceInfo info = font.library()->face(font.faceIndex());
                return std::string(info.family.data(), info.family.size()) + " " +
                       std::string(info.style.data(), info.style.size());
        }

} // namespace

TEST_CASE("FontName: read and written") {
        auto [a, aerr] = FontName::parse("IBM Plex Sans 13 bold italic");
        REQUIRE(aerr == Error::Ok);
        CHECK(a.familyName() == "IBM Plex Sans");
        CHECK(a.size == 13);
        CHECK(a.style == (FontName::Bold | FontName::Italic));
        CHECK(a.role == FontName::Role::None);
        CHECK(formatted(a) == "IBM Plex Sans 13 bold italic");

        auto [b, berr] = FontName::parse("  IBM   Plex\tSans  ");
        REQUIRE(berr == Error::Ok);
        CHECK(b.familyName() == "IBM Plex Sans");
        CHECK(b.size == 0);
        CHECK_FALSE(b.hasStyle);
        CHECK(formatted(b) == "IBM Plex Sans");

        auto [ui, uerr] = FontName::parse("UI");
        REQUIRE(uerr == Error::Ok);
        CHECK(ui.role == FontName::Role::Ui);
        CHECK(ui.familyLength == 0);
        CHECK(formatted(ui) == "ui");

        auto [title, terr] = FontName::parse("title 20 regular");
        REQUIRE(terr == Error::Ok);
        CHECK(title.role == FontName::Role::Title);
        CHECK(title.size == 20);
        CHECK(title.hasStyle);
        CHECK(title.style == 0);
        CHECK(formatted(title) == "title 20 regular");

        auto [mono, merr] = FontName::parse("mono Bold");
        REQUIRE(merr == Error::Ok);
        CHECK(mono.role == FontName::Role::Mono);
        CHECK(mono.style == FontName::Bold);

        // A role's word inside a family is just a word.
        auto [two, twoErr] = FontName::parse("Terminal Dreams 9");
        REQUIRE(twoErr == Error::Ok);
        CHECK(two.role == FontName::Role::None);
        CHECK(two.familyName() == "Terminal Dreams");

        CHECK(FontName::parse("").error() == Error::Invalid);
        CHECK(FontName::parse("13").error() == Error::Invalid);
        CHECK(FontName::parse("bold italic").error() == Error::Invalid);
        CHECK(FontName::parse("Sans 3").error() == Error::OutOfRange);
        CHECK(FontName::parse("Sans 256").error() == Error::OutOfRange);
        CHECK(FontName::parse(std::string(70, 'x').c_str()).error() == Error::OutOfRange);
        CHECK(FontName::roleFromName("Mono") == FontName::Role::Mono);
        CHECK(FontName::roleFromName("sans") == FontName::Role::None);
}

TEST_CASE("GlyphCache: hits, budget, pins, made-up bold, kerning") {
        const size_t               live = HostMemory::liveAllocations();
        const std::vector<uint8_t> data = load("IBMPlexSans-Regular.ttf");
        FontFace                   face;
        REQUIRE(face.open(ByteSpan(data.data(), data.size())) == Error::Ok);
        {
                GlyphCache     cache(64 * 1024);
                const uint16_t a = face.glyphIndex('A');
                auto [g1, e1] = cache.acquire(3, face, a, 16, FontFace::Depth::Gray4, false);
                REQUIRE(e1 == Error::Ok);
                auto [g2, e2] = cache.acquire(3, face, a, 16, FontFace::Depth::Gray4, false);
                REQUIRE(e2 == Error::Ok);
                CHECK(g1 == g2);
                GlyphCache::Stats s = cache.stats();
                CHECK(s.hits == 1);
                CHECK(s.misses == 1);
                CHECK(s.entries == 1);
                CHECK(s.pinned == 1);
                // The same coverage FontFace renders.
                auto [box, berr] = face.box(a, 16);
                REQUIRE(berr == Error::Ok);
                std::vector<uint8_t> rows(box.workBytes());
                REQUIRE(face.render(a, 16, FontFace::Depth::Gray4, box, Span<uint8_t>(rows.data(), rows.size())) ==
                        Error::Ok);
                CHECK(g1->width == box.width);
                CHECK(g1->x == box.x);
                CHECK(g1->y == box.y);
                CHECK(std::memcmp(g1->rows, rows.data(), size_t(g1->stride) * g1->height) == 0);
                cache.release(g1);
                cache.release(g2);
                CHECK(cache.stats().pinned == 0);

                // Another face id, size, depth or boldness is another entry.
                auto [b, be] = cache.acquire(3, face, a, 16, FontFace::Depth::Gray4, true);
                REQUIRE(be == Error::Ok);
                CHECK(b != g1);
                CHECK(b->width == box.width + 1);
                // Made-up bold covers at least what the regular one does, and a pixel more to the right.
                for (unsigned y = 0; y < b->height; ++y)
                        for (unsigned x = 0; x < box.width; ++x) {
                                const auto at = [](const GlyphCache::Glyph *g, unsigned px, unsigned py) {
                                        const uint8_t v = g->rows[py * g->stride + px / 2];
                                        return (px & 1) != 0 ? v & 0x0f : v >> 4;
                                };
                                CHECK(at(b, x, y) >= at(g1, x, y));
                                CHECK(at(b, x + 1, y) >= at(g1, x, y));
                        }
                cache.release(b);
                auto [m, me] = cache.acquire(3, face, a, 16, FontFace::Depth::Mono, true);
                REQUIRE(me == Error::Ok);
                CHECK(m->stride == (box.width + 1 + 7) / 8);
                cache.release(m);
                auto [sp, spe] = cache.acquire(3, face, face.glyphIndex(' '), 16, FontFace::Depth::Gray4, false);
                REQUIRE(spe == Error::Ok);
                CHECK(sp->width == 0);
                cache.release(sp);
                CHECK(cache.stats().entries == 4);

                // Kerning, one pair and a run.
                const uint16_t v = face.glyphIndex('V'), t = face.glyphIndex('T'), o = face.glyphIndex('o');
                CHECK(cache.kerning(3, face, a, v) == face.kerning(a, v));
                CHECK(cache.kerning(3, face, a, v) == -41);
                const uint16_t run[] = {t, o, a, v, a};
                int16_t        kern[4] = {};
                cache.kerning(3, face, Span<const uint16_t>(run, 5), Span<int16_t>(kern, 4));
                CHECK(kern[0] == -65);
                CHECK(kern[1] == face.kerning(o, a));
                CHECK(kern[2] == -41);
                CHECK(kern[3] == face.kerning(v, a));
                s = cache.stats();
                CHECK(s.kernHits >= 2);

                // A budget of a few glyphs: the oldest go, but never a pinned one.
                cache.setBudget(2000);
                CHECK(cache.stats().bytes <= 2000);
                auto [keep, ke] = cache.acquire(3, face, face.glyphIndex('W'), 40, FontFace::Depth::Gray4, false);
                REQUIRE(ke == Error::Ok);
                for (char c = 'a'; c <= 'z'; ++c) {
                        auto [g, ge] =
                                cache.acquire(3, face, face.glyphIndex(uint8_t(c)), 40, FontFace::Depth::Gray4, false);
                        REQUIRE(ge == Error::Ok);
                        cache.release(g);
                }
                s = cache.stats();
                CHECK(s.evictions > 20);
                auto [again, ae] = cache.acquire(3, face, face.glyphIndex('W'), 40, FontFace::Depth::Gray4, false);
                REQUIRE(ae == Error::Ok);
                CHECK(again == keep); // pinned all along: still there
                cache.release(again);
                cache.release(keep);
                CHECK(cache.stats().bytes <= 2000);

                // Forgetting a face; reclaiming when memory is short.
                cache.setBudget(1 << 20);
                for (char c = 'a'; c <= 'z'; ++c) {
                        auto [g, ge] =
                                cache.acquire(4, face, face.glyphIndex(uint8_t(c)), 20, FontFace::Depth::Gray4, false);
                        REQUIRE(ge == Error::Ok);
                        cache.release(g);
                }
                const size_t before = cache.stats().entries;
                cache.forget(4);
                CHECK(cache.stats().entries < before);
                CHECK(cache.reclaim(MemSpace::Internal, 1 << 20) == 0);
                CHECK(cache.reclaim(MemSpace::Psram, 1 << 20) > 0);
                CHECK(cache.stats().entries == 0);

                // No memory: an error, nothing kept.
                {
                        HostMemory::FailGuard fail;
                        CHECK(cache.acquire(3, face, face.glyphIndex('Q'), 30, FontFace::Depth::Gray4, false).error() ==
                              Error::NoMem);
                }
                CHECK(cache.stats().failures == 1);
        }
        CHECK(HostMemory::liveAllocations() == live);
}

TEST_CASE("GlyphCache: tasks drawing the same glyphs at once") {
        const std::vector<uint8_t> data = load("IBMPlexSans-Regular.ttf");
        FontFace                   face;
        REQUIRE(face.open(ByteSpan(data.data(), data.size())) == Error::Ok);
        GlyphCache               cache(16 * 1024);
        std::vector<std::thread> threads;
        std::atomic<int>         failures{0};
        for (int t = 0; t < 4; ++t)
                threads.emplace_back([&, t] {
                        for (int round = 0; round < 50; ++round)
                                for (char c = 'A'; c <= 'Z'; ++c) {
                                        const unsigned size = 10 + unsigned((round + t) % 6);
                                        auto [g, e] = cache.acquire(0, face, face.glyphIndex(uint8_t(c)), size,
                                                                    FontFace::Depth::Gray4, false);
                                        if (e.isError() || g->width == 0) {
                                                ++failures;
                                                continue;
                                        }
                                        (void)cache.kerning(0, face, face.glyphIndex(uint8_t(c)), face.glyphIndex('o'));
                                        cache.release(g);
                                }
                });
        for (std::thread &t : threads) t.join();
        CHECK(failures == 0);
        const GlyphCache::Stats s = cache.stats();
        CHECK(s.pinned == 0);
        CHECK(s.bytes <= 16 * 1024);
}

TEST_CASE("FontLibrary: faces, styles, roles, fallback, bitmap sizes, layout") {
        const size_t live = HostMemory::liveAllocations();
        {
                GlyphCache  cache(256 * 1024);
                HostFiles   files(true);
                FontLibrary lib(cache, &files);
                REQUIRE(lib.addBitmap(bmds::Fonts::unscii16, "unscii", 13) == Error::Ok);
                const std::string folders = std::string(BMDS_FONTS_DIR) + ";/no/such/folder";
                REQUIRE(lib.scan(StringView(folders.data(), folders.size())) == Error::Ok);
                REQUIRE(lib.faceCount() == 7);
                CHECK(lib.face(0).family == "unscii");
                CHECK(lib.face(0).bitmapSize == 16);
                unsigned plex = 0;
                for (size_t i = 1; i < lib.faceCount(); ++i) {
                        const FontLibrary::FaceInfo f = lib.face(i);
                        plex += f.family.startsWith("IBM Plex");
                        CHECK(f.loaded); // mapped: open for good
                        CHECK(f.glyphs > 1000);
                }
                CHECK(plex == 6);
                // Scanning again adds nothing.
                REQUIRE(lib.scan(BMDS_FONTS_DIR) == Error::Ok);
                CHECK(lib.faceCount() == 7);

                // Styles: the bold face, a made-up bold, no made-up italic.
                Font sans = open(lib, "IBM Plex Sans 13");
                CHECK(faceOf(sans) == "IBM Plex Sans Regular");
                CHECK(sans.pixelSize() == 13);
                CHECK(sans.metrics().ascent == 14);
                CHECK(sans.metrics().descent == 4);
                CHECK(sans.metrics().lineHeight == 18);
                CHECK_FALSE(sans.isBitmap());
                Font bold = open(lib, "ibm plex sans bold");
                CHECK(faceOf(bold) == "IBM Plex Sans Bold");
                CHECK(bold.pixelSize() == FontLibrary::DefaultSize);
                CHECK_FALSE(bold.isMadeUpBold());
                Font serifBold = open(lib, "IBM Plex Serif 20 bold");
                CHECK(faceOf(serifBold) == "IBM Plex Serif Regular");
                CHECK(serifBold.isMadeUpBold());
                Font monoItalic = open(lib, "IBM Plex Mono italic");
                CHECK(faceOf(monoItalic) == "IBM Plex Mono Regular");
                CHECK((monoItalic.name().style & FontName::Italic) == 0);
                Font italic = open(lib, "IBM Plex Sans italic");
                CHECK(faceOf(italic) == "IBM Plex Sans Italic");

                // Roles; a family missing is the ui role's; without a ui role, unscii.
                Font none = open(lib, "Nowhere Sans 20");
                CHECK(none.isBitmap());
                REQUIRE(lib.setRole(FontName::Role::Ui, FontName::parse("IBM Plex Sans 13").value()) == Error::Ok);
                REQUIRE(lib.setRole(FontName::Role::Title, FontName::parse("IBM Plex Sans 15 bold").value()) ==
                        Error::Ok);
                CHECK(lib.setRole(FontName::Role::Title, FontName::parse("ui").value()) == Error::Invalid);
                Font missing = open(lib, "Nowhere Sans 20");
                CHECK(faceOf(missing) == "IBM Plex Sans Regular");
                CHECK(missing.pixelSize() == 20);
                Font title = open(lib, "title");
                CHECK(faceOf(title) == "IBM Plex Sans Bold");
                CHECK(title.pixelSize() == 15);
                Font plainTitle = open(lib, "title 20 regular");
                CHECK(faceOf(plainTitle) == "IBM Plex Sans Regular");
                CHECK(plainTitle.pixelSize() == 20);
                char         text[FontName::MaxText];
                const size_t n = title.name().format(text, sizeof(text));
                CHECK(std::string(text, n) == "IBM Plex Sans 15 bold");
                Font unsetRole = open(lib, "terminal");
                CHECK(unsetRole.isBitmap());

                // Bitmap sizes: whole multiples.
                Font u16 = open(lib, "unscii");
                CHECK(u16.isBitmap());
                CHECK(u16.pixelSize() == 16);
                CHECK(u16.scale() == 1);
                CHECK(u16.metrics().ascent == 13);
                CHECK(u16.metrics().lineHeight == 16);
                CHECK(u16.textWidth("abc") == 24);
                Font u32 = open(lib, "unscii 32");
                CHECK(u32.scale() == 2);
                CHECK(u32.textWidth("abc") == 48);
                CHECK(open(lib, "unscii 13").pixelSize() == 16);

                // Layout: kerned, fitted.
                const uint32_t av = sans.textWidth("AV");
                CHECK(av < sans.textWidth("A") + sans.textWidth("V"));
                CHECK(sans.textWidth("") == 0);
                const uint32_t whole = sans.textWidth("Hello, world");
                CHECK(sans.fitting("Hello, world", whole) == 12);
                CHECK(sans.fitting("Hello, world", whole - 1) < 12);
                CHECK(sans.fitting("Hello, world", 0) == 0);
                CHECK(bold.textWidth("Hello") > sans.textWidth("Hello"));
                CHECK(serifBold.textWidth("ab") == open(lib, "IBM Plex Serif 20").textWidth("ab") + 2);

                // Fallback: box drawing from Mono, then unscii, then the missing glyph.
                const FontFace *sansFace = sans.face();
                const FontFace *monoFace = open(lib, "IBM Plex Mono").face();
                uint32_t        fromMono = 0, fromUnscii = 0;
                for (uint32_t c = 0x2500; c < 0x2600 && (fromMono == 0 || fromUnscii == 0); ++c) {
                        if (sansFace->glyphIndex(c) != 0) continue;
                        if (fromMono == 0 && monoFace->glyphIndex(c) != 0) fromMono = c;
                        if (fromUnscii == 0 && monoFace->glyphIndex(c) == 0 && bmds::Fonts::unscii16.hasGlyph(c))
                                fromUnscii = c;
                }
                REQUIRE(fromMono != 0);
                REQUIRE(fromUnscii != 0);
                const auto facesOf = [](const Font &f, uint32_t c) {
                        char         utf8[8] = {};
                        const size_t len = bmds::Utf8::encode(c, utf8);
                        uint16_t     face = 0xffff;
                        (void)f.layout(
                                StringView(utf8, len),
                                [](void *arg, const Font::Placed &g) {
                                        *static_cast<uint16_t *>(arg) = g.faceIndex;
                                        return true;
                                },
                                &face);
                        return face;
                };
                CHECK(facesOf(sans, fromMono) == 0); // no fallback yet: unscii has the box too
                lib.setFallback("IBM Plex Mono");
                CHECK(lib.face(facesOf(sans, fromMono)).family == "IBM Plex Mono");
                CHECK(facesOf(sans, fromUnscii) == 0);
                CHECK(facesOf(sans, 0x10fffd) == sans.faceIndex()); // nobody has it: Sans' missing glyph

                // Users: each Font holds its face.
                const uint32_t users = lib.face(sans.faceIndex()).users;
                {
                        Font copy = sans;
                        CHECK(lib.face(sans.faceIndex()).users == users + 1);
                        Font moved = static_cast<Font &&>(copy);
                        CHECK(lib.face(sans.faceIndex()).users == users + 1);
                }
                CHECK(lib.face(sans.faceIndex()).users == users);
                lib.setFallback("");
        }
        CHECK(HostMemory::liveAllocations() == live);
}

TEST_CASE("FontLibrary: faces read into memory when used, let go when memory is short") {
        const size_t live = HostMemory::liveAllocations();
        {
                GlyphCache  cache(256 * 1024);
                HostFiles   files(false);
                FontLibrary lib(cache, &files);
                REQUIRE(lib.addBitmap(bmds::Fonts::unscii16, "unscii", 13) == Error::Ok);
                REQUIRE(lib.scan(BMDS_FONTS_DIR) == Error::Ok);
                REQUIRE(lib.faceCount() == 7);
                for (size_t i = 1; i < 7; ++i) CHECK_FALSE(lib.face(i).loaded);
                CHECK(files.loads == 6); // once each, for the names
                {
                        Font sans = open(lib, "IBM Plex Sans 16");
                        CHECK(lib.face(sans.faceIndex()).loaded);
                        CHECK(files.loads == 7);
                        Font again = open(lib, "IBM Plex Sans 12");
                        CHECK(files.loads == 7);
                        CHECK(sans.textWidth("Hi") > 0);
                        // In use: kept.
                        (void)lib.reclaim(MemSpace::Psram, 1u << 30);
                        CHECK(lib.face(sans.faceIndex()).loaded);
                }
                size_t sansIndex = 0;
                for (size_t i = 1; i < 7; ++i)
                        if (lib.face(i).family == "IBM Plex Sans" && lib.face(i).style == "Regular") sansIndex = i;
                CHECK(lib.face(sansIndex).loaded); // kept while unused...
                CHECK(lib.reclaim(MemSpace::Psram, 1u << 30) > 100000);
                CHECK_FALSE(lib.face(sansIndex).loaded); // ...until memory runs short
                Font back = open(lib, "IBM Plex Sans 16");
                CHECK(files.loads == 8);
        }
        CHECK(HostMemory::liveAllocations() == live);
}

TEST_CASE("GlyphCache: a run looked up under one lock; kerning pairs that share a set") {
        const std::vector<uint8_t> data = load("IBMPlexSans-Regular.ttf");
        FontFace                   face;
        REQUIRE(face.open(ByteSpan(data.data(), data.size())) == Error::Ok);
        GlyphCache               cache(256 * 1024);
        const uint16_t           run[] = {face.glyphIndex('a'), face.glyphIndex('b'), face.glyphIndex('c')};
        const GlyphCache::Glyph *found[3] = {};
        CHECK(cache.lookup(1, Span<const uint16_t>(run, 3), 13, FontFace::Depth::Gray4, false,
                           Span<const GlyphCache::Glyph *>(found, 3)) == 0); // nothing made yet
        auto [b, be] = cache.acquire(1, face, run[1], 13, FontFace::Depth::Gray4, false);
        REQUIRE(be == Error::Ok);
        cache.release(b);
        CHECK(cache.lookup(1, Span<const uint16_t>(run, 3), 13, FontFace::Depth::Gray4, false,
                           Span<const GlyphCache::Glyph *>(found, 3)) == 1);
        CHECK(found[0] == nullptr);
        CHECK(found[1] == b);
        CHECK(found[2] == nullptr);
        CHECK(cache.stats().pinned == 1);
        cache.release(found[1]);
        CHECK(cache.stats().pinned == 0);

        // Every ASCII pair kerned twice: the second pass must agree with the face whatever got evicted.
        for (int pass = 0; pass < 2; ++pass)
                for (int a = 0x21; a < 0x7f; ++a)
                        for (int c = 0x21; c < 0x7f; ++c) {
                                const uint16_t l = face.glyphIndex(uint32_t(a)), r = face.glyphIndex(uint32_t(c));
                                REQUIRE(cache.kerning(1, face, l, r) == face.kerning(l, r));
                        }
        // A pair just kerned stays found, with the one before it in its set.
        const uint16_t t = face.glyphIndex('T'), o = face.glyphIndex('o');
        (void)cache.kerning(1, face, t, o);
        const uint32_t hits = cache.stats().kernHits;
        CHECK(cache.kerning(1, face, t, o) == -65);
        CHECK(cache.stats().kernHits == hits + 1);
}

namespace {

        // The reference: one channel as TextBlend mixes it.
        uint8_t mixRef(uint32_t c, uint32_t d, uint32_t a) {
                return uint8_t((c * a + d * (255 - a) + 127) / 255);
        }

        uint32_t noise(uint32_t &state) {
                state = state * 1664525u + 1013904223u;
                return state >> 8;
        }

} // namespace

TEST_CASE("TextBlend: coverage combined as alpha, the ramp, every format's blend and fill") {
        using bmds::TextBlend::Format;
        uint32_t seed = 7;
        // Coverage rows from any pixel, sharp and smooth, combined (the larger of two) as alpha.
        for (int c = 0; c < 500; ++c) {
                uint8_t row[40];
                for (uint8_t &b : row) b = uint8_t(noise(seed));
                uint8_t alpha[320], want[320];
                for (unsigned i = 0; i < 320; ++i) alpha[i] = want[i] = uint8_t(noise(seed) % 16 * 17);
                const unsigned first = noise(seed) % 100, count = noise(seed) % (300 - first);
                bmds::TextBlend::compose(row, FontFace::Depth::Mono, first, count, alpha);
                for (unsigned i = 0; i < count; ++i)
                        if ((row[(first + i) / 8] & (0x80 >> ((first + i) % 8))) != 0) want[i] = 255;
                REQUIRE(std::memcmp(alpha, want, 320) == 0);
                const unsigned gfirst = noise(seed) % 30, gcount = noise(seed) % (80 - gfirst);
                bmds::TextBlend::compose(row, FontFace::Depth::Gray4, gfirst, gcount, alpha);
                for (unsigned i = 0; i < gcount; ++i) {
                        const unsigned px = gfirst + i;
                        const uint8_t  v = uint8_t(((px & 1) != 0 ? row[px / 2] & 15 : row[px / 2] >> 4) * 17);
                        if (v > want[i]) want[i] = v;
                }
                REQUIRE(std::memcmp(alpha, want, 320) == 0);
        }

        // The ramp: level 0 the background, 15 the color, between them the blend; painted where not 0.
        for (Format f : {Format::Bgr24, Format::Rgb24, Format::Bgra32, Format::Rgb565, Format::Gray8, Format::Index8}) {
                bmds::TextBlend::Ramp ramp;
                bmds::TextBlend::makeRamp(f, 0x102030, 0xf0e0d0, ramp);
                const unsigned bpp = bmds::TextBlend::bytesPerPixel(f);
                uint8_t        bg[4], fg[4], mid[4];
                bmds::TextBlend::fill(bg, f, 1, 0xf0e0d0);
                bmds::TextBlend::fill(fg, f, 1, 0x102030);
                std::memcpy(mid, bg, 4);
                const uint8_t seven = 7 * 17;
                bmds::TextBlend::blend(mid, f, &seven, 1, 0x102030, bmds::Simd::Kernel::Scalar);
                CHECK(std::memcmp(ramp.bytes[0], bg, bpp) == 0);
                CHECK(std::memcmp(ramp.bytes[15], fg, bpp) == 0);
                // (RGB565 aside: the ramp mixes the 8-bit colors, a blend the stored pixel's widened bits.)
                if (f != Format::Rgb565) CHECK(std::memcmp(ramp.bytes[7], f == Format::Index8 ? bg : mid, bpp) == 0);
                uint8_t       pixels[4 * 4];
                const uint8_t levels[4] = {0, 255, 7 * 17, 0};
                std::memset(pixels, 0x5a, sizeof(pixels));
                bmds::TextBlend::paint(pixels, levels, 4, ramp);
                CHECK(pixels[0] == 0x5a);
                CHECK(std::memcmp(pixels + bpp, ramp.bytes[15], bpp) == 0);
                CHECK(std::memcmp(pixels + 2 * bpp, ramp.bytes[7], bpp) == 0);
                CHECK(pixels[3 * bpp] == 0x5a);
        }

        // Each format against the formula, with alpha 0 and 255 among the rest.
        for (int c = 0; c < 200; ++c) {
                const uint32_t color = noise(seed) & 0xffffff;
                const uint32_t r = color >> 16 & 0xff, g = color >> 8 & 0xff, b = color & 0xff;
                uint8_t        alpha[70];
                for (uint8_t &a : alpha) {
                        const uint32_t k = noise(seed) % 4;
                        a = k == 0 ? 0 : k == 1 ? 255 : uint8_t(noise(seed));
                }
                uint8_t before[70 * 4], after[70 * 4];
                for (uint8_t &v : before) v = uint8_t(noise(seed));
                const unsigned n = 1 + noise(seed) % 70;
                for (Format f :
                     {Format::Bgr24, Format::Rgb24, Format::Bgra32, Format::Rgb565, Format::Gray8, Format::Index8}) {
                        std::memcpy(after, before, sizeof(after));
                        bmds::TextBlend::blend(after, f, alpha, n, color, bmds::Simd::Kernel::Pie); // scalar here
                        const unsigned bpp = bmds::TextBlend::bytesPerPixel(f);
                        for (unsigned i = 0; i < n; ++i) {
                                const uint8_t *p = before + i * bpp, *q = after + i * bpp;
                                const uint32_t a = alpha[i];
                                switch (f) {
                                        case Format::Bgr24:
                                                REQUIRE(q[0] == mixRef(b, p[0], a));
                                                REQUIRE(q[1] == mixRef(g, p[1], a));
                                                REQUIRE(q[2] == mixRef(r, p[2], a));
                                                break;
                                        case Format::Rgb24:
                                                REQUIRE(q[0] == mixRef(r, p[0], a));
                                                REQUIRE(q[2] == mixRef(b, p[2], a));
                                                break;
                                        case Format::Bgra32:
                                                REQUIRE(q[0] == mixRef(b, p[0], a));
                                                REQUIRE(q[3] == mixRef(255, p[3], a));
                                                break;
                                        case Format::Rgb565: {
                                                const uint32_t v = p[0] | p[1] << 8, w = q[0] | q[1] << 8;
                                                const uint32_t pr = (v >> 11 & 31) << 3 | (v >> 13 & 7);
                                                const uint32_t pb = (v & 31) << 3 | (v >> 2 & 7);
                                                if (a == 0) {
                                                        REQUIRE(w == v);
                                                } else {
                                                        REQUIRE((w >> 11) == (mixRef(r, pr, a) >> 3u));
                                                        REQUIRE((w & 31) == (mixRef(b, pb, a) >> 3u));
                                                }
                                                break;
                                        }
                                        case Format::Gray8:
                                                REQUIRE(q[0] == mixRef(bmds::TextBlend::luma(color), p[0], a));
                                                break;
                                        case Format::Index8: REQUIRE(q[0] == (a >= 128 ? uint8_t(color) : p[0])); break;
                                }
                        }
                        REQUIRE(std::memcmp(after + n * bpp, before + n * bpp, sizeof(after) - n * bpp) == 0);
                }
        }

        uint8_t pixels[12];
        bmds::TextBlend::fill(pixels, Format::Bgr24, 4, 0x123456);
        CHECK(pixels[0] == 0x56);
        CHECK(pixels[2] == 0x12);
        CHECK(pixels[11] == 0x12);
        bmds::TextBlend::fill(pixels, Format::Rgb565, 2, 0xffffff);
        CHECK(pixels[0] == 0xff);
        CHECK(pixels[1] == 0xff);
        bmds::TextBlend::fill(pixels, Format::Bgra32, 1, 0x000000);
        CHECK(pixels[3] == 0xff);
}

TEST_CASE("TextDraw: lines drawn over, on a box, clipped, limited, sharp, indexed, from a bitmap font") {
        using bmds::TextBlend::Format;
        GlyphCache  cache(256 * 1024);
        HostFiles   files(true);
        FontLibrary lib(cache, &files);
        REQUIRE(lib.addBitmap(bmds::Fonts::unscii16, "unscii", 13) == Error::Ok);
        REQUIRE(lib.scan(BMDS_FONTS_DIR) == Error::Ok);
        Font sans = open(lib, "IBM Plex Sans 16");

        constexpr int        W = 200, H = 40;
        std::vector<uint8_t> pic(size_t(W) * H * 3, 0xff);
        bmds::TextTarget     t;
        t.pixels = pic.data();
        t.format = Format::Bgr24;
        t.width = W;
        t.height = H;
        t.stride = size_t(W) * 3;
        bmds::TextPaint paint;
        const auto      dark = [&](int x0, int y0, int x1, int y1) {
                unsigned n = 0;
                for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x) n += pic[(size_t(y) * W + x) * 3] < 0x80;
                return n;
        };

        // Over: ink inside the advance and the line, the advance returned.
        auto [w, err] = bmds::TextDraw::line(t, sans, "To AVA", 10, 5, paint, 0, bmds::Simd::Kernel::Scalar);
        REQUIRE(err == Error::Ok);
        CHECK(w == sans.textWidth("To AVA"));
        const unsigned ink = dark(0, 0, W, H);
        CHECK(ink > 50);
        CHECK(dark(10, 5, 10 + int(w) + 2, 5 + sans.metrics().lineHeight) == ink);
        CHECK(cache.stats().entries > 0);

        // The same again: from the cache, the same pixels.
        std::vector<uint8_t> first = pic;
        std::fill(pic.begin(), pic.end(), 0xff);
        REQUIRE(bmds::TextDraw::line(t, sans, "To AVA", 10, 5, paint, 0, bmds::Simd::Kernel::Scalar).isOk());
        CHECK(pic == first);

        // With the caller's scratch (the window manager's): the same pixels, and more than a batch
        // of glyphs (pieces reused from one batch to the next).
        std::vector<uint64_t>    memory((bmds::TextDraw::scratchBytes() + 7) / 8);
        bmds::TextDraw::Scratch *scratch = bmds::TextDraw::makeScratch(memory.data());
        REQUIRE(scratch != nullptr);
        std::fill(pic.begin(), pic.end(), 0xff);
        REQUIRE(bmds::TextDraw::line(t, sans, "To AVA", 10, 5, paint, 0, bmds::Simd::Kernel::Scalar, scratch).isOk());
        CHECK(pic == first);
        const char *longLine = "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii"; // 50 glyphs: two batches
        std::fill(pic.begin(), pic.end(), 0xff);
        REQUIRE(bmds::TextDraw::line(t, sans, longLine, 0, 5, paint, 0, bmds::Simd::Kernel::Scalar).isOk());
        std::vector<uint8_t> onStack = pic;
        std::fill(pic.begin(), pic.end(), 0xff);
        REQUIRE(bmds::TextDraw::line(t, sans, longLine, 0, 5, paint, 0, bmds::Simd::Kernel::Scalar, scratch).isOk());
        CHECK(pic == onStack);
        std::fill(pic.begin(), pic.end(), 0xff);
        REQUIRE(bmds::TextDraw::line(t, sans, "To AVA", 10, 5, paint, 0, bmds::Simd::Kernel::Scalar, scratch).isOk());
        CHECK(pic == first);
        std::fill(pic.begin(), pic.end(), 0xff);
        REQUIRE(bmds::TextDraw::line(t, sans, "To AVA", 10, 5, paint, 0, bmds::Simd::Kernel::Scalar).isOk());

        // Clipped to a column band: nothing outside it.
        std::fill(pic.begin(), pic.end(), 0xff);
        t.clipLeft = 20;
        t.clipRight = 40;
        REQUIRE(bmds::TextDraw::line(t, sans, "To AVA", 10, 5, paint, 0, bmds::Simd::Kernel::Scalar).isOk());
        CHECK(dark(0, 0, 20, H) == 0);
        CHECK(dark(40, 0, W, H) == 0);
        CHECK(dark(20, 0, 40, H) > 0);
        t.clipLeft = 0;
        t.clipRight = INT_MAX;

        // Off the picture on every side: no fault, nothing drawn outside.
        std::fill(pic.begin(), pic.end(), 0xff);
        REQUIRE(bmds::TextDraw::line(t, sans, "Wide line of text off the edge", 150, 30, paint, 0,
                                     bmds::Simd::Kernel::Scalar)
                        .isOk());
        REQUIRE(bmds::TextDraw::line(t, sans, "left", -20, -8, paint, 0, bmds::Simd::Kernel::Scalar).isOk());

        // A width limit: whole characters that fit.
        std::fill(pic.begin(), pic.end(), 0xff);
        auto [limited, lerr] =
                bmds::TextDraw::line(t, sans, "Hello, world", 0, 5, paint, 30, bmds::Simd::Kernel::Scalar);
        REQUIRE(lerr == Error::Ok);
        CHECK(limited <= 30);
        CHECK(limited == sans.textWidth(StringView("Hello, world").left(sans.fitting("Hello, world", 30))));

        // On a box: the box filled with the background, nothing outside it touched.
        std::fill(pic.begin(), pic.end(), 0xff);
        paint.opaque = true;
        paint.background = 0x00ff00;
        auto [bw, berr] = bmds::TextDraw::line(t, sans, "Box", 50, 4, paint, 0, bmds::Simd::Kernel::Scalar);
        REQUIRE(berr == Error::Ok);
        const int lh = sans.metrics().lineHeight;
        unsigned  green = 0, outside = 0;
        for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                        const uint8_t *p = &pic[(size_t(y) * W + x) * 3];
                        const bool     in = x >= 50 && x < 50 + int(bw) && y >= 4 && y < 4 + lh;
                        if (!in && (p[0] != 0xff || p[1] != 0xff || p[2] != 0xff)) ++outside;
                        if (in && p[0] == 0 && p[1] == 0xff && p[2] == 0) ++green;
                }
        CHECK(outside == 0);
        CHECK(green > bw * unsigned(lh) / 2);
        paint.opaque = false;

        // Sharp: every pixel the color or untouched.
        std::fill(pic.begin(), pic.end(), 0xff);
        paint.smooth = false;
        paint.color = 0x102030;
        REQUIRE(bmds::TextDraw::line(t, sans, "Sharp", 5, 5, paint, 0, bmds::Simd::Kernel::Scalar).isOk());
        for (size_t i = 0; i < pic.size(); i += 3)
                REQUIRE(((pic[i] == 0x30 && pic[i + 2] == 0x10) || (pic[i] == 0xff && pic[i + 2] == 0xff)));
        paint.smooth = true;

        // Indexes: sharp whatever the paint says.
        std::vector<uint8_t> idx(size_t(W) * H, 3);
        bmds::TextTarget     it = t;
        it.pixels = idx.data();
        it.format = Format::Index8;
        it.stride = W;
        paint.color = 9;
        REQUIRE(bmds::TextDraw::line(it, sans, "Index", 5, 5, paint, 0, bmds::Simd::Kernel::Scalar).isOk());
        unsigned nines = 0;
        for (uint8_t v : idx) {
                REQUIRE((v == 3 || v == 9));
                nines += v == 9;
        }
        CHECK(nines > 30);

        // A bitmap font doubled: each set bit a 2 × 2 block, its baseline on the line's.
        Font                 u = open(lib, "unscii 32");
        std::vector<uint8_t> big(size_t(W) * H * 3, 0xff);
        bmds::TextTarget     bt = t;
        bt.pixels = big.data();
        paint.color = 0;
        REQUIRE(bmds::TextDraw::line(bt, u, "A", 0, 0, paint, 0, bmds::Simd::Kernel::Scalar).isOk());
        const bmds::Glyph a = bmds::Fonts::unscii16.glyph('A');
        for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 8; ++x) {
                        const bool set = (a.rows[y] & (0x80 >> x)) != 0;
                        for (unsigned k = 0; k < 4; ++k) {
                                const size_t at = (size_t(y * 2 + k / 2) * W + x * 2 + k % 2) * 3;
                                REQUIRE((big[at] == 0) == set);
                        }
                }
}
