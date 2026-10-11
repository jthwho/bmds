/**
 * @file      images.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The image library's pure parts (devplan/images.md §3): pixel rows, PNG's every kind, BMP,
 * JPEG headers, the scaler. The pictures are tests/host/images/make.py's.
 */

#include <doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <bmds/bmp.h>
#include <bmds/image.h>
#include <bmds/imageplan.h>
#include <bmds/imagescaler.h>
#include <bmds/jpegheader.h>
#include <bmds/pngdecoder.h>
#include <bmds/softjpeg.h>

using bmds::Error;
using bmds::ImageBuffer;
using bmds::PixelFormat;

namespace {

        std::vector<uint8_t> readFile(const std::string &name) {
                std::vector<uint8_t> out;
                FILE                *f = std::fopen((std::string(BMDS_IMAGES_DIR) + "/" + name).c_str(), "rb");
                if (f == nullptr) return out;
                uint8_t buf[4096];
                for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) out.insert(out.end(), buf, buf + n);
                std::fclose(f);
                return out;
        }

        std::vector<uint32_t> argbOf(const std::string &name) {
                const std::vector<uint8_t> b = readFile(name + ".argb");
                std::vector<uint32_t>      out(b.size() / 4);
                std::memcpy(out.data(), b.data(), b.size());
                return out;
        }

        bmds::ByteSpan span(const std::vector<uint8_t> &v) {
                return bmds::ByteSpan(v.data(), v.size());
        }

        // A buffer of @p format with a stride padded past the row, to catch strides ignored.
        struct Buffer {
                        std::vector<uint8_t> bytes;
                        ImageBuffer          image;

                        Buffer(uint32_t w, uint32_t h, PixelFormat f, size_t pad = 5) {
                                image.width = w;
                                image.height = h;
                                image.format = f;
                                image.stride = size_t(w) * bmds::bytesPerPixel(f) + pad;
                                bytes.assign(image.stride * h, 0xcd);
                                image.pixels = bytes.data();
                        }

                        uint32_t argb(uint32_t x, uint32_t y) const {
                                uint32_t out;
                                bmds::Pixels::toArgb(image.row(y) + x * bmds::bytesPerPixel(image.format), 1, &out,
                                                     image.format);
                                return out;
                        }
        };

} // namespace

TEST_CASE("Pixels: every format from 0xAARRGGBB and back") {
        const uint32_t in[3] = {0xff102030u, 0x80ff0000u, 0x00123456u};
        uint8_t        out[12];
        uint32_t       back[3];
        bmds::Pixels::fromArgb(in, 3, out, PixelFormat::Bgra32, 0);
        bmds::Pixels::toArgb(out, 3, back, PixelFormat::Bgra32);
        CHECK(std::memcmp(in, back, sizeof(in)) == 0);
        bmds::Pixels::fromArgb(in, 3, out, PixelFormat::Bgr24, 0x0000ff);
        CHECK(out[0] == 0x30); // B, G, R
        CHECK(out[2] == 0x10);
        CHECK(out[3 + 2] == 0x80); // half red over blue
        CHECK(out[3 + 0] == 0x7f);
        CHECK(out[6] == 0xff); // clear: the background
        bmds::Pixels::fromArgb(in, 1, out, PixelFormat::Rgb24, 0);
        CHECK((out[0] == 0x10 && out[1] == 0x20 && out[2] == 0x30));
        bmds::Pixels::fromArgb(in, 1, out, PixelFormat::Rgb565, 0);
        bmds::Pixels::toArgb(out, 1, back, PixelFormat::Rgb565);
        CHECK(back[0] == 0xff102031u); // 5-6-5: each channel's top bits, repeated below
        bmds::Pixels::fromArgb(in, 1, out, PixelFormat::Gray8, 0);
        CHECK(out[0] == bmds::Pixels::luma(0xff102030u));
}

