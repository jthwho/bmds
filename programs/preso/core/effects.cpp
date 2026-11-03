/**
 * @file      effects.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The glow is a bloom: each pixel's excess over the background (per channel) at a quarter size,
 * box blurred three times each way (close to a Gaussian, each pass a running sum: its cost doesn't
 * grow with the radius), added back at the full size, bilinear between the low samples. Through a
 * half size and copied columns it took a second a slide on the board.
 */

#include "effects.h"
#include <stdlib.h>

namespace preso {

        namespace {

                inline void unpack(const Canvas &c, const uint8_t *row, int32_t x, int32_t &r, int32_t &g, int32_t &b) {
                        if (c.format() == PixelFormat::Rgb565) {
                                uint32_t v = reinterpret_cast<const uint16_t *>(row)[x];
                                uint32_t r5 = v >> 11, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
                                r = int32_t((r5 << 3) | (r5 >> 2));
                                g = int32_t((g6 << 2) | (g6 >> 4));
                                b = int32_t((b5 << 3) | (b5 >> 2));
                        } else {
                                uint32_t v = reinterpret_cast<const uint32_t *>(row)[x];
                                r = int32_t((v >> 16) & 0xFF);
                                g = int32_t((v >> 8) & 0xFF);
                                b = int32_t(v & 0xFF);
                        }
                        return;
                }

                inline void pack(const Canvas &c, uint8_t *row, int32_t x, int32_t r, int32_t g, int32_t b) {
                        if (r > 255) r = 255;
                        if (g > 255) g = 255;
                        if (b > 255) b = 255;
                        if (c.format() == PixelFormat::Rgb565) {
                                reinterpret_cast<uint16_t *>(row)[x] =
                                        uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                        } else {
                                reinterpret_cast<uint32_t *>(row)[x] =
                                        (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
                        }
                        return;
                }

                // One box blur of radius @p r along a line of @p n samples, @p step apart (3 channels
                // interleaved), from @p in into @p out: a running sum, the edges extended.
                void boxLine(const uint8_t *in, uint8_t *out, int32_t n, int32_t step, int32_t r) {
                        int32_t  window = 2 * r + 1;
                        uint32_t inverse =
                                (65536u + uint32_t(window) / 2) / uint32_t(window); // a multiply, not a divide
                        uint32_t sum[3] = {0, 0, 0};
                        for (int32_t k = -r; k <= r; k++) {
                                int32_t i = k < 0 ? 0 : k >= n ? n - 1 : k;
                                for (int c = 0; c < 3; c++) sum[c] += in[i * step + c];
                        }
                        for (int32_t i = 0; i < n; i++) {
                                for (int c = 0; c < 3; c++) out[i * step + c] = uint8_t((sum[c] * inverse) >> 16);
                                int32_t add = i + r + 1 >= n ? n - 1 : i + r + 1;
                                int32_t drop = i - r < 0 ? 0 : i - r;
                                for (int c = 0; c < 3; c++)
                                        sum[c] += uint32_t(in[add * step + c]) - in[drop * step + c];
                        }
                        return;
                }

        } // namespace

