/**
 * @file      fontface.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief One TrueType font file's face (`devplan/fonts.md` §4a): its names,
 *        metrics, glyphs, kerning, and its glyphs rendered as coverage.
 *
 * Reads a file in memory it doesn't own (in `/rom`, where it lies in flash;
 * elsewhere, a copy in PSRAM). stb_truetype draws the outlines, but it checks
 * nothing, so `open()` checks the tables it reads, and each glyph is checked
 * before it is drawn: a damaged or hostile file is refused, never read
 * outside its bytes. Character lookup, names and kerning are read here, with
 * bounds checks; kerning adds up every pair-adjustment lookup of the `kern`
 * feature (GPOS), or the `kern` table's pairs.
 *
 * Only TrueType outlines (`glyf`): a CFF font (`OTTO`) or a collection
 * (`ttcf`) is `NotSupported`.
 *
 * Sizes are pixels per em, as desktops name them: a 13 px font's em is 13
 * pixels. Glyph boxes are in pixels from the pen on the baseline, y down.
 *
 * @par Thread Safety
 * `open()` once; then every const call from any task at once.
 */
class FontFace {
        public:
                /** @brief How a glyph's coverage is kept. */
                enum class Depth : uint8_t {
                        Mono = 1,  ///< 1 bit a pixel, the most significant leftmost: sharp.
                        Gray4 = 4, ///< 4 bits a pixel, the high nibble leftmost, 15 fully covered: smooth.
                };

                /** @brief The face's vertical metrics, in font units (`hhea`). */
                struct Metrics {
                                int16_t  ascent = 0;     ///< Above the baseline (positive).
                                int16_t  descent = 0;    ///< Below it (negative).
                                int16_t  lineGap = 0;    ///< Between one line's descent and the next's ascent.
                                uint16_t maxAdvance = 0; ///< The widest advance.
                };

                /** @brief A glyph's rendered box, in pixels. */
                struct GlyphBox {
                                int16_t  x = 0;     ///< Its left edge, from the pen.
                                int16_t  y = 0;     ///< Its top edge, from the baseline (negative: above).
                                uint16_t width = 0; ///< 0 for a glyph with no ink (a space).
                                uint16_t height = 0;

                                /** @brief Returns a row's bytes at @p depth. */
                                size_t stride(Depth depth) const {
                                        return (size_t(width) * static_cast<uint8_t>(depth) + 7) / 8;
                                }

                                /** @brief Returns the bytes `render()` needs to work in (a byte a pixel). */
                                size_t workBytes() const { return size_t(width) * height; }
                };

                /** @brief The smallest pixel size drawn. */
                static constexpr unsigned MinPixelSize = 4;

                /** @brief The largest pixel size drawn. */
                static constexpr unsigned MaxPixelSize = 255;

                /** @brief The longest family or style name kept, in bytes (UTF-8). */
                static constexpr size_t MaxName = 63;

                /** @brief A face over nothing (`open()` it). */
                FontFace();

                FontFace(const FontFace &) = delete;
                FontFace &operator=(const FontFace &) = delete;

                /**
                 * @brief Checks @p data as a TrueType font and opens its face. @p data
                 *        must stay valid and unchanged while the face is used.
                 * @return `Ok`, `CorruptData` (not a font, or damaged), or
                 *         `NotSupported` (CFF outlines, a collection).
                 */
                Error open(ByteSpan data);

                /** @brief Returns true once `open()` has succeeded. */
                bool isOpen() const { return _glyphCount != 0; }

                /** @brief Returns the font's bytes. */
                ByteSpan data() const { return _data; }

                /** @brief Returns the family's name ("IBM Plex Sans"): the typographic one if given. */
                StringView family() const { return StringView(_family, _familyLength); }

                /** @brief Returns the style's name ("Bold Italic"): the typographic one if given. */
                StringView style() const { return StringView(_style, _styleLength); }

                /** @brief Returns the weight, 1-1000: 400 regular, 700 bold (`OS/2`, else `head`). */
                uint16_t weight() const { return _weight; }

                /** @brief Returns true for an italic or oblique face (`OS/2`, else `head`). */
                bool isItalic() const { return _italic; }

                /** @brief Returns true if the font says every glyph is as wide (`post`). */
                bool isFixedPitch() const { return _fixedPitch; }