TEST_CASE("Png: 16-bit, low-bit grey and Adam7 interlacing, into every format") {
        const char *names[] = {"png-rgb16-key",
                               "png-rgba16",
                               "png-grey1",
                               "png-grey2",
                               "png-grey4",
                               "png-grey16",
                               "png-adam7-rgba-21x13",
                               "png-adam7-rgba-3x2",
                               "png-adam7-rgba-1x1",
                               "png-adam7-palette2",
                               "png-adam7-grey4"};
        for (const char *name : names) {
                INFO(std::string(name));
                const std::vector<uint8_t> file = readFile(std::string(name) + ".png");
                REQUIRE(file.size() > 8);
                REQUIRE(file[1] == 'P'); // an LFS pointer instead: 'git lfs pull'
                const std::vector<uint32_t> want = argbOf(name);
                auto [info, ierr] = bmds::Png::info(span(file));
                REQUIRE(ierr.isOk());
                CHECK(info.codec == bmds::ImageCodec::Png);
                REQUIRE(size_t(info.width) * info.height == want.size());
                CHECK(((info.flags & bmds::ImageInfo::Progressive) != 0) == (std::strstr(name, "adam7") != nullptr));
                for (unsigned f = 0; f < bmds::PixelFormatCount; ++f) {
                        const auto format = PixelFormat(f);
                        INFO(f);
                        Buffer b(info.width, info.height, format);
                        REQUIRE(bmds::Png::decode(span(file), b.image, 0x204060, bmds::MemSpace::Psram).isOk());
                        // Each pixel as the format keeps it.
                        for (uint32_t y = 0; y < info.height; ++y)
                                for (uint32_t x = 0; x < info.width; ++x) {
                                        uint8_t  one[4];
                                        uint32_t expect;
                                        bmds::Pixels::fromArgb(&want[y * info.width + x], 1, one, format, 0x204060);
                                        bmds::Pixels::toArgb(one, 1, &expect, format);
                                        if (b.argb(x, y) != expect) {
                                                CAPTURE(x);
                                                CAPTURE(y);
                                                CHECK(b.argb(x, y) == expect);
                                        }
                                }
                        // The padding untouched.
                        CHECK(b.bytes[info.width * bmds::bytesPerPixel(format)] == 0xcd);
                }
                Buffer small(info.width - (info.width > 1 ? 1 : 0), info.height, PixelFormat::Bgr24);
                if (info.width > 1)
                        CHECK(bmds::Png::decode(span(file), small.image, 0, bmds::MemSpace::Psram) ==
                              Error::BufferTooSmall);
        }
}

TEST_CASE("Bmp: palettes, 16, 24 and 32 bits, bit fields, RLE, both row orders, OS/2") {
        const char *names[] = {"bmp-1",  "bmp-4",          "bmp-8",  "bmp-8-os2",    "bmp-16-555", "bmp-16-565",
                               "bmp-24", "bmp-24-topdown", "bmp-32", "bmp-32-alpha", "bmp-rle8",   "bmp-rle4"};
        for (const char *name : names) {
                INFO(std::string(name));
                const std::vector<uint8_t> file = readFile(std::string(name) + ".bmp");
                REQUIRE(file.size() > 14);
                const std::vector<uint32_t> want = argbOf(name);
                auto [info, ierr] = bmds::Bmp::info(span(file));
                REQUIRE(ierr.isOk());
                CHECK(info.codec == bmds::ImageCodec::Bmp);
                REQUIRE(size_t(info.width) * info.height == want.size());
                CHECK(((info.flags & bmds::ImageInfo::HasAlpha) != 0) == (std::string(name) == "bmp-32-alpha"));
                Buffer b(info.width, info.height, PixelFormat::Bgra32);
                REQUIRE(bmds::Bmp::decode(span(file), b.image, 0).isOk());
                for (uint32_t y = 0; y < info.height; ++y)
                        for (uint32_t x = 0; x < info.width; ++x)
                                if (b.argb(x, y) != want[y * info.width + x]) {
                                        CAPTURE(x);
                                        CAPTURE(y);
                                        CHECK(b.argb(x, y) == want[y * info.width + x]);
                                }
        }
        // Cut short, or not a bitmap.
        std::vector<uint8_t> file = readFile("bmp-24.bmp");
        file.resize(file.size() - 10);
        CHECK(bmds::Bmp::info(span(file)).error() == Error::CorruptData);
        file[0] = 'X';
        CHECK(bmds::Bmp::info(span(file)).error() == Error::CorruptData);
}

