/**
 * @file      tiledpattern.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/backdrop.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The desktop's pattern (`devplan/wallpaper.md` §4b, §4d): a PNG of at most
 *        `MaxSide` × `MaxSide` tiled from the window's top-left corner, a palette image's
 *        colors replaced as asked, each pixel drawn `scale` × `scale`. Pure: its owner (the
 *        panel's desktop, Settings' previews) finds the file (`PatternFiles`) and gives its bytes.
 *
 * The tile is kept in B, G, R, its rows widened to whole repeats of at least `MinRow`
 * pixels, so a row is made in a few copies; scaled across when decoded, down when made.
 * A pattern with alpha keeps its 0xAARRGGBB too, to be blended again over a new
 * background color.
 *
 * @par Thread Safety
 * Not thread-safe (its owner's lock).
 */
class TiledPattern final : public Backdrop {
        public:
                static constexpr uint16_t MaxSide = 256;   ///< The largest pattern, in its pixels.
                static constexpr unsigned MaxScale = 4;    ///< `scale` at most.
                static constexpr size_t   MaxColors = 256; ///< A palette's entries.
                static constexpr uint16_t MinRow = 128;    ///< A kept row's pixels at least.

                TiledPattern() = default;
                ~TiledPattern() { reset(); }
                TiledPattern(const TiledPattern &) = delete;
                TiledPattern &operator=(const TiledPattern &) = delete;

                /**
                 * @brief Decodes @p file (a PNG) as the pattern: a palette image's first entries
                 *        replaced by @p colors (as `parseColors()` reads them), each pixel
                 *        @p scale × @p scale (1 to `MaxScale`), blended over @p background
                 *        (0xRRGGBB) where transparent. The earlier pattern goes, whatever happens.
                 * @return `Ok`; `OutOfRange` (larger than `MaxSide`, or @p scale); `NoMem`; the
                 *         decoder's error.
                 */
                Error load(ByteSpan file, StringView colors, unsigned scale, uint32_t background);

                /** @brief Blends a pattern with alpha over @p rgb (0xRRGGBB) instead (none: nothing done). */
                void setBackground(uint32_t rgb);

                /** @brief Drops the pattern. */
                void reset();

                /** @brief Returns true if there is a pattern. */
                bool isValid() const { return _tile != nullptr; }

                /** @brief Returns the pattern's size, in its own pixels. */
                uint16_t width() const { return _width; }
                uint16_t height() const { return _height; }

                /** @brief Returns the bytes kept. */
                size_t bytes() const;

                void make(int x, int y, uint16_t width, uint16_t height, uint8_t *out, size_t stride) const override;

                /**
                 * @brief Reads @p text (`#rrggbb` an entry, `;` between them; an empty entry
                 *        keeps the file's own) over @p palette's first entries (0xRRGGBB).
                 * @return The entries read (kept ones counted); `Invalid` (not a color).
                 */
                static Result<size_t> parseColors(StringView text, Span<uint32_t> palette);

        private:
                Error build();

                uint32_t *_argb = nullptr; // the pattern's own pixels, kept only if it has alpha
                uint8_t  *_tile = nullptr; // B, G, R: _height rows of _rowWidth
                uint16_t  _width = 0;
                uint16_t  _height = 0;
                uint16_t  _period = 0;   // _width × _scale: a row's repeat
                uint16_t  _rowWidth = 0; // whole repeats, at least MinRow
                uint8_t   _scale = 1;
                uint32_t  _background = 0;
};

BMDS_NAMESPACE_END