        bool applyGlow(Canvas &canvas, Color background, uint8_t strength, uint8_t radius) {
                if (strength == 0) return true;
                // At a quarter of the size each way: a sixteenth of the pixels to blur. Every pass reads
                // memory in order (a column's blur keeps a running sum per column, not a copy of it).
                constexpr int32_t Q = 4;
                int32_t           w = canvas.width() / Q, h = canvas.height() / Q;
                if (w < 2 || h < 2) return true;
                size_t    bytes = size_t(w) * size_t(h) * 3;
                uint8_t  *a = static_cast<uint8_t *>(malloc(bytes));
                uint8_t  *b = static_cast<uint8_t *>(malloc(bytes));
                uint32_t *sums = static_cast<uint32_t *>(malloc(size_t(w) * 3 * sizeof(uint32_t)));
                int32_t  *line = static_cast<int32_t *>(malloc(size_t(w) * 3 * sizeof(int32_t)));
                if (a == nullptr || b == nullptr || sums == nullptr || line == nullptr) {
                        free(a);
                        free(b);
                        free(sums);
                        free(line);
                        return false;
                }
                int32_t br = int32_t((background >> 16) & 0xFF), bg = int32_t((background >> 8) & 0xFF),
                        bb = int32_t(background & 0xFF);
                // The excess over the background, each Q×Q averaged, a row at a time. In RGB565 each
                // field's excess is a table's: a pixel is three lookups.
                uint8_t ex5r[32], ex6g[64], ex5b[32];
                for (int32_t v = 0; v < 64; v++) {
                        int32_t g8 = (v << 2) | (v >> 4);
                        ex6g[v] = uint8_t(g8 > bg ? g8 - bg : 0);
                        if (v < 32) {
                                int32_t c8 = (v << 3) | (v >> 2);
                                ex5r[v] = uint8_t(c8 > br ? c8 - br : 0);
                                ex5b[v] = uint8_t(c8 > bb ? c8 - bb : 0);
                        }
                }
                // The brightest excess, a channel at a time: the glow brightens nothing past it, so
                // its hue stays the text's (amber stays amber where red would saturate first).
                uint32_t most[3] = {0, 0, 0};
                for (int32_t y = 0; y < h; y++) {
                        memset(sums, 0, size_t(w) * 3 * sizeof(uint32_t));
                        for (int32_t k = 0; k < Q; k++) {
                                const uint8_t *row = canvas.pixels() + size_t(y * Q + k) * canvas.stride();
                                if (canvas.format() == PixelFormat::Rgb565) {
                                        const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
                                        for (int32_t x = 0; x < w; x++, p += Q) {
                                                uint32_t r = 0, g = 0, bl = 0;
                                                for (int32_t j = 0; j < Q; j++) {
                                                        uint32_t v = p[j];
                                                        uint32_t er = ex5r[v >> 11], eg = ex6g[(v >> 5) & 0x3F],
                                                                 eb = ex5b[v & 0x1F];
                                                        r += er;
                                                        g += eg;
                                                        bl += eb;
                                                        if (er > most[0]) most[0] = er;
                                                        if (eg > most[1]) most[1] = eg;
                                                        if (eb > most[2]) most[2] = eb;
                                                }
                                                sums[x * 3] += r;
                                                sums[x * 3 + 1] += g;
                                                sums[x * 3 + 2] += bl;
                                        }
                                        continue;
                                }
                                for (int32_t x = 0; x < w * Q; x++) {
                                        int32_t r, g, bl;
                                        unpack(canvas, row, x, r, g, bl);
                                        uint32_t *s3 = sums + (x / Q) * 3;
                                        uint32_t er = uint32_t(r > br ? r - br : 0), eg = uint32_t(g > bg ? g - bg : 0),
                                                 eb = uint32_t(bl > bb ? bl - bb : 0);
                                        s3[0] += er;
                                        s3[1] += eg;
                                        s3[2] += eb;
                                        if (er > most[0]) most[0] = er;
                                        if (eg > most[1]) most[1] = eg;
                                        if (eb > most[2]) most[2] = eb;
                                }
                        }
                        uint8_t *o = a + size_t(y) * size_t(w) * 3;
                        for (int32_t i = 0; i < w * 3; i++) o[i] = uint8_t(sums[i] / (Q * Q));
                }
                // Three box blurs each way: near a Gaussian of about the radius.
                int32_t r = (radius + Q / 2) / Q;
                if (r < 1) r = 1;
                int32_t window = 2 * r + 1;
                for (int pass = 0; pass < 3; pass++) {
                        for (int32_t y = 0; y < h; y++)
                                boxLine(a + size_t(y) * size_t(w) * 3, b + size_t(y) * size_t(w) * 3, w, 3, r);
                        // Down the columns: a running sum for each, rows added and dropped in order.
                        for (int32_t i = 0; i < w * 3; i++) sums[i] = 0;
                        for (int32_t k = -r; k <= r; k++) {
                                const uint8_t *row = b + size_t(k < 0 ? 0 : k >= h ? h - 1 : k) * size_t(w) * 3;
                                for (int32_t i = 0; i < w * 3; i++) sums[i] += row[i];
                        }
                        uint32_t inverse = (65536u + uint32_t(window) / 2) / uint32_t(window);
                        for (int32_t y = 0; y < h; y++) {
                                uint8_t *o = a + size_t(y) * size_t(w) * 3;
                                for (int32_t i = 0; i < w * 3; i++) o[i] = uint8_t((sums[i] * inverse) >> 16);
                                const uint8_t *add = b + size_t(y + r + 1 >= h ? h - 1 : y + r + 1) * size_t(w) * 3;
                                const uint8_t *drop = b + size_t(y - r < 0 ? 0 : y - r) * size_t(w) * 3;
                                for (int32_t i = 0; i < w * 3; i++) sums[i] += uint32_t(add[i]) - drop[i];
                        }
                }
                // Added back: each row's low-size samples interpolated down once, then across; only
                // the blur's excess over the pixel's own light (light bleeding onto what is darker,
                // so a big bright area isn't washed out), as a gain of up to two (more filled the inside of a W).
                // At a quarter's size, a pixel j of each four lies between low samples k - 1 and k
                // (j 0, 1) or k and k + 1 (j 2, 3), sixteenths 10, 14, 2, 6 of the way: fixed, so
                // nothing is worked out a pixel.
                const int32_t            cap[3] = {br + int32_t(most[0]), bg + int32_t(most[1]), bb + int32_t(most[2])};
                static constexpr int32_t Weight[Q] = {10, 14, 2, 6};
                static constexpr int32_t Offset[Q] = {-1, -1, 0, 0};
                for (int32_t y = 0; y < h * Q; y++) {
                        int32_t m = y / Q, j = y % Q;
                        int32_t y0 = m + Offset[j], y1 = y0 + 1;
                        int32_t fy = Weight[j];
                        if (y0 < 0) {
                                y0 = 0;
                                fy = 0;
                        }
                        if (y1 >= h) y1 = h - 1;
                        const uint8_t *p0 = a + size_t(y0) * size_t(w) * 3, *p1 = a + size_t(y1) * size_t(w) * 3;
                        bool           any = false;
                        for (int32_t i = 0; i < w * 3; i++) {
                                line[i] = p0[i] * (16 - fy) + p1[i] * fy; // sixteenths
                                any = any || line[i] != 0;
                        }
                        if (!any) continue;
                        uint8_t *row = canvas.pixels() + size_t(y) * canvas.stride();
                        for (int32_t k = 0; k < w; k++) {
                                for (int32_t jx = 0; jx < Q; jx++) {
                                        int32_t x0 = k + Offset[jx], x1 = x0 + 1, fx = Weight[jx];
                                        if (x0 < 0) {
                                                x0 = 0;
                                                fx = 0;
                                        }
                                        if (x1 >= w) x1 = w - 1;
                                        const int32_t *l0 = line + x0 * 3, *l1 = line + x1 * 3;
                                        int32_t        add0 = (l0[0] * (16 - fx) + l1[0] * fx) >> 8,
                                                add1 = (l0[1] * (16 - fx) + l1[1] * fx) >> 8,
                                                add2 = (l0[2] * (16 - fx) + l1[2] * fx) >> 8;
                                        if ((add0 | add1 | add2) == 0) continue;
                                        int32_t x = k * Q + jx;
                                        int32_t pr, pg, pb;
                                        unpack(canvas, row, x, pr, pg, pb);
                                        // Only the blur's excess over the pixel's own light: light bleeding
                                        // onto what is darker, a big bright area not washed out.
                                        int32_t o0 = pr - br, o1 = pg - bg, o2 = pb - bb;
                                        add0 = add0 > o0 ? ((add0 - (o0 > 0 ? o0 : 0)) * strength) >> 7 : 0;
                                        add1 = add1 > o1 ? ((add1 - (o1 > 0 ? o1 : 0)) * strength) >> 7 : 0;
                                        add2 = add2 > o2 ? ((add2 - (o2 > 0 ? o2 : 0)) * strength) >> 7 : 0;
                                        if ((add0 | add1 | add2) == 0) continue;
                                        int32_t nr = pr + add0, ng = pg + add1, nb = pb + add2;
                                        if (nr > cap[0]) nr = pr > cap[0] ? pr : cap[0];
                                        if (ng > cap[1]) ng = pg > cap[1] ? pg : cap[1];
                                        if (nb > cap[2]) nb = pb > cap[2] ? pb : cap[2];
                                        pack(canvas, row, x, nr, ng, nb);
                                }
                        }
                }
                free(a);
                free(b);
                free(sums);
                free(line);
                return true;
        }