TEST_CASE("JpegHeader: size, sampling, coding, restarts, EXIF orientation") {
        struct Want {
                        const char                *name;
                        bmds::JpegHeader::Sampling sampling;
                        bmds::JpegHeader::Coding   coding;
                        uint8_t                    orientation;
                        bool                       restart;
        };
        using S = bmds::JpegHeader::Sampling;
        using C = bmds::JpegHeader::Coding;
        const Want cases[] = {
                {"jpeg-444", S::Yuv444, C::Baseline, 1, false},
                {"jpeg-422", S::Yuv422, C::Baseline, 1, false},
                {"jpeg-420", S::Yuv420, C::Baseline, 1, false},
                {"jpeg-grey", S::Gray, C::Baseline, 1, false},
                {"jpeg-progressive", S::Yuv420, C::Progressive, 1, false},
                {"jpeg-restart", S::Yuv420, C::Baseline, 1, true},
                {"jpeg-exif6", S::Yuv420, C::Baseline, 6, false},
        };
        for (const Want &w : cases) {
                INFO(std::string(w.name));
                const std::vector<uint8_t> file = readFile(std::string(w.name) + ".jpg");
                REQUIRE(file.size() > 4);
                auto [h, err] = bmds::JpegHeader::parse(span(file));
                REQUIRE(err.isOk());
                CHECK(h.width == 61);
                CHECK(h.height == 37);
                CHECK(h.sampling == w.sampling);
                CHECK(h.coding == w.coding);
                CHECK(h.orientation == w.orientation);
                CHECK((h.restartInterval != 0) == w.restart);
                CHECK_FALSE(h.isHardware()); // 61 × 37 isn't a multiple of 8: ESP-IDF refuses it
                CHECK(h.isSoftware() == (w.coding == C::Baseline));
                CHECK(h.paddedWidth() == 64u); // 61 up to the MCU's 8 or 16
                CHECK(h.paddedHeight() == (w.sampling == S::Yuv420 ? 48u : 40u));
                CHECK(file[h.scanAt + 1] == 0xda);
                const bmds::ImageInfo info = h.info();
                CHECK(info.codec == bmds::ImageCodec::Jpeg);
                CHECK(((info.flags & bmds::ImageInfo::Progressive) != 0) == (w.coding == C::Progressive));
        }
        std::vector<uint8_t> file = readFile("jpeg-444.jpg");
        file.resize(200); // before the scan
        CHECK(bmds::JpegHeader::parse(span(file)).error() == Error::CorruptData);
        file[0] = 0;
        CHECK(bmds::JpegHeader::parse(span(file)).error() == Error::CorruptData);
}

TEST_CASE("ImageScaler: fit, down by averaging, up bilinear, the same size, orientations") {
        using bmds::ImageScaler::fit;
        CHECK(fit(4000, 3000, 1280, 720, false).width == 960);
        CHECK(fit(4000, 3000, 1280, 720, false).height == 720);
        CHECK(fit(100, 50, 1280, 720, false).width == 100); // never larger
        CHECK(fit(100, 50, 1280, 720, true).width == 1280);
        CHECK(fit(100, 50, 1280, 720, true).height == 640);
        CHECK(fit(1, 10000, 100, 100, false).width == 1); // at least 1

        // A checkerboard halved: every pixel the average.
        Buffer in(8, 6, PixelFormat::Bgr24);
        for (uint32_t y = 0; y < 6; ++y)
                for (uint32_t x = 0; x < 8; ++x) std::memset(in.image.row(y) + 3 * x, (x + y) % 2 ? 200 : 0, 3);
        Buffer half(4, 3, PixelFormat::Bgr24);
        REQUIRE(bmds::ImageScaler::scale(in.image, half.image, bmds::MemSpace::Psram).isOk());
        for (uint32_t y = 0; y < 3; ++y)
                for (uint32_t x = 0; x < 4; ++x) CHECK(half.argb(x, y) == 0xff646464u);
        // By 3 (not a whole number of pixels each): still the average, near enough.
        Buffer third(3, 2, PixelFormat::Bgr24);
        REQUIRE(bmds::ImageScaler::scale(in.image, third.image, bmds::MemSpace::Psram).isOk());
        for (uint32_t y = 0; y < 2; ++y)
                for (uint32_t x = 0; x < 3; ++x) {
                        const unsigned v = third.argb(x, y) & 0xff;
                        CHECK(v >= 85);
                        CHECK(v <= 115);
                }
        // Up: two pixels, 0 and 240, across 8: from 0 to 240 in steps, ends held.
        Buffer two(2, 1, PixelFormat::Gray8);
        two.image.row(0)[0] = 0;
        two.image.row(0)[1] = 240;
        Buffer wide(8, 3, PixelFormat::Gray8);
        REQUIRE(bmds::ImageScaler::scale(two.image, wide.image, bmds::MemSpace::Psram).isOk());
        const uint8_t *r = wide.image.row(1);
        CHECK(r[0] == 0);
        CHECK(r[1] == 0);
        CHECK(r[7] == 240);
        for (int x = 1; x < 8; ++x) CHECK(r[x] >= r[x - 1]);
        CHECK(r[4] > 100);
        CHECK(r[4] < 200);
        // A large reduction: 1000 × 1 of a ramp into 3.
        Buffer ramp(1000, 1, PixelFormat::Gray8);
        for (uint32_t x = 0; x < 1000; ++x) ramp.image.row(0)[x] = uint8_t(x * 255 / 999);
        Buffer three(3, 1, PixelFormat::Gray8);
        REQUIRE(bmds::ImageScaler::scale(ramp.image, three.image, bmds::MemSpace::Psram).isOk());
        CHECK(int(three.image.row(0)[1]) - 127 <= 1);
        CHECK(127 - int(three.image.row(0)[1]) <= 1);
        // The same size: copied; formats must agree.
        Buffer same(8, 6, PixelFormat::Bgr24);
        REQUIRE(bmds::ImageScaler::scale(in.image, same.image, bmds::MemSpace::Psram).isOk());
        CHECK(same.argb(1, 0) == in.argb(1, 0));
        Buffer other(4, 3, PixelFormat::Bgra32);
        CHECK(bmds::ImageScaler::scale(in.image, other.image, bmds::MemSpace::Psram) == Error::Invalid);

        // Orientations: a 3 × 2 picture of distinct pixels.
        Buffer pic(3, 2, PixelFormat::Gray8);
        for (uint8_t i = 0; i < 6; ++i) pic.image.row(i / 3)[i % 3] = i; // 0 1 2 / 3 4 5
        const char *upright[9] = {nullptr,  "012345", "210543", "543210", "345012",
                                  "031425", "304152", "524130", "251403"};
        for (uint8_t o = 1; o <= 8; ++o) {
                INFO(int(o));
                const auto s = bmds::ImageScaler::oriented(3, 2, o);
                Buffer     out(s.width, s.height, PixelFormat::Gray8);
                REQUIRE(bmds::ImageScaler::orient(pic.image, out.image, o).isOk());
                std::string got;
                for (uint32_t y = 0; y < s.height; ++y)
                        for (uint32_t x = 0; x < s.width; ++x) got += char('0' + out.image.row(y)[x]);
                CHECK(got == upright[o]);
        }
}

