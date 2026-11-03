/**
 * @file      images.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Preso's pictures (devplan/preso.md §4, step 3): scaling, filters, a palette, drawing with
 * alpha, the cache, and where layout puts them. Pictures come from a platform that makes them.
 */

#include <doctest.h>

#include <stdlib.h>
#include <string.h>
#include "core/images.h"
#include "core/layout.h"
#include "headless/headless.h"

using preso::Canvas;
using preso::Color;
using preso::Deck;
using preso::DrawOp;
using preso::Error;
using preso::FontCache;
using preso::ImageCache;
using preso::Layouter;
using preso::OpType;
using preso::Palette;
using preso::Picture;
using preso::PixelFormat;
using preso::ScaledImage;
using preso::SlideLayout;
using preso::StringView;
using preso::Theme;
namespace ScaleFilter = preso::ScaleFilter;

namespace {

        // A picture made by a function of (x, y): 0xAARRGGBB.
        using Fill = uint32_t (*)(int32_t x, int32_t y);

        struct Made {
                        const char *name;
                        int32_t     width;
                        int32_t     height;
                        Fill        fill;
                        bool        alpha;
        };

        uint32_t red(int32_t, int32_t) {
                return 0xFFFF0000;
        }
        uint32_t checker(int32_t x, int32_t y) {
                return ((x + y) & 1) != 0 ? 0xFFFFFFFF : 0xFF000000;
        }
        uint32_t halfClear(int32_t x, int32_t) {
                return x < 2 ? 0x00000000 : 0xFF00FF00; // the left half transparent (black, but clear)
        }
        uint32_t ramp(int32_t x, int32_t) {
                uint32_t v = uint32_t(x * 255 / 99);
                return 0xFF000000 | (v << 16) | (v << 8) | v;
        }

        const Made Pictures[] = {
                {"red.png", 40, 20, red, false},      {"checker.png", 64, 64, checker, false},
                {"clear.png", 4, 4, halfClear, true}, {"ramp.png", 100, 10, ramp, false},
                {"tall.png", 100, 400, red, false},   {"wide.jpg", 1600, 900, red, false},
        };

        class PicturePlatform : public preso::HeadlessPlatform {
                public:
                        uint32_t decodes = 0;

                        const Made *find(const char *path) const {
                                const char *slash = strrchr(path, '/');
                                const char *name = slash != nullptr ? slash + 1 : path;
                                for (const Made &m : Pictures) {
                                        if (strcmp(m.name, name) == 0) return &m;
                                }
                                return nullptr;
                        }
                        Error imageSize(const char *path, int32_t *w, int32_t *h) override {
                                if (strstr(path, "broken") != nullptr) return Error::Invalid;
                                const Made *m = find(path);
                                if (m == nullptr) return Error::NotFound;
                                *w = m->width;
                                *h = m->height;
                                return Error::Ok;
                        }
                        Error decodeImage(const char *path, int32_t, int32_t, Picture *picture) override {
                                const Made *m = find(path);
                                if (m == nullptr) return Error::NotFound;
                                decodes++;
                                picture->width = m->width;
                                picture->height = m->height;
                                picture->stride = uint32_t(m->width) * 4;
                                picture->alpha = m->alpha;
                                picture->pixels =
                                        static_cast<uint32_t *>(allocatePixels(picture->stride * uint32_t(m->height)));
                                for (int32_t y = 0; y < m->height; y++) {
                                        for (int32_t x = 0; x < m->width; x++)
                                                picture->pixels[y * m->width + x] = m->fill(x, y);
                                }
                                return Error::Ok;
                        }
        };

        Picture make(const Made &m) {
                Picture p;
                p.width = m.width;
                p.height = m.height;
                p.stride = uint32_t(m.width) * 4;
                p.alpha = m.alpha;
                p.pixels = static_cast<uint32_t *>(malloc(p.stride * uint32_t(m.height)));
                for (int32_t y = 0; y < m.height; y++) {
                        for (int32_t x = 0; x < m.width; x++) p.pixels[y * m.width + x] = m.fill(x, y);
                }
                return p;
        }

