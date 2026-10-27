/**
 * @file      fonts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The font library's faces (devplan/fonts.md §4a-§4c, bmds_fonts): IBM Plex opened, its names,
 * metrics, character map and kerning (the figures from fontTools, adding up every lookup of the
 * `kern` feature), every glyph of every shipped face rendered, and damaged files refused.
 */

#include <doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <bmds/fontface.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::FontFace;
using bmds::Span;

namespace {

        std::vector<uint8_t> load(const char *name) {
                std::ifstream in(std::string(BMDS_FONTS_DIR) + "/" + name, std::ios::binary);
                REQUIRE(in.good());
                return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        uint16_t be16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] << 8 | p[1]);
        }

        uint32_t be32(const uint8_t *p) {
                return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }

        // The directory entry of table @p tag (its record's offset in the file), or 0.
        size_t tableRecord(const std::vector<uint8_t> &font, const char *tag) {
                const uint16_t count = be16(font.data() + 4);
                for (size_t i = 0; i < count; ++i)
                        if (std::memcmp(font.data() + 12 + i * 16, tag, 4) == 0) return 12 + i * 16;
                return 0;
        }

        size_t tableAt(const std::vector<uint8_t> &font, const char *tag) {
                return be32(font.data() + tableRecord(font, tag) + 8);
        }

        bool longLoca(const std::vector<uint8_t> &font) {
                return be16(font.data() + tableAt(font, "head") + 50) == 1;
        }

        // Where glyph @p glyph's outline starts in the file.
        size_t glyphAt(const std::vector<uint8_t> &font, uint16_t glyph) {
                const uint8_t *loca = font.data() + tableAt(font, "loca");
                return tableAt(font, "glyf") +
                       (longLoca(font) ? be32(loca + glyph * 4u) : be16(loca + glyph * 2u) * 2u);
        }

        int kern(const FontFace &face, char a, char b) {
                return face.kerning(face.glyphIndex(uint8_t(a)), face.glyphIndex(uint8_t(b)));
        }

        // Renders @p glyph at @p size and @p depth into @p rows (packed), @p box its box.
        Error render(const FontFace &face, uint16_t glyph, unsigned size, FontFace::Depth depth,
                     FontFace::GlyphBox &box, std::vector<uint8_t> &rows) {
                auto [b, err] = face.box(glyph, size);
                if (err.isError()) return err;
                box = b;
                rows.assign(box.workBytes() + 1, 0xee);
                return face.render(glyph, size, depth, box, Span<uint8_t>(rows.data(), rows.size()));
        }

        unsigned gray(const std::vector<uint8_t> &rows, const FontFace::GlyphBox &box, unsigned x, unsigned y) {
                const uint8_t b = rows[y * box.stride(FontFace::Depth::Gray4) + x / 2];
                return (x & 1) != 0 ? b & 0x0f : b >> 4;
        }

        bool mono(const std::vector<uint8_t> &rows, const FontFace::GlyphBox &box, unsigned x, unsigned y) {
                return (rows[y * box.stride(FontFace::Depth::Mono) + x / 8] & (0x80 >> (x & 7))) != 0;
        }

} // namespace

TEST_CASE("FontFace opens IBM Plex: names, metrics, character map") {
        const std::vector<uint8_t> sansData = load("IBMPlexSans-Regular.ttf");
        FontFace                   sans;
        REQUIRE(sans.open(ByteSpan(sansData.data(), sansData.size())) == Error::Ok);
        CHECK(sans.isOpen());
        CHECK(sans.family() == "IBM Plex Sans");
        CHECK(sans.style() == "Regular");
        CHECK_FALSE(sans.isFixedPitch());
        CHECK(sans.glyphCount() == 1019);
        CHECK(sans.unitsPerEm() == 1000);
        CHECK(sans.metrics().ascent == 1025);
        CHECK(sans.metrics().descent == -275);
        CHECK(sans.metrics().lineGap == 0);
        CHECK(sans.hasKerning());
        CHECK(sans.open(ByteSpan(sansData.data(), sansData.size())) == Error::InvalidState);

        CHECK(sans.glyphIndex('A') == 33);
        CHECK(sans.glyphIndex(0x416) == 686);  // Cyrillic Zhe (format 4, past the first 256)
        CHECK(sans.glyphIndex(0x20ac) == 173); // the euro sign
        CHECK(sans.glyphIndex(0x4e00) == 0);   // no CJK
        CHECK(sans.glyphIndex(0x10ffff) == 0);
        CHECK(sans.glyphIndex(0xffffffffu) == 0);
        CHECK(sans.advance(33) == 641);
        CHECK(sans.advance(60000) == 0);

        const std::vector<uint8_t> monoData = load("IBMPlexMono-Regular.ttf");
        FontFace                   monoFace;
        REQUIRE(monoFace.open(ByteSpan(monoData.data(), monoData.size())) == Error::Ok);
        CHECK(monoFace.family() == "IBM Plex Mono");
        CHECK(monoFace.isFixedPitch());
        CHECK(monoFace.glyphCount() == 1207);
        CHECK(monoFace.advance(monoFace.glyphIndex('W')) == 600);
        CHECK(monoFace.advance(monoFace.glyphIndex('i')) == 600);
        CHECK_FALSE(monoFace.hasKerning());
        CHECK(kern(monoFace, 'A', 'V') == 0);

        const std::vector<uint8_t> boldData = load("IBMPlexSans-Bold.ttf");
        FontFace                   bold;
        REQUIRE(bold.open(ByteSpan(boldData.data(), boldData.size())) == Error::Ok);
        CHECK(bold.family() == "IBM Plex Sans");
        CHECK(bold.style() == "Bold");
}

