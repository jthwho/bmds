/**
 * @file      textblend.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/fontface.h>
#include <bmds/simd.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Text's inner loops (`devplan/fonts.md` §4d): a glyph's coverage turned
 *        into alpha, and one color blended by it over a row of pixels in any
 *        format.
 *
 * Every blend is `(color × a + pixel × (255 − a) + 127) / 255` per channel,
 * as the icons' alpha blend is (`Pixels::fromArgb()`), so a fully covered
 * pixel is the color and an uncovered one is left as it was. Text on a known
 * background is a fill and then the same blend: nothing on the screen is read.
 * Coverage is linear here (alpha = level × 17); the contrast curve is applied
 * when a glyph is rendered (`GlyphCache::setCurve()`).
 *
 * Two kernels, as everywhere (`devplan/simd.md`): `Scalar`, plain C++, and
 * `Pie` for B, G, R and R, G, B (16 pixels a step: three planes, two QACC
 * passes a channel) and for grey, writing the same bytes. RGB565, B, G, R, A
 * and palette indexes are scalar.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace TextBlend {

        /** @brief The pixel formats text is drawn into: the SDK's `BMDS_PIXEL_*` values. */
        enum class Format : uint8_t {
                Bgr24 = 0,  ///< Bytes B, G, R (the screen's).
                Rgb24 = 1,  ///< Bytes R, G, B.
                Bgra32 = 2, ///< Bytes B, G, R, A: alpha becomes a + A × (255 − a) / 255.
                Rgb565 = 3, ///< A little-endian 16-bit word, red in the top 5 bits.
                Gray8 = 4,  ///< One byte of luma: the color's (BT.601).
                Index8 = 5, ///< A palette index (`L8`): the color's low byte, written where alpha ≥ 128.
        };

        /** @brief Returns @p format's bytes a pixel. */
        constexpr unsigned bytesPerPixel(Format format) {
                switch (format) {
                        case Format::Bgr24:
                        case Format::Rgb24: return 3;
                        case Format::Bgra32: return 4;
                        case Format::Rgb565: return 2;
                        case Format::Gray8:
                        case Format::Index8: return 1;
                }
                return 1;
        }

        /** @brief Each 4-bit level's alpha (level × 17). */
        inline constexpr uint8_t Levels[16] = {0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255};

        /**
         * @brief Combines @p count pixels of a coverage row (`FontFace::Depth`), from
         *        its pixel @p first, into @p alpha (level × 17: a sharp pixel 255), each
         *        the larger of the two: glyphs that overlap (kerned) cover both.
         * @hotpath Inline: called for each glyph's row.
         */
        inline void compose(const uint8_t *row, FontFace::Depth depth, unsigned first, unsigned count, uint8_t *alpha) {
                const unsigned end = first + count;
                if (depth == FontFace::Depth::Mono) {
                        for (unsigned i = first; i < end; ++i, ++alpha)
                                if ((row[i >> 3] & (0x80 >> (i & 7))) != 0) *alpha = 255;
                        return;
                }
                unsigned i = first;
                if ((i & 1) != 0 && i < end) {
                        const uint8_t v = uint8_t((row[i >> 1] & 15u) * 17u);
                        if (v > *alpha) *alpha = v;
                        ++alpha;
                        ++i;
                }
                for (; i + 2 <= end; i += 2, alpha += 2) {
                        const uint8_t b = row[i >> 1];
                        if (b == 0) continue;
                        const uint8_t hi = Levels[b >> 4], lo = Levels[b & 15u];
                        if (hi > alpha[0]) alpha[0] = hi;
                        if (lo > alpha[1]) alpha[1] = lo;
                }
                if (i < end) {
                        const uint8_t v = uint8_t((row[i >> 1] >> 4) * 17u);
                        if (v > *alpha) *alpha = v;
                }
                return;
        }

        /** @brief The 16 pixels from a background to a color, a level each (`paint()`): alpha >> 4. */
        struct Ramp {
                        uint8_t bytes[16][4] = {}; ///< Each level's pixel in the format.
                        Format  format = Format::Bgr24;
        };

        /**
         * @brief Makes the ramp from @p background to @p color in @p format: level l
         *        is the blend of the two by alpha l × 17 (`Index8`: the color's index
         *        from level 8 up, the background's below).
         */
        void makeRamp(Format format, uint32_t color, uint32_t background, Ramp &out);

        /**
         * @brief Writes each of @p count pixels whose alpha (as `compose()` makes it:
         *        level × 17) isn't 0 from @p ramp, by its level (alpha >> 4): text on a
         *        known background, nothing read.
         * @hotpath
         */
        void paint(uint8_t *pixels, const uint8_t *alpha, unsigned count, const Ramp &ramp);

        /**
         * @brief Blends @p color (0xRRGGBB; `Index8`: the index) by @p alpha over
         *        @p count pixels of @p format at @p pixels.
         * @hotpath
         */
        void blend(uint8_t *pixels, Format format, const uint8_t *alpha, unsigned count, uint32_t color,
                   Simd::Kernel kernel);

        /** @brief Sets @p count pixels of @p format at @p pixels to @p color (`Bgra32`: opaque). @hotpath */
        void fill(uint8_t *pixels, Format format, unsigned count, uint32_t color);

        /** @brief Returns @p rgb's luma (BT.601), as `Gray8` blends it. */
        constexpr uint8_t luma(uint32_t rgb) {
                return uint8_t((77u * (rgb >> 16 & 0xff) + 150u * (rgb >> 8 & 0xff) + 29u * (rgb & 0xff) + 128u) >> 8);
        }

} // namespace TextBlend

BMDS_NAMESPACE_END