        Color at(const ScaledImage &s, int32_t x, int32_t y) {
                const uint8_t *row = s.pixels + size_t(y) * s.stride;
                if (s.format == PixelFormat::Xrgb8888) return reinterpret_cast<const uint32_t *>(row)[x] & 0xFFFFFF;
                Canvas c; // for its RGB565 conversion
                return c.colorOf(reinterpret_cast<const uint16_t *>(row)[x]);
        }

        int channel(Color c, int shift) {
                return int((c >> shift) & 0xFF);
        }

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

        const DrawOp *imageOp(const SlideLayout *l, const char *path, int nth = 0) {
                for (const DrawOp *o = l->ops; o != nullptr; o = o->next) {
                        if (o->type == OpType::Image && o->image->path.equals(StringView(path)) && nth-- == 0) return o;
                }
                return nullptr;
        }

        // A deck laid out with pictures.
        struct Laid {
                        PicturePlatform platform;
                        FontCache       fonts{platform};
                        ImageCache      images{platform, "deck/talk.md"};
                        Deck            deck;
                        preso::Arena    arena;
                        preso::ThemeSet themes;
                        Layouter       *layouter = nullptr;

                        explicit Laid(const char *text) {
                                REQUIRE(parse(deck, text) == Error::Ok);
                                REQUIRE(themes.load(platform, deck.settings().theme, "deck/talk.md", nullptr, deck) ==
                                        Error::Ok);
                                layouter = new Layouter(deck, themes, fonts, platform, &images);
                        }
                        ~Laid() { delete layouter; }
                        SlideLayout *slide(uint32_t n) { return layouter->layout(*deck.slide(n), arena, true); }
        };

} // namespace

