/**
 * @file      wallpaper.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop's pattern (devplan/wallpaper.md steps 1, 3): a palette PNG's colors read and
 * replaced, the tile made into rows at any place, scaled, blended over the desktop's color; the
 * palettes a pattern carries and the user's saved ones, read and edited.
 */

#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <bmds/image.h>
#include <bmds/patternpalettes.h>
#include <bmds/pngdecoder.h>
#include <bmds/string.h>
#include <bmds/tiledpattern.h>

using bmds::Error;
using bmds::TiledPattern;

namespace {

        // 4x2, a palette of red, green and blue (blue transparent): rows 0 1 2 0 and 2 1 0 1.
        std::vector<uint8_t> palettePng() {
                std::vector<uint8_t> out;
                FILE                *f = std::fopen((std::string(BMDS_PNG_DIR) + "/palette-4x2.png").c_str(), "rb");
                if (f == nullptr) return out;
                uint8_t buf[512];
                for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) out.insert(out.end(), buf, buf + n);
                std::fclose(f);
                return out;
        }

        bmds::ByteSpan span(const std::vector<uint8_t> &v) {
                return bmds::ByteSpan(v.data(), v.size());
        }

        // @p width × @p height of the pattern from (@p x, @p y), as 0xRRGGBB.
        std::vector<uint32_t> made(const TiledPattern &w, int x, int y, uint16_t width, uint16_t height) {
                std::vector<uint8_t> bgr(size_t(width) * height * 3 + 3, 0xcd);
                w.make(x, y, width, height, bgr.data(), size_t(width) * 3);
                CHECK(bgr.back() == 0xcd); // nothing past
                std::vector<uint32_t> out;
                for (size_t i = 0; i < size_t(width) * height; ++i)
                        out.push_back(uint32_t(bgr[3 * i + 2]) << 16 | uint32_t(bgr[3 * i + 1]) << 8 | bgr[3 * i]);
                return out;
        }

        // What the 2D-DMA copies from the pattern's tile() at (@p x, @p y) (Surface::startTile()).
        uint32_t fromTile(const bmds::Backdrop::Tile &t, int x, int y) {
                const auto wrap = [](int v, int n) {
                        return ((v % n) + n) % n;
                };
                const int      row = wrap(y - t.y, int(t.height) * t.repeat) / t.repeat;
                const int      col = wrap(x - t.x, t.width);
                const uint8_t *p = t.pixels + size_t(row) * t.stride + size_t(col) * 3;
                return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
        }

        // The tile's pixels are make()'s everywhere, on 4-pixel columns.
        void checkTile(const TiledPattern &w, uint16_t width, uint16_t height) {
                bmds::Backdrop::Tile t;
                REQUIRE(w.tile(t));
                CHECK(t.width % 4 == 0);
                CHECK(t.width >= TiledPattern::MinRow);
                CHECK(t.stride == size_t(t.width) * 3);
                const std::vector<uint32_t> want = made(w, -8, -5, width, height);
                size_t                      wrong = 0;
                for (uint16_t y = 0; y < height; ++y)
                        for (uint16_t x = 0; x < width; ++x)
                                wrong += fromTile(t, x - 8, y - 5) != want[size_t(y) * width + x];
                CHECK(wrong == 0);
        }

        std::vector<uint8_t> romPattern(const char *name) {
                std::vector<uint8_t> file;
                FILE *f = std::fopen((std::string(BMDS_PNG_DIR) + "/../../../rom/patterns/" + name).c_str(), "rb");
                if (f == nullptr) return file;
                int c;
                while ((c = std::fgetc(f)) != EOF) file.push_back(uint8_t(c));
                std::fclose(f);
                return file;
        }

        constexpr uint32_t Red = 0xff0000, Green = 0x00ff00;

} // namespace

