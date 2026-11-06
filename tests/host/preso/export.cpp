/**
 * @file      export.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `preso export` (devplan/preso.md §8a, step 10): every step of every slide as a picture, script
 * regions at the end of each beat, posters beside their scripts; and the plain PNG the core
 * writes when a platform has no encoder of its own.
 */

#include <doctest.h>

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "core/presenter.h"
#include "headless/headless.h"
#include "script/runtime.h"

using preso::Canvas;
using preso::Color;
using preso::Error;
using preso::PixelFormat;
using preso::Presenter;
using preso::Rect;

namespace {

        // Files by name; the pictures written kept as their names, areas and a pixel each.
        class ExportPlatform : public preso::HeadlessPlatform {
                public:
                        struct File {
                                        const char *path;
                                        const char *text;
                        };
                        struct Written {
                                        std::string path;
                                        Rect        area;
                                        Color       probe; ///< The canvas's colour at (probeX, probeY).
                        };
                        const File          *files = nullptr;
                        uint32_t             fileCount = 0;
                        std::vector<Written> written;
                        std::string          folderMade;
                        int32_t              probeX = 0, probeY = 0;
                        uint32_t             failAfter = UINT32_MAX; ///< Writes that succeed before one fails.

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
                        bool fileExists(const char *path) override {
                                for (uint32_t i = 0; i < fileCount; i++) {
                                        if (strcmp(files[i].path, path) == 0) return true;
                                }
                                return false;
                        }
                        PixelFormat pixelFormat() const override { return PixelFormat::Xrgb8888; }
                        Error       makeFolder(const char *path) override {
                                folderMade = path;
                                return Error::Ok;
                        }
                        Error writePicture(const char *path, const Canvas &canvas, const Rect &area) override {
                                if (written.size() >= failAfter) return Error::Io;
                                written.push_back(Written{path, area, canvas.colorAt(probeX, probeY)});
                                return Error::Ok;
                        }
        };

        // A folder of the host's for the test ($TMPDIR, else /tmp), removed after.
        class ScratchDir {
                public:
                        ScratchDir() {
                                const char *base = getenv("TMPDIR");
                                std::string t = std::string(base != nullptr ? base : "/tmp") + "/preso-export-XXXXXX";
                                std::vector<char> name(t.begin(), t.end());
                                name.push_back(0);
                                REQUIRE(mkdtemp(name.data()) != nullptr);
                                path = name.data();
                        }
                        ~ScratchDir() {
                                (void)nftw(
                                        path.c_str(),
                                        [](const char *p, const struct stat *, int, struct FTW *) {
                                                return ::remove(p);
                                        },
                                        16, FTW_DEPTH | FTW_PHYS);
                        }
                        std::string path;
        };

        uint32_t big(const uint8_t *p) {
                return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }

        uint32_t crc32(const uint8_t *p, size_t n) {
                uint32_t c = 0xFFFFFFFFu;
                for (size_t i = 0; i < n; i++) {
                        c ^= p[i];
                        for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                }
                return c ^ 0xFFFFFFFFu;
        }

} // namespace

