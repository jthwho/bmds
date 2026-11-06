/**
 * @file      effects.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A theme's scanlines (devplan/preso.md §5).
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

        } // namespace

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
                if (theme.scanlines != 0) applyScanlines(canvas, theme.scanlines);
                return;
        }

} // namespace preso
