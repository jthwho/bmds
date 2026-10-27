/**
 * @file      textdraw.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/font.h>
#include <bmds/result.h>
#include <bmds/simd.h>
#include <bmds/stringview.h>
#include <bmds/textblend.h>

BMDS_NAMESPACE_BEGIN

/** @brief Pixels text is drawn into: a picture and the part of it that may change. */
struct TextTarget {
                uint8_t          *pixels = nullptr;
                TextBlend::Format format = TextBlend::Format::Bgr24;
                int               width = 0;
                int               height = 0;
                size_t            stride = 0; ///< Bytes from a row to the next.
                int               clipLeft = 0;
                int               clipTop = 0;
                int               clipRight = INT_MAX;  ///< One past the last column (clamped to `width`).
                int               clipBottom = INT_MAX; ///< One past the last row (clamped to `height`).
};

/** @brief How text is drawn. */
struct TextPaint {
                uint32_t color = 0;      ///< 0xRRGGBB (`Index8`: the index).
                uint32_t background = 0; ///< With `opaque`: the box under the text, filled first.
                bool     opaque = false; ///< Fill the line's box with `background` (nothing read back).
                bool     smooth = true;  ///< TrueType smoothed (4 bits), else sharp (1); never for `Index8`.
};

/**
 * @brief Draws lines of text (`devplan/fonts.md` §4d): laid out (`Font::layout()`),
 *        their glyphs from the glyph cache a run at a time, blended by
 *        `TextBlend` into a `TextTarget`, clipped.
 * @par Thread Safety
 * Reentrant (the cache is the system's).
 */
namespace TextDraw {

        /**
         * @brief Draws @p utf8 on one line in @p font, the line's top at (@p x, @p top)
         *        (its baseline the font's ascent below), as far as @p maxWidth
         *        pixels allow (whole characters; 0: no limit).
         * @return The width drawn in pixels (the advance, rounded up), or the
         *         cache's error for a glyph (damaged, `NoMem`): what came before
         *         it is drawn.
         * @hotpath
         */
        Result<uint32_t> line(const TextTarget &target, const Font &font, StringView utf8, int x, int top,
                              const TextPaint &paint, uint32_t maxWidth, Simd::Kernel kernel);

} // namespace TextDraw

BMDS_NAMESPACE_END
