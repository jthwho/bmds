/**
 * @file      glyphrenderer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/bitmapfont.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/** @brief A character cell to draw: a codepoint, its colors, its style. */
struct TextCell {
                static constexpr uint8_t Underline = 0x01; ///< The bottom row set.
                static constexpr uint8_t Bold = 0x02;      ///< The glyph smeared one pixel right.

                uint32_t codepoint = ' ';
                uint32_t fg = 0xffffff; ///< 0xRRGGBB.
                uint32_t bg = 0x000000;
                uint8_t  flags = 0;
};

/**
 * @brief Expands glyphs into RGB888 pixels (B, G, R bytes) in internal RAM
 *        (`devplan/windows.md` §4d): the inner loop of all text.
 *
 * Two kernels: `Scalar`, a table of 4 pixels per nibble of a glyph row
 * (rebuilt when the colors change), and `Pie`, the ESP32-P4's 128-bit SIMD
 * (each row's byte broadcast, compared into a mask of its pixels' bytes and
 * blended between the colors; nothing kept between glyphs). Both write the
 * same bytes.
 *
 * Output rows start 8-byte aligned and lie @p stride bytes apart (a
 * multiple of 8): a glyph at a multiple of 8 pixels in a strip whose width
 * is a multiple of 8.
 *
 * @par Thread Safety
 * One task per renderer.
 */
class GlyphRenderer {
        public:
                /** @brief How glyphs are expanded. */
                enum class Kernel : uint8_t {
                        Scalar, ///< Plain C++: two table lookups and six word stores a row.
                        Pie,    ///< The ESP32-P4's PIE vector unit.
                };

                /** @brief Returns true if this build has @p kernel. */
                static bool isSupported(Kernel kernel);

                /** @brief Returns the fastest kernel this build has. */
                static Kernel best();

                /** @brief A renderer with @p kernel (`Scalar` where it is missing). */
                explicit GlyphRenderer(Kernel kernel) : _kernel(isSupported(kernel) ? kernel : Kernel::Scalar) {}

                /** @brief Returns the kernel in use. */
                Kernel kernel() const { return _kernel; }

                /**
                 * @brief Writes @p glyph (@p height rows) in @p fg on @p bg, styled
                 *        by @p flags (`TextCell`), at @p out.
                 * @hotpath
                 */
                void draw(const Glyph &glyph, unsigned height, uint8_t flags, uint32_t fg, uint32_t bg, uint8_t *out,
                          size_t stride);

                /**
                 * @brief Writes @p cells in @p font side by side from @p out, as
                 *        many as fit in @p maxWidth pixels; @p used gets how many.
                 *        @p out null: measured only, nothing drawn.
                 * @return The pixels written (whole glyphs).
                 * @hotpath
                 */
                uint16_t drawCells(Span<const TextCell> cells, const BitmapFont &font, uint8_t *out, size_t stride,
                                   uint16_t maxWidth, size_t &used);

                /**
                 * @brief Writes @p utf8 in @p font, @p fg on @p bg, side by side
                 *        from @p out, as much as fits in @p maxWidth pixels; @p used
                 *        gets the bytes of @p utf8 written. @p out null: measured only
                 *        (what would be written, nothing drawn: a part off the clip).
                 * @return The pixels written.
                 * @hotpath
                 */
                uint16_t drawText(StringView utf8, const BitmapFont &font, uint32_t fg, uint32_t bg, uint8_t *out,
                                  size_t stride, uint16_t maxWidth, size_t &used);

        private:
                static constexpr unsigned MaxHeight = 32;

                void           setColors(uint32_t fg, uint32_t bg);
                void           begin();      // a run's constant state (PIE: registers)
                void           loadColors(); // PIE: the colors into their registers
                const uint8_t *style(const Glyph &glyph, unsigned height, uint8_t flags, uint8_t *styled) const;
                void           put(const uint8_t *rows, unsigned bytesPerRow, unsigned height, uint8_t *out,
                                   size_t stride) const; // with begin() and the colors loaded
                void           drawScalar(const uint8_t *rows, unsigned bytesPerRow, unsigned height, uint8_t *out,
                                          size_t stride) const;

                Kernel   _kernel;
                bool     _valid = false; // colors set
                uint32_t _fg = 0;
                uint32_t _bg = 0;
                uint32_t _table[16][3] = {}; // Scalar: 4 pixels for each nibble
                // Pie: the background's 24 bytes, then the colors' difference (fg ^ bg).
                alignas(16) uint8_t _pattern[64] = {};
};

BMDS_NAMESPACE_END