TEST_CASE("Png: a palette read, and its entries replaced (their alpha kept)") {
        const std::vector<uint8_t> file = palettePng();
        REQUIRE(!file.empty());
        uint32_t palette[4] = {};
        auto [n, err] = bmds::Png::palette(span(file), bmds::Span<uint32_t>(palette, 4));
        REQUIRE(err.isOk());
        CHECK(n == 4); // Pillow pads it to 2 bits' 4 entries
        CHECK(palette[0] == 0xff0000);
        CHECK(palette[1] == 0x00ff00);
        CHECK(palette[2] == 0x0000ff);

        uint32_t          argb[8] = {};
        bmds::ImageBuffer out;
        out.pixels = reinterpret_cast<uint8_t *>(argb);
        out.width = 4;
        out.height = 2;
        out.stride = 16;
        out.format = bmds::PixelFormat::Bgra32;
        const uint32_t colors[] = {0x123456, 0x80ff80ff, 0xabcdef}; // the second's alpha ignored
        REQUIRE(bmds::Png::decode(span(file), out, 0, bmds::MemSpace::Psram, bmds::Span<const uint32_t>(colors, 3))
                        .isOk());
        CHECK(argb[0] == 0xff123456);
        CHECK(argb[1] == 0xffff80ff);
        CHECK(argb[2] == 0x00abcdef); // transparent still
        // Fewer colors: the rest the file's own.
        REQUIRE(bmds::Png::decode(span(file), out, 0, bmds::MemSpace::Psram, bmds::Span<const uint32_t>(colors, 1))
                        .isOk());
        CHECK(argb[0] == 0xff123456);
        CHECK(argb[1] == 0xff00ff00);

        // Not a palette image: no entries.
        const std::vector<uint8_t> grey = [] {
                std::vector<uint8_t> v;
                FILE                *f = std::fopen((std::string(BMDS_PNG_DIR) + "/grey.png").c_str(), "rb");
                if (f == nullptr) return v;
                uint8_t buf[512];
                for (size_t k; (k = std::fread(buf, 1, sizeof(buf), f)) > 0;) v.insert(v.end(), buf, buf + k);
                std::fclose(f);
                return v;
        }();
        REQUIRE(!grey.empty());
        auto [none, gerr] = bmds::Png::palette(span(grey), bmds::Span<uint32_t>(palette, 4));
        CHECK(gerr.isOk());
        CHECK(none == 0);
}

TEST_CASE("TiledPattern: colors read over a palette") {
        uint32_t p[3] = {1, 2, 3};
        auto [n, err] = TiledPattern::parseColors("#010203; ;#0A0b0c", bmds::Span<uint32_t>(p, 3));
        REQUIRE(err.isOk());
        CHECK(n == 3);
        CHECK(p[0] == 0x010203);
        CHECK(p[1] == 2); // kept
        CHECK(p[2] == 0x0a0b0c);
        CHECK(TiledPattern::parseColors("", bmds::Span<uint32_t>(p, 3)).value() == 0);
        CHECK(TiledPattern::parseColors("#12345", bmds::Span<uint32_t>(p, 3)).error() == Error::Invalid);
        CHECK(TiledPattern::parseColors("red", bmds::Span<uint32_t>(p, 3)).error() == Error::Invalid);
        CHECK(TiledPattern::parseColors("#12345g", bmds::Span<uint32_t>(p, 3)).error() == Error::Invalid);
        // More than the palette has: counted, not stored.
        CHECK(TiledPattern::parseColors("#000000;#000000;#000000;#ffffff", bmds::Span<uint32_t>(p, 3)).value() == 4);
}

TEST_CASE("TiledPattern: tiled at any place, recolored, blended over the desktop's color") {
        const std::vector<uint8_t> file = palettePng();
        REQUIRE(!file.empty());
        TiledPattern w;
        CHECK(!w.isValid());
        REQUIRE(w.load(span(file), "#ffffff", 1, 0x808080).isOk());
        CHECK(w.isValid());
        CHECK(w.width() == 4);
        CHECK(w.height() == 2);
        constexpr uint32_t White = 0xffffff, Bg = 0x808080;
        // Two repeats across, both rows.
        CHECK(made(w, 0, 0, 8, 2) == std::vector<uint32_t>{White, Green, Bg, White, White, Green, Bg, White, Bg, Green,
                                                           White, Green, Bg, Green, White, Green});
        // Anywhere: the phase from the window's corner, negative places too.
        CHECK(made(w, 3, 1, 2, 1) == std::vector<uint32_t>{Green, Bg});
        CHECK(made(w, -1, -1, 2, 1) == std::vector<uint32_t>{Green, Bg});
        CHECK(made(w, 401, 3, 1, 1) == std::vector<uint32_t>{Green});
        // Wider than the kept row (MinRow): the last pixel as the first repeat's.
        const std::vector<uint32_t> wide = made(w, 0, 0, 300, 1);
        CHECK(wide[299] == White);
        CHECK(wide[298] == Bg);
        // A new background: the transparent pixels blended again.
        w.setBackground(0x000000);
        CHECK(made(w, 2, 0, 1, 1) == std::vector<uint32_t>{0x000000});
        CHECK(made(w, 0, 0, 1, 1) == std::vector<uint32_t>{White});
}