TEST_CASE("Export: each step of each slide, named so they sort") {
        static const ExportPlatform::File files[] = {
                {"deck.md", "# One\n\n---\n\n# Two\n\n* a\n* b\n\n---\n\n# Three\n"},
        };
        ExportPlatform platform;
        platform.files = files;
        platform.fileCount = 1;
        Presenter presenter(platform, "deck.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        uint32_t written = 0;
        REQUIRE(presenter.exportSlides("out", false, &written) == Error::Ok);
        CHECK(platform.folderMade == "out");
        REQUIRE(written == 5);
        REQUIRE(platform.written.size() == 5);
        const char *names[] = {"out/001.png", "out/002-1.png", "out/002-2.png", "out/002-3.png", "out/003.png"};
        for (uint32_t i = 0; i < 5; i++) {
                CHECK(platform.written[i].path == names[i]);
                CHECK(platform.written[i].area.width == 1280);
                CHECK(platform.written[i].area.height == 720);
        }
        // A write that fails stops it there, saying so.
        ExportPlatform failing;
        failing.files = files;
        failing.fileCount = 1;
        failing.failAfter = 2;
        Presenter other(failing, "deck.md", nullptr);
        REQUIRE(other.load() == Error::Ok);
        CHECK(other.exportSlides("out", false, &written) == Error::Io);
        CHECK(written == 2);
}

TEST_CASE("Export: script regions at each beat's end, and their posters beside the scripts") {
        // Its rectangle is red after the first beat, blue after the second.
        static const ExportPlatform::File files[] = {
                {"talk/deck.md", "# Region\n\n![script w:200 h:100](anim/fill.as)\n"},
                {"talk/anim/fill.as", "SceneRect@ r = SceneRect(200, 100).at(100, 50);\n"
                                      "r.fill = Color::Red;\n"
                                      "r.fillOpacity = 1;\n"
                                      "scene.add(r);\n"
                                      "scene.pause();\n"
                                      "scene.play(r.fillTo(Color::Blue));\n"},
        };
        ExportPlatform platform;
        platform.files = files;
        platform.fileCount = 2;
        Presenter presenter(platform, "talk/deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(presenter.load() == Error::Ok);
        uint32_t written = 0;
        REQUIRE(presenter.exportSlides("pictures", true, &written) == Error::Ok);
        REQUIRE(platform.written.size() == 3);
        CHECK(platform.written[0].path == "pictures/001-1.png");
        CHECK(platform.written[1].path == "pictures/001-2.png");
        CHECK(platform.written[2].path == "talk/anim/fill.as.png");
        // The poster: the region's own rectangle.
        Rect region = platform.written[2].area;
        CHECK(region.width == 200);
        CHECK(region.height == 100);
        // Again, looking at the region's middle: red after its first beat, blue after the second.
        ExportPlatform again;
        again.files = files;
        again.fileCount = 2;
        again.probeX = region.x + 100;
        again.probeY = region.y + 50;
        Presenter second(again, "talk/deck.md", nullptr, preso::makeAngelScripts);
        REQUIRE(second.load() == Error::Ok);
        REQUIRE(second.exportSlides("pictures", true, &written) == Error::Ok);
        REQUIRE(again.written.size() == 3);
        auto red = [](Color c) {
                return ((c >> 16) & 0xFF) > 0x80 && (c & 0xFF) < 0x80;
        };
        auto blue = [](Color c) {
                return (c & 0xFF) > 0x80 && ((c >> 16) & 0xFF) < 0x80;
        };
        CHECK(red(again.written[0].probe));
        CHECK(blue(again.written[1].probe));
        CHECK(blue(again.written[2].probe)); // the poster drawn from the same picture
}

TEST_CASE("Export: the core's PNG, uncompressed and whole") {
        uint32_t pixels[6 * 4];
        for (uint32_t i = 0; i < 24; i++) pixels[i] = 0x102030 + i;
        Canvas canvas;
        canvas.attach(pixels, 6 * 4, 6, 4, PixelFormat::Xrgb8888);
        ScratchDir dir;
        if (dir.path.empty()) return; // reported: no folder to write in
        std::string             path = dir.path + "/p.png";
        preso::HeadlessPlatform platform;
        REQUIRE(platform.writePicture(path.c_str(), canvas, Rect{1, 1, 3, 2}) == Error::Ok);
        FILE *f = fopen(path.c_str(), "rb");
        REQUIRE(f != nullptr);
        if (f == nullptr) return;
        uint8_t data[512];
        size_t  n = fread(data, 1, sizeof data, f);
        fclose(f);
        REQUIRE(n > 8 + 25);
        CHECK(memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0);
        // Its chunks, each CRC right: IHDR 3 × 2 RGB, then the rows stored.
        size_t               at = 8;
        std::vector<uint8_t> idat;
        uint32_t             chunks = 0;
        while (at + 12 <= n) {
                uint32_t length = big(data + at);
                REQUIRE(at + 12 + length <= n);
                CHECK(crc32(data + at + 4, length + 4) == big(data + at + 8 + length));
                if (memcmp(data + at + 4, "IHDR", 4) == 0) {
                        CHECK(big(data + at + 8) == 3);
                        CHECK(big(data + at + 12) == 2);
                        CHECK(data[at + 16] == 8);
                        CHECK(data[at + 17] == 2);
                }
                if (memcmp(data + at + 4, "IDAT", 4) == 0) idat.assign(data + at + 8, data + at + 8 + length);
                chunks++;
                at += 12 + length;
        }
        CHECK(chunks == 3);
        CHECK(at == n);
        // zlib, one final stored block of 2 rows (a filter byte and 3 pixels each), then Adler-32.
        REQUIRE(idat.size() == 2 + 5 + 2 * 10 + 4);
        CHECK(idat[2] == 1);
        CHECK((idat[3] | idat[4] << 8) == 20);
        const uint8_t *row = idat.data() + 7;
        CHECK(row[0] == 0);
        // Pixel (1, 1): index 7.
        CHECK(row[1] == 0x10);
        CHECK(row[2] == 0x20);
        CHECK(row[3] == 0x30 + 7);
        CHECK(row[19] == 0x30 + 15); // (3, 2)
}
