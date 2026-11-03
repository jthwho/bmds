/**
 * @file      images.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Scaling is separable and done a row at a time: each output row is a weighted sum of a few
 * source rows, each already scaled across, kept in a small ring. So a big picture never needs
 * a second copy of itself, only as many scaled rows as one output row reads. Weights are 16.16
 * fixed point, colours premultiplied by alpha so transparent pixels don't darken the edges
 * around them.
 */

#include "images.h"
#include <stdio.h>
#include <stdlib.h>

namespace preso {

        namespace {

                // ---- Kernels: for each output index, its source indices and their weights (summing to 65536).

                struct Kernel {
                                uint32_t *first = nullptr;
                                uint32_t *weights = nullptr; // `taps` a row
                                uint32_t  taps = 0;

                                ~Kernel() {
                                        free(first);
                                        free(weights);
                                }
                };

                // Output indices [from, from + count) of a source of @p source scaled to @p target.
                bool makeKernel(int32_t source, int32_t target, int32_t from, int32_t count, Kernel &k) {
                        if (target < source) {
                                // Down: each output pixel the average of the source it covers.
                                float scale = float(source) / float(target);
                                k.taps = uint32_t(scale) + 2;
                        } else {
                                k.taps = 2; // up: bilinear
                        }
                        k.first = static_cast<uint32_t *>(malloc(size_t(count) * sizeof(uint32_t)));
                        k.weights = static_cast<uint32_t *>(calloc(size_t(count) * k.taps, sizeof(uint32_t)));
                        if (k.first == nullptr || k.weights == nullptr) return false;
                        for (int32_t n = 0; n < count; n++) {
                                int32_t   i = from + n;
                                uint32_t *w = k.weights + size_t(n) * k.taps;
                                if (target < source) {
                                        float    scale = float(source) / float(target);
                                        float    a = float(i) * scale, b = float(i + 1) * scale;
                                        int32_t  first = int32_t(a);
                                        uint32_t sum = 0, biggest = 0;
                                        k.first[n] = uint32_t(first);
                                        for (uint32_t t = 0; t < k.taps; t++) {
                                                int32_t j = first + int32_t(t);
                                                if (j >= source) break;
                                                float lo = float(j) > a ? float(j) : a;
                                                float hi = float(j + 1) < b ? float(j + 1) : b;
                                                if (hi <= lo) continue;
                                                w[t] = uint32_t((hi - lo) / scale * 65536.0f);
                                                sum += w[t];
                                                if (w[t] > w[biggest]) biggest = t;
                                        }
                                        w[biggest] += 65536 - sum; // rounding: the weights sum to one
                                } else {
                                        float   x = (float(i) + 0.5f) * float(source) / float(target) - 0.5f;
                                        int32_t j = x < 0.0f ? 0 : int32_t(x);
                                        float   f = x - float(j);
                                        if (f < 0.0f) f = 0.0f;
                                        if (j >= source - 1) {
                                                j = source - 1;
                                                f = 0.0f;
                                        }
                                        k.first[n] = uint32_t(j);
                                        w[1] = uint32_t(f * 65536.0f);
                                        w[0] = 65536 - w[1];
                                }
                        }
                        return true;
                }

                constexpr uint8_t Bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