TEST_CASE("SoftJpeg: TJpgDec near libjpeg at full size, and reduced") {
        for (const char *name : {"jpeg-444", "jpeg-422", "jpeg-420", "jpeg-grey", "jpeg-restart", "jpeg-exif6"}) {
                INFO(std::string(name));
                const std::vector<uint8_t> file = readFile(std::string(name) + ".jpg");
                const std::vector<uint8_t> want = readFile(std::string(name) + ".rgb"); // R, G, B
                REQUIRE(want.size() == 61u * 37 * 3);
                Buffer b(61, 37, PixelFormat::Rgb24);
                REQUIRE(bmds::SoftJpeg::decode(span(file), b.image, 0, bmds::MemSpace::Psram).isOk());
                // IDCTs differ: near, not equal.
                long total = 0;
                int  most = 0;
                for (uint32_t y = 0; y < 37; ++y)
                        for (uint32_t x = 0; x < 61 * 3; ++x) {
                                const int d = std::abs(int(b.image.row(y)[x]) - int(want[(y * 61) * 3 + x]));
                                total += d;
                                most = d > most ? d : most;
                        }
                CHECK(double(total) / (61 * 37 * 3) < 2.0);
                CHECK(most <= 24);
                // Into the screen's order: the same pixels.
                Buffer bgr(61, 37, PixelFormat::Bgr24);
                REQUIRE(bmds::SoftJpeg::decode(span(file), bgr.image, 0, bmds::MemSpace::Psram).isOk());
                CHECK(bgr.image.row(5)[3 * 7] == b.image.row(5)[3 * 7 + 2]);
                // Reduced: 1/2 to 1/8, sizes as reduced() says, near the full picture's averages.
                for (unsigned shift = 1; shift <= 3; ++shift) {
                        INFO(shift);
                        const uint32_t w = bmds::SoftJpeg::reduced(61, shift), h = bmds::SoftJpeg::reduced(37, shift);
                        Buffer         r(w, h, PixelFormat::Rgb24);
                        REQUIRE(bmds::SoftJpeg::decode(span(file), r.image, shift, bmds::MemSpace::Psram).isOk());
                        Buffer avg(w, h, PixelFormat::Rgb24);
                        Buffer full(w << shift, h << shift, PixelFormat::Rgb24);
                        for (uint32_t y = 0; y < (h << shift); ++y)
                                std::memcpy(full.image.row(y), b.image.row(y), size_t(w << shift) * 3);
                        REQUIRE(bmds::ImageScaler::scale(full.image, avg.image, bmds::MemSpace::Psram).isOk());
                        long sum = 0;
                        for (uint32_t y = 0; y < h; ++y)
                                for (uint32_t x = 0; x < w * 3; ++x)
                                        sum += std::abs(int(r.image.row(y)[x]) - int(avg.image.row(y)[x]));
                        // At 1/8 it takes each block's DC alone, a subsampled chroma's over 2 × 2 pixels.
                        CHECK(double(sum) / (w * h * 3) < (shift == 3 ? 14.0 : 6.0));
                }
        }
        // Progressive: refused; a reduction too far; too small a buffer.
        const std::vector<uint8_t> progressive = readFile("jpeg-progressive.jpg");
        Buffer                     b(61, 37, PixelFormat::Rgb24);
        CHECK(bmds::SoftJpeg::decode(span(progressive), b.image, 0, bmds::MemSpace::Psram) == Error::NotSupported);
        const std::vector<uint8_t> file = readFile("jpeg-420.jpg");
        CHECK(bmds::SoftJpeg::decode(span(file), b.image, 4, bmds::MemSpace::Psram) == Error::Invalid);
        Buffer small(60, 37, PixelFormat::Rgb24);
        CHECK(bmds::SoftJpeg::decode(span(file), small.image, 0, bmds::MemSpace::Psram) == Error::BufferTooSmall);
}