TEST_CASE("Images: a solid picture stays its colour, smaller or bigger") {
        Picture       p = make(Pictures[0]);
        const int32_t widths[] = {10, 40, 160};
        for (int32_t w : widths) {
                ScaledImage s;
                REQUIRE(preso::scalePicture(p, w, w / 2, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
                CHECK(s.alpha == nullptr);
                CHECK(at(s, 0, 0) == 0xFF0000);
                CHECK(at(s, w - 1, w / 2 - 1) == 0xFF0000);
                preso::freeScaled(s);
        }
        free(p.pixels);
}

TEST_CASE("Images: at its own size, a picture is its own pixels, a window of it too") {
        Picture     p = make(Pictures[1]);
        ScaledImage s;
        preso::Rect window{5, 7, 20, 9};
        REQUIRE(preso::scalePicture(p, 64, 64, &window, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        for (int32_t y = 0; y < 9; y++) {
                for (int32_t x = 0; x < 20; x++) CHECK(at(s, x, y) == (checker(x + 5, y + 7) & 0xFFFFFF));
        }
        preso::freeScaled(s);
        free(p.pixels);
}

TEST_CASE("Images: halved, a checkerboard averages to grey; enlarged, a ramp stays a ramp") {
        Picture     p = make(Pictures[1]);
        ScaledImage s;
        REQUIRE(preso::scalePicture(p, 32, 32, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        for (int32_t x = 0; x < 32; x++) {
                int v = channel(at(s, x, 7), 0);
                CHECK(v >= 126);
                CHECK(v <= 129);
        }
        preso::freeScaled(s);
        free(p.pixels);
        Picture r = make(Pictures[3]);
        REQUIRE(preso::scalePicture(r, 300, 10, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        int last = -1;
        for (int32_t x = 0; x < 300; x++) {
                int v = channel(at(s, x, 5), 8);
                CHECK(v >= last);
                last = v;
        }
        CHECK(channel(at(s, 0, 0), 8) <= 2);
        CHECK(channel(at(s, 299, 0), 8) >= 253);
        preso::freeScaled(s);
        free(r.pixels);
}

TEST_CASE("Images: transparency is kept, and drawn over what is there") {
        Picture     p = make(Pictures[2]);
        ScaledImage s;
        REQUIRE(preso::scalePicture(p, 4, 4, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        REQUIRE(s.alpha != nullptr);
        CHECK(s.alpha[0] == 0);
        CHECK(s.alpha[3] == 255);
        uint32_t pixels[4 * 4];
        Canvas   c;
        c.attach(pixels, 16, 4, 4, PixelFormat::Xrgb8888);
        c.fill(preso::Rect{0, 0, 4, 4}, 0x0000FF);
        preso::blitImage(c, s, 0, 0);
        CHECK(c.colorAt(0, 0) == 0x0000FF); // clear: the blue shows
        CHECK(c.colorAt(3, 0) == 0x00FF00); // opaque: green
        preso::freeScaled(s);
        // A window of a picture: only that part made, the same pixels as in the whole.
        Picture     r = make(Pictures[3]);
        ScaledImage whole, part;
        preso::Rect window{150, 2, 40, 3};
        REQUIRE(preso::scalePicture(r, 300, 10, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &whole) == Error::Ok);
        REQUIRE(preso::scalePicture(r, 300, 10, &window, 0, nullptr, PixelFormat::Xrgb8888, &part) == Error::Ok);
        CHECK(part.width == 40);
        CHECK(part.height == 3);
        for (int32_t x = 0; x < 40; x++) CHECK(at(part, x, 1) == at(whole, x + 150, 3));
        preso::Rect outside{290, 0, 20, 1};
        ScaledImage none;
        CHECK(preso::scalePicture(r, 300, 10, &outside, 0, nullptr, PixelFormat::Xrgb8888, &none) == Error::Invalid);
        preso::freeScaled(whole);
        preso::freeScaled(part);
        free(r.pixels);
        // Halved, the edge is half green over what is there, not darkened by the clear black.
        REQUIRE(preso::scalePicture(p, 2, 2, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        CHECK(s.alpha[0] == 0);
        CHECK(s.alpha[1] == 255);
        preso::freeScaled(s);
        REQUIRE(preso::scalePicture(p, 1, 1, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        CHECK(s.alpha[0] >= 127);
        CHECK(s.alpha[0] <= 128);
        CHECK(channel(at(s, 0, 0), 8) == s.alpha[0]); // premultiplied: green as strong as it is opaque
        preso::freeScaled(s);
        free(p.pixels);
}

TEST_CASE("Images: a picture without alpha is opaque, whatever its alpha bytes say") {
        Picture p = make(Pictures[0]);
        for (int32_t i = 0; i < p.width * p.height; i++) p.pixels[i] &= 0x00FFFFFF; // a decoder that left them 0
        ScaledImage s;
        REQUIRE(preso::scalePicture(p, 10, 5, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        CHECK(s.alpha == nullptr);
        CHECK(at(s, 3, 3) == 0xFF0000);
        preso::freeScaled(s);
        free(p.pixels);
}

TEST_CASE("Images: a picture as bytes B, G, R (a JPEG decoder's) scales as BGRA does") {
        Picture p;
        p.width = 2;
        p.height = 1;
        p.stride = 8; // padded, as a codec's rows may be
        p.layout = Picture::Layout::Bgr24;
        uint8_t bytes[8] = {0x10, 0x20, 0xF0, 0x10, 0x20, 0xF0, 0, 0}; // B, G, R twice
        p.pixels = reinterpret_cast<uint32_t *>(bytes);
        ScaledImage s;
        REQUIRE(preso::scalePicture(p, 4, 2, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        CHECK(s.alpha == nullptr);
        CHECK(at(s, 1, 1) == 0xF02010);
        preso::freeScaled(s);
}

TEST_CASE("Images: a picture as RGB565 words scales as BGRA does") {
        Picture  p;
        uint16_t words[2] = {0xF800, 0xF800}; // red
        p.width = 2;
        p.height = 1;
        p.stride = 4;
        p.layout = Picture::Layout::Rgb565;
        p.pixels = reinterpret_cast<uint32_t *>(words);
        ScaledImage s;
        REQUIRE(preso::scalePicture(p, 3, 3, nullptr, 0, nullptr, PixelFormat::Xrgb8888, &s) == Error::Ok);
        CHECK(at(s, 2, 2) == 0xFF0000);
        preso::freeScaled(s);
}

TEST_CASE("Images: the plain paths to RGB565 (own size and scaled, BGR24 and RGB565) agree") {
        // A ramp as BGR24 and as RGB565, at its own size and halved.
        uint8_t  bgr[64 * 3];
        uint16_t words[64];
        for (int i = 0; i < 64; i++) {
                uint8_t v = uint8_t(i * 4);
                bgr[i * 3] = bgr[i * 3 + 1] = bgr[i * 3 + 2] = v;
                words[i] = uint16_t(((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3));
        }
        Picture b;
        b.width = 64;
        b.height = 1;
        b.stride = 64 * 3;
        b.layout = Picture::Layout::Bgr24;
        b.pixels = reinterpret_cast<uint32_t *>(bgr);
        Picture w = b;
        w.stride = 128;
        w.layout = Picture::Layout::Rgb565;
        w.pixels = reinterpret_cast<uint32_t *>(words);
        const int32_t sizes[] = {64, 32};
        for (int32_t size : sizes) {
                ScaledImage fromBgr, from565;
                REQUIRE(preso::scalePicture(b, size, 1, nullptr, 0, nullptr, PixelFormat::Rgb565, &fromBgr) ==
                        Error::Ok);
                REQUIRE(preso::scalePicture(w, size, 1, nullptr, 0, nullptr, PixelFormat::Rgb565, &from565) ==
                        Error::Ok);
                for (int32_t x = 0; x < size; x++) {
                        int want = x * 4 * (64 / size);
                        CHECK(channel(at(fromBgr, x, 0), 8) >= want - 4);
                        CHECK(channel(at(fromBgr, x, 0), 8) <= want + 8);
                        CHECK(channel(at(from565, x, 0), 8) >= want - 6);
                        CHECK(channel(at(from565, x, 0), 8) <= want + 8);
                }
                if (size == 64) {
                        // Its own size from RGB565: the words themselves.
                        for (int32_t x = 0; x < 64; x++)
                                CHECK(reinterpret_cast<uint16_t *>(from565.pixels)[x] == words[x]);
                }
                preso::freeScaled(fromBgr);
                preso::freeScaled(from565);
        }
}

TEST_CASE("Images: grayscale and sepia") {
        Picture     p = make(Pictures[0]);
        ScaledImage s;
        REQUIRE(preso::scalePicture(p, 4, 2, nullptr, ScaleFilter::Grayscale, nullptr, PixelFormat::Xrgb8888, &s) ==
                Error::Ok);
        Color g = at(s, 1, 1);
        CHECK(channel(g, 16) == channel(g, 8));
        CHECK(channel(g, 8) == channel(g, 0));
        CHECK(channel(g, 0) == 76); // red's luma
        preso::freeScaled(s);
        REQUIRE(preso::scalePicture(p, 4, 2, nullptr, ScaleFilter::Sepia, nullptr, PixelFormat::Xrgb8888, &s) ==
                Error::Ok);
        Color sepia = at(s, 1, 1);
        CHECK(channel(sepia, 16) > channel(sepia, 8));
        CHECK(channel(sepia, 8) > channel(sepia, 0));
        preso::freeScaled(s);
        free(p.pixels);
}

TEST_CASE("Images: mapped to a palette, every pixel is one of its colours, dithered between them") {
        static const Color ega[] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                                    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
        Palette            palette{ega, 16};
        Picture            p = make(Pictures[3]);
        ScaledImage        s;
        REQUIRE(preso::scalePicture(p, 100, 10, nullptr, 0, &palette, PixelFormat::Xrgb8888, &s) == Error::Ok);
        uint32_t used = 0;
        for (int32_t y = 0; y < 10; y++) {
                for (int32_t x = 0; x < 100; x++) {
                        Color c = at(s, x, y);
                        bool  in = false;
                        for (Color e : ega) in = in || e == c;
                        CHECK(in);
                }
        }
        // The grey ramp through the palette's greys (black, 555555, AAAAAA, white), mixed between them.
        for (int32_t x = 0; x < 100; x++) {
                Color c = at(s, x, 4);
                for (int i = 0; i < 16; i++) {
                        if (ega[i] == c) used |= 1u << i;
                }
        }
        CHECK((used & ((1u << 0) | (1u << 8) | (1u << 7) | (1u << 15))) ==
              ((1u << 0) | (1u << 8) | (1u << 7) | (1u << 15)));
        // A grey halfway between two of the palette's (128: 0x55 and 0xAA) is dithered: both in a few pixels.
        bool dark = false, light = false;
        for (int32_t y = 0; y < 8; y++) {
                for (int32_t x = 48; x < 53; x++) {
                        dark = dark || at(s, x, y) == 0x555555;
                        light = light || at(s, x, y) == 0xAAAAAA;
                }
        }
        CHECK(dark);
        CHECK(light);
        preso::freeScaled(s);
        free(p.pixels);
}

TEST_CASE("Images: RGB565, dithered, stays close") {
        Picture     p = make(Pictures[3]);
        ScaledImage s;
        REQUIRE(preso::scalePicture(p, 100, 10, nullptr, 0, nullptr, PixelFormat::Rgb565, &s) == Error::Ok);
        for (int32_t x = 0; x < 100; x += 9) {
                int want = x * 255 / 99;
                int got = channel(at(s, x, 3), 8);
                CHECK(got >= want - 6);
                CHECK(got <= want + 10);
        }
        preso::freeScaled(s);
        free(p.pixels);
}

TEST_CASE("Images: the cache keeps a scaled picture, and drops the oldest past its budget") {
        PicturePlatform platform;
        ImageCache      cache(platform, "deck/talk.md");
        int32_t         w = 0, h = 0;
        CHECK(cache.size("red.png", &w, &h) == Error::Ok);
        CHECK(w == 40);
        CHECK(h == 20);
        CHECK(cache.size("missing.png", &w, &h) == Error::NotFound);
        CHECK(cache.size("broken.png", &w, &h) == Error::Invalid);
        const ScaledImage *a = cache.get("red.png", 20, 10, preso::Rect{0, 0, 20, 10}, 0, nullptr, PixelFormat::Rgb565);
        REQUIRE(a != nullptr);
        CHECK(cache.get("red.png", 20, 10, preso::Rect{0, 0, 20, 10}, 0, nullptr, PixelFormat::Rgb565) == a);
        CHECK(platform.decodes == 1);
        CHECK(cache.get("red.png", 20, 10, preso::Rect{0, 0, 20, 10}, ScaleFilter::Sepia, nullptr,
                        PixelFormat::Rgb565) != a);
        CHECK(platform.decodes == 2);
        CHECK(cache.get("missing.png", 20, 10, preso::Rect{0, 0, 20, 10}, 0, nullptr, PixelFormat::Rgb565) == nullptr);
        // 1280×720 at 2 bytes is 1.8 MB: three of them don't fit in 4 MB together.
        for (int i = 0; i < 6; i++)
                REQUIRE(cache.get("wide.jpg", 1280, 720 - i, preso::Rect{0, 0, 1280, 720 - i}, 0, nullptr,
                                  PixelFormat::Rgb565) != nullptr);
        CHECK(cache.bytesKept() <= ImageCache::MaxBytes);
        cache.clear();
        CHECK(cache.bytesKept() == 0);
}

TEST_CASE("Layout: inline pictures at their own size, or in proportion to w: or h:") {
        Laid         l("![](red.png) ![w:80](red.png) ![h:60](red.png) ![w:10 h:10](red.png)\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        const DrawOp *own = imageOp(s, "red.png", 0), *w = imageOp(s, "red.png", 1), *h = imageOp(s, "red.png", 2),
                     *both = imageOp(s, "red.png", 3);
        REQUIRE(both != nullptr);
        CHECK(own->width == 40);
        CHECK(own->height == 20);
        CHECK(w->width == 80);
        CHECK(w->height == 40);
        CHECK(h->width == 120);
        CHECK(h->height == 60);
        CHECK(both->width == 10);
        CHECK(both->height == 10);
        // On one baseline: the bottoms meet.
        CHECK(own->y + own->height == h->y + h->height);
        CHECK(l.deck.diagnosticCount() == 0);
}

TEST_CASE("Layout: a background covers the slide by default; contain, auto and a percentage") {
        Laid          l("![bg](tall.png)\n\n---\n\n![bg contain](tall.png)\n\n---\n\n![bg auto](tall.png)\n\n---\n\n"
                                 "![bg 50%](tall.png)\n");
        const DrawOp *cover = imageOp(l.slide(1), "tall.png");
        REQUIRE(cover != nullptr);
        CHECK(cover->width == 1280);
        CHECK(cover->height == 5120); // only the middle 720 rows of it are ever scaled
        CHECK(cover->clip.width == 1280);
        CHECK(cover->clip.height == 720);
        const DrawOp *contain = imageOp(l.slide(2), "tall.png");
        CHECK(contain->height == 720);
        CHECK(contain->width == 180);
        CHECK(contain->x == (1280 - 180) / 2);
        const DrawOp *natural = imageOp(l.slide(3), "tall.png");
        CHECK(natural->width == 100);
        CHECK(natural->height == 400);
        const DrawOp *half = imageOp(l.slide(4), "tall.png");
        CHECK(half->width == 50);
        CHECK(half->height == 200);
        // Backgrounds are behind: drawn first, before the slide's text.
        CHECK(l.slide(1)->ops->type == OpType::Image);
}

TEST_CASE("Layout: several backgrounds share the slide, or its split, across or down") {
        Laid l("![bg](red.png)\n![bg](red.png)\n\n---\n\n![bg right:40% vertical](red.png)\n![bg](red.png)\n\ntext\n");
        const DrawOp *a = imageOp(l.slide(1), "red.png", 0), *b = imageOp(l.slide(1), "red.png", 1);
        REQUIRE(b != nullptr);
        CHECK(a->clip.x == 0);
        CHECK(a->clip.width == 640);
        CHECK(b->clip.x == 640);
        SlideLayout  *split = l.slide(2);
        const DrawOp *top = imageOp(split, "red.png", 0), *bottom = imageOp(split, "red.png", 1);
        REQUIRE(bottom != nullptr);
        CHECK(top->clip.x == 1280 - 512);
        CHECK(top->clip.width == 512);
        CHECK(top->clip.height == 360);
        CHECK(bottom->clip.y == 360);
        // The text on the other side.
        for (const DrawOp *o = split->ops; o != nullptr; o = o->next) {
                if (o->type == OpType::Text) CHECK(o->x + o->width <= 1280 - 512);
        }
}

TEST_CASE("Layout: backgroundImage, and pictures that can't be shown") {
        Laid l("<!-- _backgroundImage: url('red.png') -->\n# A\n\n---\n\n![bg](missing.png)\n\n![](broken.png)\n\n"
               "![](https://example.com/x.png)\n");
        const DrawOp *bg = imageOp(l.slide(1), "red.png");
        REQUIRE(bg != nullptr);
        CHECK(bg->clip.width == 1280);
        SlideLayout *two = l.slide(2);
        REQUIRE(two != nullptr);
        CHECK(reported(l.deck, "image `missing.png` isn't there"));
        CHECK(reported(l.deck, "image `broken.png` isn't a picture"));
        CHECK(reported(l.deck, "is on the web"));
        uint32_t frames = 0;
        for (const DrawOp *o = two->ops; o != nullptr; o = o->next) frames += o->type == OpType::Frame;
        CHECK(frames == 3);
}

TEST_CASE("Layout: drawn, a slide's pictures come from the cache, dropped where clipped") {
        Laid         l("![bg left](red.png)\n\nword\n");
        SlideLayout *s = l.slide(1);
        REQUIRE(s != nullptr);
        static uint16_t pixels[1280 * 720];
        Canvas          c;
        c.attach(pixels, 1280 * 2, 1280, 720, PixelFormat::Rgb565);
        preso::drawSlide(*s, 0, c, l.fonts, &l.images);
        CHECK(c.colorAt(10, 10) == 0xFF0000);
        CHECK(c.colorAt(639, 719) == 0xFF0000);
        CHECK(c.colorAt(700, 10) == Theme::paper().background);
        CHECK(l.platform.decodes == 1);
        preso::drawSlide(*s, 0, c, l.fonts, &l.images);
        CHECK(l.platform.decodes == 1); // the second time, from the cache
}

TEST_CASE("Layout: a picture tiled, filtered and stacked is drawn in each of its tiles") {
        Laid            l("![bg contain](tall.png)\n![bg sepia](wide.jpg)\n![bg grayscale](wide.jpg)\n\n---\n\n"
                                     "![bg left vertical](wide.jpg)\n![bg](red.png)\n\n# Stacked\n");
        static uint16_t pixels[1280 * 720];
        Canvas          c;
        c.attach(pixels, 1280 * 2, 1280, 720, PixelFormat::Rgb565);
        for (uint32_t n = 1; n <= 2; n++) {
                SlideLayout *s = l.slide(n);
                REQUIRE(s != nullptr);
                preso::drawSlide(*s, 0, c, l.fonts, &l.images);
                for (const DrawOp *o = s->ops; o != nullptr; o = o->next) {
                        if (o->type != OpType::Image) continue;
                        preso::Rect shown = preso::Rect{o->x, o->y, o->width, o->height}.intersected(o->clip);
                        preso::Rect window{shown.x - o->x, shown.y - o->y, shown.width, shown.height};
                        INFO("slide ", n, " ", o->image->path.data(), " at ", o->x, ",", o->y, " ", o->width, "x",
                             o->height);
                        CHECK(l.images.get(o->image->path, o->width, o->height, window, o->alpha, nullptr,
                                           PixelFormat::Rgb565) != nullptr);
                }
        }
}

TEST_CASE("Layout: a picture far bigger than drawn is said to be") {
        Laid l("![w:400](wide.jpg)\n\n---\n\n![bg](wide.jpg)\n");
        REQUIRE(l.slide(1) != nullptr);
        CHECK(reported(l.deck, "image `wide.jpg` is 1600x900 but drawn 400x225"));
        uint32_t before = l.deck.diagnosticCount();
        REQUIRE(l.slide(2) != nullptr); // 1600×900 covering 1280×720: not that much bigger
        CHECK(l.deck.diagnosticCount() == before);
}

TEST_CASE("Layout: pictures at extreme sizes are drawn without trouble") {
        Laid            l("![bg 1000%](tiny.png)\n\n---\n\n![w:8000 h:1](red.png) ![w:1 h:1](red.png) "
                                     "![h:5000](tall.png)\n\n---\n\n"
                                     "![bg 1%](wide.jpg)\n![bg left:99% vertical](checker.png)\n![bg](clear.png)\n![bg contain](tall.png)\n");
        static uint16_t pixels[1280 * 720];
        Canvas          c;
        c.attach(pixels, 1280 * 2, 1280, 720, PixelFormat::Rgb565);
        for (uint32_t n = 1; n <= 3; n++) {
                SlideLayout *s = l.slide(n);
                REQUIRE(s != nullptr);
                preso::drawSlide(*s, 0, c, l.fonts, &l.images);
                for (const DrawOp *o = s->ops; o != nullptr; o = o->next) {
                        if (o->type == OpType::Image) CHECK(o->width >= 1);
                }
        }
        CHECK(l.images.bytesKept() <= ImageCache::MaxBytes);
}