TEST_CASE("FontFace kerns: every lookup of the kern feature adds its part") {
        const std::vector<uint8_t> data = load("IBMPlexSans-Regular.ttf");
        FontFace                   face;
        REQUIRE(face.open(ByteSpan(data.data(), data.size())) == Error::Ok);
        // From fontTools. stb_truetype's own lookup stops at the first lookup covering the left
        // glyph, and gives 0 for To, Ta, Yo, VA and "A.
        CHECK(kern(face, 'A', 'V') == -41);
        CHECK(kern(face, 'T', 'o') == -65);
        CHECK(kern(face, 'T', 'a') == -70);
        CHECK(kern(face, 'Y', 'o') == -50);
        CHECK(kern(face, 'V', 'A') == -41);
        CHECK(kern(face, 'L', 'T') == -70);
        CHECK(kern(face, 'P', '.') == -100);
        CHECK(kern(face, '"', 'A') == -50);
        CHECK(kern(face, '"', 'T') == 15);
        CHECK(kern(face, 'r', 'n') == 0);
        CHECK(face.kerning(33, 60000) == 0);

        int total = 0, kerned = 0;
        for (int a = 0x21; a < 0x7f; ++a)
                for (int b = 0x21; b < 0x7f; ++b) {
                        const int k = kern(face, char(a), char(b));
                        total += k;
                        kerned += k != 0;
                }
        CHECK(total == -19505);
        CHECK(kerned == 1228);
}

TEST_CASE("FontFace renders: boxes, sharp and smooth coverage, compound glyphs") {
        const std::vector<uint8_t> data = load("IBMPlexSans-Regular.ttf");
        FontFace                   face;
        REQUIRE(face.open(ByteSpan(data.data(), data.size())) == Error::Ok);

        FontFace::GlyphBox   box, sharpBox;
        std::vector<uint8_t> smooth, sharp;
        const uint16_t       a = face.glyphIndex('A');
        REQUIRE(render(face, a, 16, FontFace::Depth::Gray4, box, smooth) == Error::Ok);
        CHECK(box.width >= 9);
        CHECK(box.width <= 12);
        CHECK(box.y < -9); // above the baseline
        CHECK(box.y + box.height <= 1);
        CHECK(box.stride(FontFace::Depth::Gray4) == (box.width + 1u) / 2);
        CHECK(box.stride(FontFace::Depth::Mono) == (box.width + 7u) / 8);

        // Sharp is smooth's coverage at half or more: the same rounding, so they agree exactly.
        for (unsigned size : {9u, 13u, 16u, 24u, 48u})
                for (char c : {'A', 'g', 'W', '@', '%'}) {
                        const uint16_t glyph = face.glyphIndex(uint8_t(c));
                        REQUIRE(render(face, glyph, size, FontFace::Depth::Gray4, box, smooth) == Error::Ok);
                        REQUIRE(render(face, glyph, size, FontFace::Depth::Mono, sharpBox, sharp) == Error::Ok);
                        REQUIRE(sharpBox.width == box.width);
                        unsigned inked = 0, full = 0;
                        for (unsigned y = 0; y < box.height; ++y)
                                for (unsigned x = 0; x < box.width; ++x) {
                                        CHECK(mono(sharp, box, x, y) == (gray(smooth, box, x, y) >= 8));
                                        inked += gray(smooth, box, x, y) != 0;
                                        full += gray(smooth, box, x, y) == 15;
                                }
                        CHECK(inked > 0);
                        if (size >= 24) CHECK(full > 0);
                        // A row's padding is clear.
                        if ((box.width & 1) != 0) CHECK((smooth[box.stride(FontFace::Depth::Gray4) - 1] & 0x0f) == 0);
                }

        // A space: no box, nothing to draw.
        REQUIRE(render(face, face.glyphIndex(' '), 16, FontFace::Depth::Gray4, box, smooth) == Error::Ok);
        CHECK(box.width == 0);
        CHECK(box.height == 0);

        // Á is A and an acute: taller than A, the same width.
        FontFace::GlyphBox plain;
        REQUIRE(render(face, a, 24, FontFace::Depth::Gray4, plain, smooth) == Error::Ok);
        REQUIRE(render(face, face.glyphIndex(0xc1), 24, FontFace::Depth::Gray4, box, smooth) == Error::Ok);
        CHECK(box.y < plain.y);
        CHECK(box.y + box.height == plain.y + plain.height);

        // Wrong arguments.
        REQUIRE(face.box(a, 16).isOk());
        box = face.box(a, 16).value();
        std::vector<uint8_t> small(box.workBytes() - 1);
        CHECK(face.render(a, 16, FontFace::Depth::Gray4, box, Span<uint8_t>(small.data(), small.size())) ==
              Error::BufferTooSmall);
        CHECK(face.box(a, 3).error() == Error::OutOfRange);
        CHECK(face.box(a, 256).error() == Error::OutOfRange);
        CHECK(face.box(face.glyphCount(), 16).error() == Error::OutOfRange);
}