        void applyScanlines(Canvas &canvas, uint8_t strength) {
                if (strength == 0) return;
                int32_t keep = 255 - strength;
                for (int32_t y = 1; y < canvas.height(); y += 2) {
                        uint8_t *row = canvas.pixels() + size_t(y) * canvas.stride();
                        if (canvas.format() == PixelFormat::Rgb565) {
                                // Each field scaled where it lies: no unpacking.
                                auto *p = reinterpret_cast<uint16_t *>(row);
                                for (int32_t x = 0; x < canvas.width(); x++) {
                                        uint32_t v = p[x];
                                        uint32_t r5 = ((v >> 11) * uint32_t(keep)) >> 8,
                                                 g6 = (((v >> 5) & 0x3F) * uint32_t(keep)) >> 8,
                                                 b5 = ((v & 0x1F) * uint32_t(keep)) >> 8;
                                        p[x] = uint16_t((r5 << 11) | (g6 << 5) | b5);
                                }
                                continue;
                        }
                        for (int32_t x = 0; x < canvas.width(); x++) {
                                int32_t r, g, b;
                                unpack(canvas, row, x, r, g, b);
                                pack(canvas, row, x, r * keep / 255, g * keep / 255, b * keep / 255);
                        }
                }
                return;
        }

        void applyEffects(Canvas &canvas, const Theme &theme) {
                canvas.clearClip();
                if (theme.glow != 0) (void)applyGlow(canvas, theme.background, theme.glow, theme.glowRadius);
                if (theme.scanlines != 0) applyScanlines(canvas, theme.scanlines);
                return;
        }

} // namespace preso
