/**
 * @file      font.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A glyph's bitmap: `Font::height()` rows, the top one first, each
 *        `width / 8` bytes with the most significant bit leftmost.
 */
struct Glyph {
                const uint8_t *rows = nullptr;
                uint8_t        width = 0; ///< In pixels: the font's cell width, or twice it.
};

/**
 * @brief A bitmap font in rodata: glyphs one or two cells wide, found by
 *        codepoint (`devplan/windows.md` W7).
 *
 * Generated from a `.hex` file by `tools/fontconv.py`. Lookups binary-search
 * runs of consecutive codepoints; a missing character gets the replacement
 * glyph.
 *
 * @par Thread Safety
 * Immutable: safe from any task.
 */
class Font {
        public:
                /** @brief A run of consecutive codepoints with consecutive glyphs. */
                struct Range {
                                uint32_t first;
                                uint16_t count;
                                uint16_t index; ///< The first glyph's index in the bitmaps.
                };

                template <size_t NarrowRuns, size_t NarrowBytes, size_t WideRuns, size_t WideBytes>
                constexpr Font(uint8_t width, uint8_t height, const Range (&narrow)[NarrowRuns],
                               const uint8_t (&narrowBits)[NarrowBytes], const Range (&wide)[WideRuns],
                               const uint8_t (&wideBits)[WideBytes], uint32_t replacement)
                    : _narrow(narrow, NarrowRuns), _wide(wide, WideRuns), _narrowBits(narrowBits), _wideBits(wideBits),
                      _replacement(replacement), _glyphBytes(static_cast<uint16_t>(height * (width / 8))),
                      _width(width), _height(height) {
                        static_assert(NarrowRuns > 0, "a font has narrow glyphs");
                }

                Font(const Font &) = delete;
                Font &operator=(const Font &) = delete;

                /** @brief Returns the cell width in pixels. */
                uint8_t width() const { return _width; }

                /** @brief Returns the height in pixels. */
                uint8_t height() const { return _height; }

                /** @brief Returns true if it has a glyph for @p codepoint. */
                bool hasGlyph(uint32_t codepoint) const;

                /** @brief Returns @p codepoint's glyph, or the replacement glyph. @hotpath */
                Glyph glyph(uint32_t codepoint) const {
                        // The first narrow run (ASCII, in unscii) inline, without a search.
                        const Range &first = _narrow[0];
                        if (codepoint - first.first < first.count)
                                return Glyph{_narrowBits + (first.index + (codepoint - first.first)) * _glyphBytes,
                                             _width};
                        return lookup(codepoint);
                }

                /** @brief Returns @p utf8's width in pixels as drawn (wide glyphs twice a cell). */
                uint32_t textWidth(StringView utf8) const;

                /**
                 * @brief Returns how many bytes of @p utf8 fit in @p maxWidth pixels,
                 *        whole characters only.
                 */
                size_t fitting(StringView utf8, uint32_t maxWidth) const;

                /** @brief Returns the glyph count. */
                size_t glyphCount() const;

        private:
                bool  find(uint32_t codepoint, Glyph &out) const;
                Glyph lookup(uint32_t codepoint) const;

                Span<const Range> _narrow;
                Span<const Range> _wide;
                const uint8_t    *_narrowBits;
                const uint8_t    *_wideBits;
                uint32_t          _replacement;
                uint16_t          _glyphBytes; // a narrow glyph's
                uint8_t           _width;
                uint8_t           _height;
};

/** @brief The fonts built in. */
namespace Fonts {
        /** @brief unscii-16 by Viznut (public domain): 8×16, the system font. */
        extern const Font unscii16;

        /** @brief The window manager's icons (`Icon`), 16×16: BMDS's own. */
        extern const Font icons16;
} // namespace Fonts

/** @brief `Fonts::icons16`'s glyphs, in the private use area. Append only. */
namespace IconGlyph {
        constexpr uint32_t Menu = 0xe000;     ///< ≡
        constexpr uint32_t Minimize = 0xe001; ///< A bar at the bottom.
        constexpr uint32_t Maximize = 0xe002; ///< A window.
        constexpr uint32_t Restore = 0xe003;  ///< A window in front of another.
        constexpr uint32_t Close = 0xe004;    ///< ×
        constexpr uint32_t Check = 0xe005;    ///< ✓: a checked menu item.
        constexpr uint32_t Submenu = 0xe006;  ///< ▸: a menu item opening a submenu.
} // namespace IconGlyph

BMDS_NAMESPACE_END