TEST_CASE("ImagePlan: the codec when it can, TJpgDec reduced when it must") {
        using D = bmds::ImagePlan::Decoder;
        bmds::JpegHeader h;
        h.coding = bmds::JpegHeader::Coding::Baseline;
        h.components = 3;
        h.sampling = bmds::JpegHeader::Sampling::Yuv420;
        h.mcuWidth = h.mcuHeight = 16;
        h.width = 4000;
        h.height = 3000;
        bmds::ImagePlan::Want want;
        want.budgetPixels = 8'000'000;
        // 12 megapixels: over the budget; TJpgDec at 1/2 (3 MP fits).
        auto [p, err] = bmds::ImagePlan::make(h.info(), &h, want);
        REQUIRE(err.isOk());
        CHECK(p.decoder == D::Software);
        CHECK(p.shift == 1);
        CHECK(p.width == 2000);
        // For a 1280 × 720 window: 1/2 still (1/4 would be 1000 × 750, under 1280 across).
        want.minWidth = 1280;
        want.minHeight = 720;
        CHECK(bmds::ImagePlan::make(h.info(), &h, want).value().shift == 1);
        // For a 320 × 240 thumbnail: 1/8.
        want.minWidth = 320;
        want.minHeight = 240;
        CHECK(bmds::ImagePlan::make(h.info(), &h, want).value().shift == 3);
        // 1920 × 1080 fits: the codec, whole.
        h.width = 1920;
        h.height = 1080;
        want.minWidth = want.minHeight = 0;
        CHECK(bmds::ImagePlan::make(h.info(), &h, want).value().decoder == D::Hardware);
        // No codec: TJpgDec, whole.
        want.hardware = false;
        p = bmds::ImagePlan::make(h.info(), &h, want).value();
        CHECK(p.decoder == D::Software);
        CHECK(p.shift == 0);
        // Sides the codec refuses (61 × 37): TJpgDec.
        want.hardware = true;
        h.width = 61;
        h.height = 37;
        CHECK(bmds::ImagePlan::make(h.info(), &h, want).value().decoder == D::Software);
        // Progressive: nobody yet.
        h.coding = bmds::JpegHeader::Coding::Progressive;
        CHECK(bmds::ImagePlan::make(h.info(), &h, want).error() == Error::NotSupported);
        // Too big even at 1/8: 30000 × 30000 is 14 MP at 1/8.
        h.coding = bmds::JpegHeader::Coding::Baseline;
        h.width = h.height = 30000;
        CHECK(bmds::ImagePlan::make(h.info(), &h, want).error() == Error::OutOfRange);
        // PNG: whole, within the budget.
        bmds::ImageInfo png;
        png.codec = bmds::ImageCodec::Png;
        png.width = 2000;
        png.height = 2000;
        CHECK(bmds::ImagePlan::make(png, nullptr, want).value().decoder == D::Png);
        png.width = 5000;
        CHECK(bmds::ImagePlan::make(png, nullptr, want).error() == Error::OutOfRange);
}