TEST_CASE("TiledPattern: scaled, and what it refuses") {
        const std::vector<uint8_t> file = palettePng();
        REQUIRE(!file.empty());
        TiledPattern w;
        REQUIRE(w.load(span(file), "", 2, 0x000000).isOk());
        // Each pixel 2x2: across and down.
        CHECK(made(w, 0, 0, 6, 1) == std::vector<uint32_t>{Red, Red, Green, Green, 0, 0});
        CHECK(made(w, 0, 2, 2, 2) == std::vector<uint32_t>{0, 0, 0, 0});
        CHECK(made(w, 2, 3, 2, 1) == std::vector<uint32_t>{Green, Green});
        CHECK(made(w, 0, 4, 1, 1) == std::vector<uint32_t>{Red}); // down: the repeat is 4 rows
        CHECK(w.bytes() > 0);

        CHECK(w.load(span(file), "", 5, 0) == Error::OutOfRange);
        CHECK(!w.isValid());
        CHECK(w.load(span(file), "", 0, 0) == Error::OutOfRange);
        CHECK(w.load(span(file), "nonsense", 1, 0) == Error::Invalid);
        const uint8_t junk[] = {1, 2, 3};
        CHECK(w.load(bmds::ByteSpan(junk, 3), "", 1, 0).isError());
        CHECK(!w.isValid());
        CHECK(w.bytes() == 0);
}

TEST_CASE("TiledPattern: the tile the 2D-DMA copies") {
        const std::vector<uint8_t> file = palettePng();
        REQUIRE(!file.empty());
        TiledPattern         w;
        bmds::Backdrop::Tile t;
        CHECK(!w.tile(t));
        // Small: kept scaled down and at least MinTall rows, each shown once.
        for (unsigned scale = 1; scale <= TiledPattern::MaxScale; ++scale) {
                REQUIRE(w.load(span(file), "", scale, 0x123456).isOk());
                REQUIRE(w.tile(t));
                CHECK(t.repeat == 1);
                CHECK(t.height >= TiledPattern::MinTall);
                CHECK(t.height % (2 * scale) == 0);
                checkTile(w, 600, 80);
        }
        // An odd width (hexagons, 28: a multiple of 4 already; scanlines 12 at scale 3 is 36).
        const std::vector<uint8_t> scan = romPattern("scanlines.png");
        REQUIRE(!scan.empty());
        REQUIRE(w.load(span(scan), "", 3, 0).isOk());
        checkTile(w, 300, 40);
        // Large: past MaxScaledBytes, rows kept once and shown scale times by the 2D-DMA.
        const std::vector<uint8_t> topo = romPattern("topo.png");
        REQUIRE(!topo.empty());
        REQUIRE(w.load(span(topo), "", 2, 0).isOk());
        REQUIRE(w.tile(t));
        CHECK(t.repeat == 2);
        CHECK(t.height == 256);
        checkTile(w, 1100, 600);
        REQUIRE(w.load(span(topo), "", 1, 0).isOk());
        REQUIRE(w.tile(t));
        CHECK(t.repeat == 1);
        checkTile(w, 600, 300);
}

TEST_CASE("Png: text chunks") {
        // checks.png's: Author, License, then its built-in palettes.
        std::vector<uint8_t> file;
        FILE *f = std::fopen((std::string(BMDS_PNG_DIR) + "/../../../rom/patterns/checks.png").c_str(), "rb");
        REQUIRE(f != nullptr);
        uint8_t buf[512];
        for (size_t k; (k = std::fread(buf, 1, sizeof(buf), f)) > 0;) file.insert(file.end(), buf, buf + k);
        std::fclose(f);
        size_t           at = 8;
        bmds::StringView key, text;
        REQUIRE(bmds::Png::nextText(span(file), at, key, text));
        CHECK(key == "Author");
        CHECK(text == "BMDS");
        REQUIRE(bmds::Png::nextText(span(file), at, key, text));
        CHECK(key == "License");
        bmds::PatternPalettes::Palette found[4];
        CHECK(bmds::PatternPalettes::builtIn(span(file), bmds::Span<bmds::PatternPalettes::Palette>(found, 4)) == 2);
        CHECK(found[0].name == "Mint");
        CHECK(found[0].colors == "#a8d8c0;#86b8a0");
        CHECK(found[1].name == "Sand");
        // None in a file without them.
        const std::vector<uint8_t> plain = palettePng();
        at = 8;
        CHECK(!bmds::Png::nextText(span(plain), at, key, text));
}