                constexpr uint8_t Bayer8[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
                                                  {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
                                                  {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
                                                  {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};

                // Pixel @p j of a source row, as 0xAARRGGBB (a template: no switch a pixel).
                template <Picture::Layout L> inline uint32_t pixelAt(const uint8_t *row, uint32_t j) {
                        if constexpr (L == Picture::Layout::Bgr24) {
                                const uint8_t *q = row + size_t(j) * 3;
                                return 0xFF000000u | (uint32_t(q[2]) << 16) | (uint32_t(q[1]) << 8) | q[0];
                        } else if constexpr (L == Picture::Layout::Rgb565) {
                                uint32_t v = reinterpret_cast<const uint16_t *>(row)[j];
                                uint32_t r5 = v >> 11, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
                                return 0xFF000000u | (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8) |
                                       ((b5 << 3) | (b5 >> 2));
                        } else {
                                return reinterpret_cast<const uint32_t *>(row)[j];
                        }
                }

                // A source row scaled across: four channels, premultiplied, 8.8 fixed point. A picture
                // without alpha is opaque whatever its alpha bytes hold (a decoder may leave them 0),
                // and needs no premultiplying (three divisions a sample saved).
                template <Picture::Layout L, bool Alpha>
                void scaleRowT(const uint8_t *row, int32_t sourceWidth, const Kernel &k, int32_t width, uint16_t *out) {
                        for (int32_t i = 0; i < width; i++) {
                                const uint32_t *w = k.weights + size_t(i) * k.taps;
                                uint32_t        first = k.first[i];
                                uint32_t        r = 0, g = 0, b = 0, a = 0;
                                for (uint32_t t = 0; t < k.taps; t++) {
                                        if (w[t] == 0) continue;
                                        uint32_t j = first + t;
                                        if (j >= uint32_t(sourceWidth)) break;
                                        uint32_t p = pixelAt<L>(row, j);
                                        uint32_t pa = 255, pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
                                        if constexpr (Alpha) {
                                                // Premultiplied: a colour weighs as much as it is opaque.
                                                pa = p >> 24;
                                                pr = pr * pa / 255;
                                                pg = pg * pa / 255;
                                                pb = pb * pa / 255;
                                        }
                                        r += w[t] * pr;
                                        g += w[t] * pg;
                                        b += w[t] * pb;
                                        a += w[t] * pa;
                                }
                                uint16_t *o = out + size_t(i) * 4;
                                o[0] = uint16_t(r >> 8);
                                o[1] = uint16_t(g >> 8);
                                o[2] = uint16_t(b >> 8);
                                o[3] = uint16_t(a >> 8);
                        }
                        return;
                }

                void scaleRow(const uint8_t *row, Picture::Layout layout, int32_t sourceWidth, const Kernel &k,
                              int32_t width, bool alpha, uint16_t *out) {
                        if (layout == Picture::Layout::Bgr24) {
                                scaleRowT<Picture::Layout::Bgr24, false>(row, sourceWidth, k, width, out);
                        } else if (layout == Picture::Layout::Rgb565) {
                                scaleRowT<Picture::Layout::Rgb565, false>(row, sourceWidth, k, width, out);
                        } else if (alpha) {
                                scaleRowT<Picture::Layout::Bgra32, true>(row, sourceWidth, k, width, out);
                        } else {
                                scaleRowT<Picture::Layout::Bgra32, false>(row, sourceWidth, k, width, out);
                        }
                        return;
                }

                // RGB565 nudged by the 4×4 threshold map's @p t (a step of RGB565 is 8, green's 4): the
                // map breaks up the bands a smooth picture would show.
                inline uint16_t dithered565(int32_t r, int32_t g, int32_t b, int32_t t) {
                        r += t >> 1;
                        g += t >> 2;
                        b += t >> 1;
                        if (r > 255) r = 255;
                        if (g > 255) g = 255;
                        if (b > 255) b = 255;
                        return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                }

                uint8_t clamp8(int32_t v) {
                        return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
                }

                Color nearest(const Palette &palette, int32_t r, int32_t g, int32_t b) {
                        Color    best = palette.colors[0];
                        uint32_t bestDistance = UINT32_MAX;
                        for (uint32_t i = 0; i < palette.count; i++) {
                                Color   c = palette.colors[i];
                                int32_t dr = int32_t((c >> 16) & 0xFF) - r, dg = int32_t((c >> 8) & 0xFF) - g,
                                        db = int32_t(c & 0xFF) - b;
                                // Weighted as the eye is: green counts most.
                                uint32_t d = uint32_t(3 * dr * dr + 4 * dg * dg + 2 * db * db);
                                if (d < bestDistance) {
                                        bestDistance = d;
                                        best = c;
                                }
                        }
                        return best;
                }

        } // namespace

        Error scalePicture(const Picture &picture, int32_t fullWidth, int32_t fullHeight, const Rect *window,
                           uint8_t filters, const Palette *palette, PixelFormat format, ScaledImage *out) {
                *out = ScaledImage();
                Rect whole{0, 0, fullWidth, fullHeight};
                Rect part = window != nullptr ? *window : whole;
                if (fullWidth <= 0 || fullHeight <= 0 || fullWidth > 65536 || fullHeight > 65536 ||
                    picture.width <= 0 || picture.height <= 0 || picture.pixels == nullptr || part.isEmpty() ||
                    part.width > 8192 || part.height > 8192 || part.intersected(whole).width != part.width ||
                    part.intersected(whole).height != part.height) {
                        return Error::Invalid;
                }
                int32_t width = part.width, height = part.height;
                Kernel  across, down;
                if (!makeKernel(picture.width, fullWidth, part.x, width, across) ||
                    !makeKernel(picture.height, fullHeight, part.y, height, down)) {
                        return Error::NoMemory;
                }
                // The ring of source rows scaled across: as many as an output row reads.
                uint32_t ringRows = down.taps + 1;
                auto *ring = static_cast<uint16_t *>(malloc(size_t(ringRows) * size_t(width) * 4 * sizeof(uint16_t)));
                auto *ringRow = static_cast<int32_t *>(malloc(ringRows * sizeof(int32_t)));
                auto *rows = static_cast<const uint16_t **>(malloc(down.taps * sizeof(uint16_t *)));
                uint32_t bpp = bytesPerPixel(format);
                out->width = width;
                out->height = height;
                out->format = format;
                out->stride = uint32_t(width) * bpp;
                out->pixels = static_cast<uint8_t *>(malloc(size_t(out->stride) * size_t(height)));
                out->alpha = picture.alpha ? static_cast<uint8_t *>(malloc(size_t(width) * size_t(height))) : nullptr;
                if (ring == nullptr || ringRow == nullptr || rows == nullptr || out->pixels == nullptr ||
                    (picture.alpha && out->alpha == nullptr)) {
                        free(ring);
                        free(ringRow);
                        free(rows);
                        freeScaled(*out);
                        return Error::NoMemory;
                }
                bool opaque = true;
                // A pixel finished (premultiplied R, G, B, A): filtered, mapped or dithered, stored.
                auto put = [&](int32_t x, int32_t y, int32_t R, int32_t G, int32_t B, int32_t A) {
                        uint8_t *row = out->pixels + size_t(y) * out->stride;
                        if ((filters & ScaleFilter::Grayscale) != 0) {
                                R = G = B = (77 * R + 150 * G + 29 * B) >> 8;
                        }
                        if ((filters & ScaleFilter::Sepia) != 0) {
                                int32_t sr = (101 * R + 197 * G + 48 * B) >> 8, sg = (89 * R + 176 * G + 43 * B) >> 8,
                                        sb = (70 * R + 137 * G + 34 * B) >> 8;
                                R = sr > A ? A : sr;
                                G = sg > A ? A : sg;
                                B = sb > A ? A : sb;
                        }
                        if (palette != nullptr && palette->count > 0 && A > 0) {
                                // Ordered dithering to the palette: the colour (unpremultiplied) nudged
                                // by the threshold map, then the nearest of the palette.
                                int32_t ur = R * 255 / A, ug = G * 255 / A, ub = B * 255 / A;
                                int32_t t = (int32_t(Bayer8[(y + part.y) & 7][(x + part.x) & 7]) - 32) * 3 / 2;
                                Color   c = nearest(*palette, clamp8(ur + t), clamp8(ug + t), clamp8(ub + t));
                                R = int32_t((c >> 16) & 0xFF) * A / 255;
                                G = int32_t((c >> 8) & 0xFF) * A / 255;
                                B = int32_t(c & 0xFF) * A / 255;
                        }
                        if (format == PixelFormat::Rgb565) {
                                // Dithered to RGB565 (a palette's colours are kept as they are).
                                int32_t t = palette == nullptr || palette->count == 0
                                                    ? Bayer4[(y + part.y) & 3][(x + part.x) & 3]
                                                    : 0;
                                reinterpret_cast<uint16_t *>(row)[x] = dithered565(R, G, B, t);
                        } else {
                                reinterpret_cast<uint32_t *>(row)[x] =
                                        (uint32_t(R) << 16) | (uint32_t(G) << 8) | uint32_t(B);
                        }
                        if (out->alpha != nullptr) {
                                out->alpha[size_t(y) * size_t(width) + size_t(x)] = uint8_t(A);
                                if (A != 255) opaque = false;
                        }
                };
                bool alpha = picture.alpha && picture.layout == Picture::Layout::Bgra32;
                // The usual case: opaque, unfiltered, its own colours, to RGB565. Its loops do no work a
                // pixel doesn't need: through put() a slide-sized picture took 314 ms on the board.
                bool plain = !alpha && filters == 0 && (palette == nullptr || palette->count == 0) &&
                             format == PixelFormat::Rgb565;
                bool ownSize = picture.width == fullWidth && picture.height == fullHeight;
                for (uint32_t i = 0; i < ringRows; i++) ringRow[i] = -1;
                for (int32_t y = 0; y < height; y++) {
                        const uint8_t *map = Bayer4[(y + part.y) & 3];
                        if (ownSize) {
                                // Its own size (a picture made for the slide): nothing to scale.
                                const uint8_t *source = reinterpret_cast<const uint8_t *>(picture.pixels) +
                                                        size_t(y + part.y) * picture.stride;
                                auto *to = reinterpret_cast<uint16_t *>(out->pixels + size_t(y) * out->stride);
                                if (plain && picture.layout == Picture::Layout::Rgb565) {
                                        // Already RGB565 and as dithered as it will be: the row as it is.
                                        memcpy(to, source + size_t(part.x) * 2, size_t(width) * 2);
                                        continue;
                                }
                                if (plain && picture.layout == Picture::Layout::Bgr24) {
                                        const uint8_t *q = source + size_t(part.x) * 3;
                                        for (int32_t x = 0; x < width; x++, q += 3) {
                                                to[x] = dithered565(q[2], q[1], q[0], map[(x + part.x) & 3]);
                                        }
                                        continue;
                                }
                                for (int32_t x = 0; x < width; x++) {
                                        uint32_t j = uint32_t(x + part.x);
                                        uint32_t p = picture.layout == Picture::Layout::Bgr24
                                                             ? pixelAt<Picture::Layout::Bgr24>(source, j)
                                                     : picture.layout == Picture::Layout::Rgb565
                                                             ? pixelAt<Picture::Layout::Rgb565>(source, j)
                                                             : pixelAt<Picture::Layout::Bgra32>(source, j);
                                        int32_t  A = alpha ? int32_t(p >> 24) : 255;
                                        int32_t  R = int32_t((p >> 16) & 0xFF), G = int32_t((p >> 8) & 0xFF),
                                                B = int32_t(p & 0xFF);
                                        if (A != 255) {
                                                R = R * A / 255;
                                                G = G * A / 255;
                                                B = B * A / 255;
                                        }
                                        put(x, y, R, G, B, A);
                                }
                                continue;
                        }
                        const uint32_t *w = down.weights + size_t(y) * down.taps;
                        uint32_t        first = down.first[y];
                        // The source rows this output row reads, scaled across (made once each).
                        uint32_t n = down.taps;
                        for (uint32_t t = 0; t < n; t++) {
                                rows[t] = nullptr;
                                if (w[t] == 0) continue;
                                int32_t j = int32_t(first + t);
                                if (j >= picture.height) break;
                                uint32_t  slot = uint32_t(j) % ringRows;
                                uint16_t *line = ring + size_t(slot) * size_t(width) * 4;
                                if (ringRow[slot] != j) {
                                        const uint8_t *source = reinterpret_cast<const uint8_t *>(picture.pixels) +
                                                                size_t(j) * picture.stride;
                                        scaleRow(source, picture.layout, picture.width, across, width, alpha, line);
                                        ringRow[slot] = j;
                                }
                                rows[t] = line;
                        }
                        auto *to = reinterpret_cast<uint16_t *>(out->pixels + size_t(y) * out->stride);
                        for (int32_t x = 0; x < width; x++) {
                                uint32_t r = 0, g = 0, b = 0, a = 0;
                                for (uint32_t t = 0; t < n; t++) {
                                        if (rows[t] == nullptr) continue;
                                        const uint16_t *p = rows[t] + size_t(x) * 4;
                                        r += w[t] * p[0];
                                        g += w[t] * p[1];
                                        b += w[t] * p[2];
                                        a += w[t] * p[3];
                                }
                                int32_t R = int32_t((r + (1u << 23)) >> 24), G = int32_t((g + (1u << 23)) >> 24),
                                        B = int32_t((b + (1u << 23)) >> 24), A = int32_t((a + (1u << 23)) >> 24);
                                if (plain) {
                                        to[x] = dithered565(R, G, B, map[(x + part.x) & 3]);
                                        continue;
                                }
                                if (A > 255) A = 255;
                                if (R > A) R = A;
                                if (G > A) G = A;
                                if (B > A) B = A;
                                put(x, y, R, G, B, A);
                        }
                }
                free(ring);
                free(ringRow);
                free(rows);
                if (opaque && out->alpha != nullptr) {
                        // Said to have alpha, but every pixel is opaque: drawn as an opaque one.
                        free(out->alpha);
                        out->alpha = nullptr;
                }
                return Error::Ok;
        }

        void freeScaled(ScaledImage &image) {
                free(image.pixels);
                free(image.alpha);
                image = ScaledImage();
                return;
        }

        void blitImage(Canvas &canvas, const ScaledImage &image, int32_t x, int32_t y) {
                if (image.format != canvas.format() || image.pixels == nullptr) return;
                Rect r = Rect{x, y, image.width, image.height}.intersected(canvas.clip());
                if (r.isEmpty()) return;
                uint32_t bpp = bytesPerPixel(image.format);
                for (int32_t row = r.y; row < r.y + r.height; row++) {
                        int32_t        sy = row - y;
                        int32_t        sx = r.x - x;
                        const uint8_t *from = image.pixels + size_t(sy) * image.stride + size_t(sx) * bpp;
                        uint8_t       *to = canvas.pixels() + size_t(row) * canvas.stride() + size_t(r.x) * bpp;
                        if (image.alpha == nullptr) {
                                memcpy(to, from, size_t(r.width) * bpp);
                                continue;
                        }
                        const uint8_t *alpha = image.alpha + size_t(sy) * size_t(image.width) + size_t(sx);
                        for (int32_t i = 0; i < r.width; i++) {
                                uint32_t a = alpha[i];
                                if (a == 0) continue;
                                if (image.format == PixelFormat::Rgb565) {
                                        uint16_t s = reinterpret_cast<const uint16_t *>(from)[i];
                                        if (a == 255) {
                                                reinterpret_cast<uint16_t *>(to)[i] = s;
                                                continue;
                                        }
                                        uint16_t d = reinterpret_cast<uint16_t *>(to)[i];
                                        uint32_t keep = 255 - a;
                                        // Premultiplied: the source plus what shows through of what is there.
                                        uint32_t rr = ((s >> 11) & 0x1F) + ((d >> 11) & 0x1F) * keep / 255;
                                        uint32_t gg = ((s >> 5) & 0x3F) + ((d >> 5) & 0x3F) * keep / 255;
                                        uint32_t bb = (s & 0x1F) + (d & 0x1F) * keep / 255;
                                        if (rr > 31) rr = 31;
                                        if (gg > 63) gg = 63;
                                        if (bb > 31) bb = 31;
                                        reinterpret_cast<uint16_t *>(to)[i] = uint16_t((rr << 11) | (gg << 5) | bb);
                                } else {
                                        uint32_t s = reinterpret_cast<const uint32_t *>(from)[i];
                                        if (a == 255) {
                                                reinterpret_cast<uint32_t *>(to)[i] = s;
                                                continue;
                                        }
                                        uint32_t d = reinterpret_cast<uint32_t *>(to)[i];
                                        uint32_t keep = 255 - a;
                                        uint32_t rr = ((s >> 16) & 0xFF) + ((d >> 16) & 0xFF) * keep / 255;
                                        uint32_t gg = ((s >> 8) & 0xFF) + ((d >> 8) & 0xFF) * keep / 255;
                                        uint32_t bb = (s & 0xFF) + (d & 0xFF) * keep / 255;
                                        reinterpret_cast<uint32_t *>(to)[i] = ((rr > 255 ? 255 : rr) << 16) |
                                                                              ((gg > 255 ? 255 : gg) << 8) |
                                                                              (bb > 255 ? 255 : bb);
                                }
                        }
                }
                return;
        }

        // ---- The cache

        struct ImageCache::Info {
                        char   *path = nullptr; // as written
                        Error   error = Error::Ok;
                        int32_t width = 0;
                        int32_t height = 0;
                        bool    failed = false; // decoding or scaling it failed
                        Info   *next = nullptr;
        };

        struct ImageCache::Entry {
                        char        *path = nullptr;
                        int32_t      width = 0;
                        int32_t      height = 0;
                        Rect         window;
                        uint8_t      filters = 0;
                        const Color *palette = nullptr;
                        PixelFormat  format = PixelFormat::Rgb565;
                        ScaledImage  image;
                        size_t       bytes = 0;
                        uint32_t     used = 0;
                        Entry       *next = nullptr;
        };

        void folderOf(const char *path, char *out, size_t size) {
                const char *slash = strrchr(path, '/');
                if (slash == nullptr) {
                        snprintf(out, size, ".");
                } else {
                        snprintf(out, size, "%.*s", int(slash == path ? 1 : slash - path), path);
                }
                return;
        }

        ImageCache::ImageCache(Platform &platform, const char *deckPath) : _platform(platform) {
                folderOf(deckPath, _folder, sizeof _folder);
        }

        ImageCache::~ImageCache() {
                clear();
        }

        void ImageCache::clear() {
                while (_infos != nullptr) {
                        Info *next = _infos->next;
                        free(_infos->path);
                        free(_infos);
                        _infos = next;
                }
                while (_entries != nullptr) {
                        Entry *next = _entries->next;
                        freeScaled(_entries->image);
                        free(_entries->path);
                        free(_entries);
                        _entries = next;
                }
                _bytes = 0;
                return;
        }

        bool ImageCache::resolve(StringView path, char *out, size_t size) const {
                // Markdown's paths are URLs: %20 and the like decoded.
                char     decoded[512];
                uint32_t n = 0;
                for (uint32_t i = 0; i < path.length() && n + 1 < sizeof decoded; i++) {
                        char c = path[i];
                        if (c == '%' && i + 2 < path.length()) {
                                auto hex = [](char h) {
                                        return h >= '0' && h <= '9'   ? h - '0'
                                               : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                               : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                                      : -1;
                                };
                                int hi = hex(path[i + 1]), lo = hex(path[i + 2]);
                                if (hi >= 0 && lo >= 0) {
                                        decoded[n++] = char(hi * 16 + lo);
                                        i += 2;
                                        continue;
                                }
                        }
                        decoded[n++] = c;
                }
                decoded[n] = '\0';
                int w = decoded[0] == '/' ? snprintf(out, size, "%s", decoded)
                                          : snprintf(out, size, "%s/%s", _folder, decoded);
                return w > 0 && size_t(w) < size;
        }

        ImageCache::Info *ImageCache::info(StringView path) {
                for (Info *i = _infos; i != nullptr; i = i->next) {
                        if (StringView(i->path).equals(path)) return i;
                }
                Info *i = static_cast<Info *>(calloc(1, sizeof(Info)));
                if (i == nullptr) return nullptr;
                i->path = static_cast<char *>(malloc(path.length() + 1));
                if (i->path == nullptr) {
                        free(i);
                        return nullptr;
                }
                memcpy(i->path, path.data(), path.length());
                i->path[path.length()] = '\0';
                char full[1024];
                if (!resolve(path, full, sizeof full)) {
                        i->error = Error::TooBig;
                } else {
                        i->error = _platform.imageSize(full, &i->width, &i->height);
                }
                i->next = _infos;
                _infos = i;
                return i;
        }

        Error ImageCache::takeFailure(char *path, size_t size) {
                Error e = _failed;
                if (e != Error::Ok) snprintf(path, size, "%s", _failedPath);
                _failed = Error::Ok;
                return e;
        }

        Error ImageCache::size(StringView path, int32_t *width, int32_t *height) {
                Info *i = info(path);
                if (i == nullptr) return Error::NoMemory;
                *width = i->width;
                *height = i->height;
                return i->error;
        }

        void ImageCache::dropOldest() {
                Entry **oldest = nullptr;
                for (Entry **e = &_entries; *e != nullptr; e = &(*e)->next) {
                        if (oldest == nullptr || (*e)->used < (*oldest)->used) oldest = e;
                }
                if (oldest == nullptr) return;
                Entry *gone = *oldest;
                *oldest = gone->next;
                _bytes -= gone->bytes;
                freeScaled(gone->image);
                free(gone->path);
                free(gone);
                return;
        }

        const ScaledImage *ImageCache::get(StringView path, int32_t width, int32_t height, const Rect &window,
                                           uint8_t filters, const Palette *palette, PixelFormat format) {
                _clock++;
                const Color *colors = palette != nullptr && palette->count > 0 ? palette->colors : nullptr;
                for (Entry *e = _entries; e != nullptr; e = e->next) {
                        if (e->width == width && e->height == height && e->window.x == window.x &&
                            e->window.y == window.y && e->window.width == window.width &&
                            e->window.height == window.height && e->filters == filters && e->palette == colors &&
                            e->format == format && StringView(e->path).equals(path)) {
                                e->used = _clock;
                                return &e->image;
                        }
                }
                Info *i = info(path);
                if (i == nullptr || i->error != Error::Ok) return nullptr;
                if (i->failed) return nullptr; // once failed, not tried again until the deck reloads
                char full[1024];
                if (!resolve(path, full, sizeof full)) return nullptr;
                auto fail = [&](Error why) -> const ScaledImage * {
                        i->failed = true;
                        if (_failed == Error::Ok) {
                                _failed = why;
                                snprintf(_failedPath, sizeof _failedPath, "%.*s", int(path.length()), path.data());
                        }
                        return nullptr;
                };
                // Decoded at least as big as drawn (a JPEG comes reduced toward it), then scaled. Out
                // of memory, the pictures kept are let go and it is tried once more: a decoded
                // picture is big (a 1920×1080 JPEG is 4 MB), and memory is what the board has least.
                Picture picture;
                Error   decoded = _platform.decodeImage(full, width, height, &picture);
                if (decoded == Error::NoMemory && _entries != nullptr) {
                        while (_entries != nullptr) dropOldest();
                        decoded = _platform.decodeImage(full, width, height, &picture);
                }
                if (decoded != Error::Ok) return fail(decoded);
                Entry *e = static_cast<Entry *>(calloc(1, sizeof(Entry)));
                Error  error = e != nullptr ? scalePicture(picture, width, height, &window, filters, palette, format,
                                                           &e->image)
                                            : Error::NoMemory;
                if (error == Error::NoMemory && _entries != nullptr) {
                        while (_entries != nullptr) dropOldest();
                        if (e == nullptr) e = static_cast<Entry *>(calloc(1, sizeof(Entry)));
                        error = e != nullptr ? scalePicture(picture, width, height, &window, filters, palette, format,
                                                            &e->image)
                                             : Error::NoMemory;
                }
                _platform.freePixels(picture.pixels);
                if (e != nullptr) e->path = static_cast<char *>(malloc(path.length() + 1));
                if (error != Error::Ok || e == nullptr || e->path == nullptr) {
                        if (e != nullptr) {
                                freeScaled(e->image);
                                free(e->path);
                                free(e);
                        }
                        return fail(error != Error::Ok ? error : Error::NoMemory);
                }
                memcpy(e->path, path.data(), path.length());
                e->path[path.length()] = '\0';
                e->width = width;
                e->height = height;
                e->window = window;
                e->filters = filters;
                e->palette = colors;
                e->format = format;
                e->bytes = size_t(e->image.stride) * size_t(e->image.height) +
                           (e->image.alpha != nullptr ? size_t(e->image.width) * size_t(e->image.height) : 0);
                e->used = _clock;
                // Room for it: the least recently drawn go (never this one).
                while (_bytes + e->bytes > MaxBytes && _entries != nullptr) dropOldest();
                e->next = _entries;
                _entries = e;
                _bytes += e->bytes;
                return &e->image;
        }

} // namespace preso