TEST_CASE("FontFace draws every glyph of every shipped face") {
        for (const char *name : {"IBMPlexSans-Regular.ttf", "IBMPlexSans-Bold.ttf", "IBMPlexSans-Italic.ttf",
                                 "IBMPlexMono-Regular.ttf", "IBMPlexMono-Bold.ttf", "IBMPlexSerif-Regular.ttf"}) {
                CAPTURE(name);
                const std::vector<uint8_t> data = load(name);
                FontFace                   face;
                REQUIRE(face.open(ByteSpan(data.data(), data.size())) == Error::Ok);
                FontFace::GlyphBox   box;
                std::vector<uint8_t> rows;
                unsigned             failed = 0;
                for (uint32_t g = 0; g < face.glyphCount(); ++g)
                        failed += render(face, uint16_t(g), 13, FontFace::Depth::Gray4, box, rows).isError();
                CHECK(failed == 0);
        }
}

TEST_CASE("FontFace refuses damaged files") {
        const std::vector<uint8_t> good = load("IBMPlexSans-Regular.ttf");
        const auto                 opens = [](const std::vector<uint8_t> &font) {
                FontFace face;
                return face.open(ByteSpan(font.data(), font.size()));
        };
        CHECK(opens(std::vector<uint8_t>()) == Error::CorruptData);
        CHECK(opens(std::vector<uint8_t>(good.begin(), good.begin() + 100)) == Error::CorruptData);
        CHECK(opens(std::vector<uint8_t>(good.begin(), good.end() - 1000)) == Error::CorruptData);

        std::vector<uint8_t> bad = good;
        std::memcpy(bad.data(), "OTTO", 4);
        CHECK(opens(bad) == Error::NotSupported);
        std::memcpy(bad.data(), "ttcf", 4);
        CHECK(opens(bad) == Error::NotSupported);
        std::memcpy(bad.data(), "XXXX", 4);
        CHECK(opens(bad) == Error::CorruptData);

        // A table past the end of the file.
        bad = good;
        bad[tableRecord(bad, "hmtx") + 8] = 0x7f;
        CHECK(opens(bad) == Error::CorruptData);

        // No glyf: an outline format not read here.
        bad = good;
        std::memcpy(bad.data() + tableRecord(bad, "glyf"), "xxxx", 4);
        CHECK(opens(bad) == Error::NotSupported);

        // loca going backwards: glyph 40 starting after glyph 41.
        bad = good;
        const size_t loca = tableAt(bad, "loca") + (longLoca(bad) ? 40 * 4 : 40 * 2);
        bad[loca] = bad[loca + (longLoca(bad) ? 4 : 2)];
        bad[loca + 1] = 0xff;
        CHECK(opens(bad) == Error::CorruptData);

        // More horizontal metrics than glyphs.
        bad = good;
        bad[tableAt(bad, "hhea") + 34] = 0xff;
        CHECK(opens(bad) == Error::CorruptData);

        // A compound glyph made of itself, and one made of a glyph that isn't there.
        bad = good;
        const uint16_t aacute = 391;
        const size_t   at = glyphAt(bad, aacute);
        REQUIRE(int16_t(be16(bad.data() + at)) < 0);
        bad[at + 12] = uint8_t(aacute >> 8);
        bad[at + 13] = uint8_t(aacute & 0xff);
        {
                FontFace face;
                REQUIRE(face.open(ByteSpan(bad.data(), bad.size())) == Error::Ok);
                CHECK(face.box(aacute, 16).error() == Error::CorruptData);
                CHECK(face.box(33, 16).isOk()); // the rest of the font still draws
        }
        bad[at + 12] = 0xff;
        bad[at + 13] = 0xff;
        {
                FontFace face;
                REQUIRE(face.open(ByteSpan(bad.data(), bad.size())) == Error::Ok);
                CHECK(face.box(aacute, 16).error() == Error::CorruptData);
        }

        // A simple glyph whose instructions run past its end.
        bad = good;
        const size_t   glyphA = glyphAt(bad, 33);
        const uint16_t contours = be16(bad.data() + glyphA);
        REQUIRE(contours > 0);
        bad[glyphA + 10 + contours * 2] = 0xff;
        {
                FontFace face;
                REQUIRE(face.open(ByteSpan(bad.data(), bad.size())) == Error::Ok);
                CHECK(face.box(33, 16).error() == Error::CorruptData);
                std::vector<uint8_t>     rows(4096);
                const FontFace::GlyphBox box{0, -12, 10, 12};
                CHECK(face.render(33, 16, FontFace::Depth::Gray4, box, Span<uint8_t>(rows.data(), rows.size())) ==
                      Error::CorruptData);
        }
}