                /** @brief Returns the number of glyphs. */
                uint16_t glyphCount() const { return _glyphCount; }

                /** @brief Returns the font units in an em. */
                uint16_t unitsPerEm() const { return _unitsPerEm; }

                /** @brief Returns the vertical metrics. */
                const Metrics &metrics() const { return _metrics; }

                /** @brief Returns font units to pixels at @p pixelSize. */
                float scale(unsigned pixelSize) const { return float(pixelSize) / float(_unitsPerEm); }

                /**
                 * @brief Returns @p codepoint's glyph, or 0 (the font's missing glyph).
                 * @hotpath The first 256 codepoints from a table.
                 */
                uint16_t glyphIndex(uint32_t codepoint) const;

                /** @brief Returns @p glyph's advance in font units (0 for a glyph not in the font). */
                int advance(uint16_t glyph) const;

                /**
                 * @brief Returns the kerning between @p left and @p right, in font units
                 *        (negative: closer).
                 */
                int kerning(uint16_t left, uint16_t right) const;

                /** @brief Returns true if the font kerns at all. */
                bool hasKerning() const { return _kernLookupCount != 0 || _kern != 0; }

                /**
                 * @brief Returns @p glyph's box at @p pixelSize.
                 * @return The box (empty for a glyph with no ink), `OutOfRange` (the
                 *         glyph or size), or `CorruptData` (a damaged glyph).
                 */
                Result<GlyphBox> box(uint16_t glyph, unsigned pixelSize) const;

                /**
                 * @brief Renders @p glyph at @p pixelSize into @p out, as `box()` gave
                 *        @p box: rows of `box.stride(depth)` bytes, top first.
                 *        @p out holds at least `box.workBytes()` bytes (it is worked in).
                 *        The outline's memory comes from `Internal` (or `Psram` if that
                 *        is short) and is freed before it returns. @p curve, if given,
                 *        maps each pixel's 8-bit coverage before it is reduced to @p depth
                 *        (the contrast, `GlyphCache::setCurve()`).
                 * @return `Ok`, `BufferTooSmall`, `OutOfRange`, `CorruptData`, `NoMem`.
                 */
                Error render(uint16_t glyph, unsigned pixelSize, Depth depth, const GlyphBox &box, Span<uint8_t> out,
                             const uint8_t *curve = nullptr) const;

        private:
                static constexpr size_t MaxKernLookups = 16;
                static constexpr size_t StbBytes = 192;

                Error    checkGlyph(uint16_t glyph, unsigned depth, uint32_t &points) const;
                bool     glyphRange(uint16_t glyph, uint32_t &start, uint32_t &end) const;
                int      gposKerning(uint16_t left, uint16_t right) const;
                int      kernTableKerning(uint16_t left, uint16_t right) const;
                bool     readName(uint16_t id, char *out, uint8_t &length) const;
                uint16_t cmapLookup(uint32_t codepoint) const;

                ByteSpan _data;
                Metrics  _metrics;
                uint32_t _glyf = 0, _glyfLength = 0;
                uint32_t _loca = 0;
                uint32_t _hmtx = 0;
                uint32_t _cmap = 0, _cmapEnd = 0; // the subtable used, to the cmap table's end
                uint32_t _name = 0, _nameLength = 0;
                uint32_t _gpos = 0, _gposLength = 0;
                uint32_t _kern = 0, _kernLength = 0;
                uint16_t _kernLookups[MaxKernLookups] = {}; // GPOS lookups of `kern`, in order
                uint8_t  _kernLookupCount = 0;
                uint16_t _cmapFormat = 0; // 4 or 12
                uint16_t _glyphCount = 0;
                uint16_t _hMetricCount = 0;
                uint16_t _unitsPerEm = 0;
                bool     _longLoca = false;
                bool     _fixedPitch = false;
                bool     _italic = false;
                uint16_t _weight = 400;
                uint8_t  _familyLength = 0, _styleLength = 0;
                char     _family[MaxName + 1] = {};
                char     _style[MaxName + 1] = {};
                uint16_t _latin[256] = {};                         // the first 256 codepoints' glyphs
                alignas(void *) unsigned char _stb[StbBytes] = {}; // stbtt_fontinfo
};

BMDS_NAMESPACE_END