TEST_CASE("PatternPalettes: saved ones read, set, replaced and removed, other lines kept") {
        namespace PP = bmds::PatternPalettes;
        const bmds::StringView text = "# my palettes\n"
                                      "[truchet]\n"
                                      "Current=#000010;#00e0ff\n"
                                      "Ice=#000010;#c0f0ff\n"
                                      "\n"
                                      "[starfield]\r\n"
                                      "Sunset = #200010;#ff6030\r\n";
        PP::Palette            p[4];
        CHECK(PP::saved(text, "truchet", bmds::Span<PP::Palette>(p, 4)) == 1);
        CHECK(p[0].name == "Ice");
        CHECK(p[0].colors == "#000010;#c0f0ff");
        CHECK(PP::saved(text, "starfield", bmds::Span<PP::Palette>(p, 4)) == 1);
        CHECK(p[0].name == "Sunset");
        CHECK(p[0].colors == "#200010;#ff6030");
        CHECK(PP::saved(text, "checks", bmds::Span<PP::Palette>(p, 4)) == 0);
        bmds::StringView current;
        REQUIRE(PP::current(text, "truchet", current));
        CHECK(current == "#000010;#00e0ff");
        CHECK(!PP::current(text, "starfield", current));

        bmds::String out(bmds::MemSpace::Psram);
        // Replaced in place.
        REQUIRE(PP::set(text, "truchet", "Current", "#111111", out).isOk());
        CHECK(out.view() == "# my palettes\n[truchet]\nCurrent=#111111\nIce=#000010;#c0f0ff\n\n[starfield]\r\n"
                            "Sunset = #200010;#ff6030\r\n");
        // Added after the group's last entry (before its blank line).
        REQUIRE(PP::set(text, "truchet", "Moss", "#222222", out).isOk());
        CHECK(out.view() == "# my palettes\n[truchet]\nCurrent=#000010;#00e0ff\nIce=#000010;#c0f0ff\nMoss=#222222\n\n"
                            "[starfield]\r\nSunset = #200010;#ff6030\r\n");
        // A new group at the end.
        REQUIRE(PP::set(text, "checks", "Current", "#333333", out).isOk());
        CHECK(out.view().endsWith("Sunset = #200010;#ff6030\r\n\n[checks]\nCurrent=#333333\n"));
        // Into nothing, and onto a text without a last newline.
        REQUIRE(PP::set("", "checks", "Mine", "#444444", out).isOk());
        CHECK(out.view() == "[checks]\nMine=#444444\n");
        REQUIRE(PP::set("[checks]\nMine=#444444", "checks", "Yours", "#555555", out).isOk());
        CHECK(out.view() == "[checks]\nMine=#444444\nYours=#555555\n");
        // Names the text can't hold.
        CHECK(PP::set(text, "truchet", "Current=x", "#000000", out) == bmds::Error::Invalid);
        CHECK(PP::set(text, "a]b", "Mine", "#000000", out) == bmds::Error::Invalid);
        CHECK(PP::set(text, "truchet", "Mine", "#000000\n[x]", out) == bmds::Error::Invalid);
        // Removed.
        REQUIRE(PP::remove(text, "starfield", "Sunset", out).isOk());
        CHECK(out.view() ==
              "# my palettes\n[truchet]\nCurrent=#000010;#00e0ff\nIce=#000010;#c0f0ff\n\n[starfield]\r\n");
        CHECK(PP::remove(text, "truchet", "Sunset", out) == bmds::Error::NotExist);

        CHECK(PP::isValidName("Ice"));
        CHECK(PP::isValidName("Deep blue 2"));
        CHECK(!PP::isValidName(""));
        CHECK(!PP::isValidName(" Ice"));
        CHECK(!PP::isValidName("Current"));
        CHECK(!PP::isValidName("Original"));
        CHECK(!PP::isValidName("a=b"));
        CHECK(!PP::isValidName("a;b"));
        CHECK(!PP::isValidName("#x"));
        CHECK(!PP::isValidName("123456789012345678901234567890123"));
}