TEST_CASE("FontFace: the fuzzer's finds open and draw safely, or not at all") {
        unsigned files = 0;
        for (const auto &entry : std::filesystem::directory_iterator(std::string(BMDS_FONTS_TEST_DIR) + "/fuzz")) {
                CAPTURE(entry.path().string());
                std::ifstream        in(entry.path(), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                ++files;
                FontFace face;
                if (face.open(ByteSpan(data.data(), data.size())).isError()) continue;
                FontFace::GlyphBox   box;
                std::vector<uint8_t> rows;
                for (uint32_t g = 0; g < face.glyphCount() && g < 64; ++g)
                        for (unsigned size : {7u, 13u, 40u})
                                for (FontFace::Depth depth : {FontFace::Depth::Gray4, FontFace::Depth::Mono})
                                        (void)render(face, uint16_t(g), size, depth, box, rows);
        }
        CHECK(files > 0);
}

TEST_CASE("FontFace: what other fonts do (edge.ttf, symbol.ttf from make-edge-fonts.py)") {
        const auto loadTest = [](const char *name) {
                std::ifstream in(std::string(BMDS_FONTS_TEST_DIR) + "/" + name, std::ios::binary);
                REQUIRE(in.good());
                return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        const std::vector<uint8_t> edge = loadTest("edge.ttf");
        FontFace                   face;
        REQUIRE(face.open(ByteSpan(edge.data(), edge.size())) == Error::Ok);
        FontFace::GlyphBox   box;
        std::vector<uint8_t> rows;

        // An empty contour, then squares: each drawn, nothing joining them.
        REQUIRE(render(face, face.glyphIndex('A'), 100, FontFace::Depth::Gray4, box, rows) == Error::Ok);
        const int  mid = box.height / 2 - 6; // 35 px up: inside the first square, above the second
        const auto at = [&](int x) {
                return gray(rows, box, unsigned(x - box.x), unsigned(mid));
        };
        CHECK(at(30) == 15); // inside the first square (10-60 px)
        CHECK(at(65) == 0);  // between them
        // The two squares on the right (70-90 px across; 0-30 and 40-70 px up), not joined: nothing between.
        const auto up = [&](int x, int y) {
                return gray(rows, box, unsigned(x - box.x), unsigned(box.height - 1 - y));
        };
        CHECK(up(80, 15) == 15);
        CHECK(up(80, 55) == 15);
        CHECK(up(80, 35) == 0);

        // A lone on-curve point last: drawn (a hinting anchor); a lone off-curve one: refused.
        CHECK(face.box(face.glyphIndex('B'), 16).isOk());
        CHECK(face.box(face.glyphIndex('C'), 16).error() == Error::CorruptData);

        // A box turned inside out: nothing to draw, no error.
        auto [empty, eerr] = face.box(face.glyphIndex('D'), 16);
        REQUIRE(eerr == Error::Ok);
        CHECK(empty.width == 0);
        CHECK(empty.height == 0);

        // 20,000 points: drawn.
        REQUIRE(render(face, face.glyphIndex('E'), 40, FontFace::Depth::Gray4, box, rows) == Error::Ok);
        CHECK(gray(rows, box, box.width / 2, box.height / 2) == 15);

        // Only a symbol map: 0x20-0xFF found at U+F020-F0FF too.
        const std::vector<uint8_t> sym = loadTest("symbol.ttf");
        FontFace                   symbol;
        REQUIRE(symbol.open(ByteSpan(sym.data(), sym.size())) == Error::Ok);
        CHECK(symbol.glyphIndex('A') != 0);
        CHECK(symbol.glyphIndex('A') == symbol.glyphIndex(0xf041));
        CHECK(symbol.glyphIndex('B') == 0);
}
