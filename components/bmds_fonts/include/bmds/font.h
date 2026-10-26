/**
 * @file      font.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/fontname.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class BitmapFont;
class FontFace;
class FontLibrary;

/**
 * @brief A face at a size (`devplan/fonts.md` §4a): what text is measured and
 *        drawn with. Made by `FontLibrary::open()`; a copy is cheap, and while
 *        any copy lives its face stays loaded.
 *
 * TrueType faces are scaled to the pixel size; a bitmap face (unscii) is
 * drawn at a whole multiple of its own. A bold the family lacks is made up
 * (each glyph smeared a pixel right, one pixel wider).
 *
 * @par Thread Safety
 * A value: const calls from any task at once.
 */
class Font {
        public:
                /** @brief Vertical metrics in pixels, rounded out. */
                struct Metrics {
                                int16_t ascent = 0;     ///< Above the baseline.
                                int16_t descent = 0;    ///< Below it (positive).
                                int16_t lineHeight = 0; ///< From one baseline to the next.
                                int16_t maxAdvance = 0; ///< The widest advance.
                };

                /** @brief One glyph of a laid out line. */
                struct Placed {
                                const FontFace *face = nullptr; ///< The TrueType face drawing it (a fallback's, maybe).
                                const BitmapFont *bitmap = nullptr; ///< Or the bitmap font.
                                uint16_t          faceIndex = 0; ///< Its face's index in the library (the cache's id).
                                uint16_t          scale = 1;     ///< A bitmap font's multiple.
                                uint16_t          ascent = 0;    ///< A bitmap font's baseline below its top, scaled.
                                uint32_t          glyph = 0;     ///< A TrueType glyph, or a bitmap font's codepoint.
                                int32_t           x = 0;   ///< The pen in pixels from the line's start (`pen` snapped).
                                int64_t           pen = 0; ///< The pen, in 1/64 pixels.
                                int32_t           advance = 0; ///< In 1/64 pixels.
                                uint32_t          offset = 0;  ///< Its character's first byte in the text.
                };

                /** @brief Called for each glyph; false stops the layout. */
                using Visit = bool (*)(void *arg, const Placed &glyph);

                /** @brief No font (`isValid()` false). */
                Font() = default;
                Font(const Font &other);
                Font(Font &&other);
                Font &operator=(const Font &other);
                Font &operator=(Font &&other);
                ~Font();

                /** @brief Returns true for a font from `FontLibrary::open()`. */
                bool isValid() const { return _library != nullptr; }

                /** @brief Returns its library. */
                FontLibrary *library() const { return _library; }

                /** @brief Returns its face's index in the library (the glyph cache's id). */
                uint16_t faceIndex() const { return _face; }

                /** @brief Returns the pixel size (a bitmap face's: its height times the scale). */
                unsigned pixelSize() const { return _size; }

                /** @brief Returns a bitmap face's multiple (1 for TrueType). */
                unsigned scale() const { return _scale; }

                /** @brief Returns true if its bold is made up. */
                bool isMadeUpBold() const { return _madeUpBold; }

                /** @brief Returns true for a bitmap face. */
                bool isBitmap() const;

                /** @brief Returns the bitmap face (null for TrueType). */
                const BitmapFont *bitmap() const;

                /** @brief Returns the TrueType face (null for a bitmap one). */
                const FontFace *face() const;

                /** @brief Returns the metrics. */
                const Metrics &metrics() const { return _metrics; }

                /** @brief Returns what it is: its face's family and style, its size. */
                FontName name() const;

                /**
                 * @brief Lays out @p utf8 on one line: each character's glyph (from this
                 *        face, else a fallback face, else unscii, else the missing glyph),
                 *        kerned within a face, pens kept in 1/64 pixels and snapped.
                 * @return The line's advance, in 1/64 pixels (to where it stopped).
                 */
                int64_t layout(StringView utf8, Visit visit, void *arg) const;

                /** @brief Returns @p utf8's advance in whole pixels (rounded up). */
                uint32_t textWidth(StringView utf8) const;

                /** @brief Returns how many bytes of @p utf8 fit in @p maxWidth pixels, whole characters only. */
                size_t fitting(StringView utf8, uint32_t maxWidth) const;

        private:
                friend class FontLibrary;

                void retain() const;
                void drop();

                FontLibrary *_library = nullptr;
                Metrics      _metrics;
                int32_t      _unitsTo64 = 0; // TrueType: font units to 1/64 pixels, 16.16
                uint16_t     _face = 0;
                uint16_t     _size = 0;
                uint8_t      _scale = 1;
                bool         _madeUpBold = false;
};

BMDS_NAMESPACE_END
