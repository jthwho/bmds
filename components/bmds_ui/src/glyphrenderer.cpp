/**
 * @file      glyphrenderer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Glyphs expanded into RGB888 pixels: a scalar kernel and the ESP32-P4's
 * PIE one (devplan/windows.md §4d).
 */

#include <cstring>
#include <bmds/glyphrenderer.h>
#include <bmds/utf8.h>

#if defined(ESP_PLATFORM) && defined(__riscv_xespv) && defined(__riscv_xesploop)
#define BMDS_GLYPHS_PIE 1
#else
#define BMDS_GLYPHS_PIE 0
#endif

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned RowBytes = 24; // 8 pixels

        // Each nibble's 4 pixels (12 bytes) as a mask: 0xff bytes where a bit is set.
        struct NibbleMasks {
                        uint32_t words[16][3];
        };

        constexpr NibbleMasks makeNibbleMasks() {
                NibbleMasks m{};
                for (unsigned n = 0; n < 16; ++n)
                        for (unsigned i = 0; i < 12; ++i)
                                if ((n & (8u >> (i / 3))) != 0) m.words[n][i / 4] |= 0xffu << (8 * (i % 4));
                return m;
        }

        constexpr NibbleMasks Nibbles = makeNibbleMasks();

        // 4 pixels of @p rgb as three words (B, G, R bytes).
        void pixelWords(uint32_t rgb, uint32_t (&w)[3]) {
                const uint32_t b = rgb & 0xff, g = (rgb >> 8) & 0xff, r = (rgb >> 16) & 0xff;
                w[0] = b | g << 8 | r << 16 | b << 24;
                w[1] = g | r << 8 | b << 16 | g << 24;
                w[2] = r | b << 8 | g << 16 | r << 24;
                return;
        }

#if BMDS_GLYPHS_PIE
        // Each byte of a row of 8 pixels (3 a pixel) and its pixel's bit: the
        // first 16 bytes, then the last 8 (the rest of the register unused).
        struct Selectors {
                        alignas(16) uint8_t bytes[32];
        };

        constexpr Selectors makeSelectors() {
                Selectors s{};
                for (unsigned i = 0; i < RowBytes; ++i) s.bytes[i] = static_cast<uint8_t>(0x80u >> (i / 3));
                for (unsigned i = RowBytes; i < 32; ++i) s.bytes[i] = 0xff;
                return s;
        }

        constexpr Selectors PieSelectors = makeSelectors();
#endif

} // namespace

bool GlyphRenderer::isSupported(Kernel kernel) {
        return kernel == Kernel::Scalar || (kernel == Kernel::Pie && BMDS_GLYPHS_PIE != 0);
}

GlyphRenderer::Kernel GlyphRenderer::best() {
        return BMDS_GLYPHS_PIE != 0 ? Kernel::Pie : Kernel::Scalar;
}

void GlyphRenderer::setColors(uint32_t fg, uint32_t bg) {
        if (_valid && fg == _fg && bg == _bg) return;
        _fg = fg;
        _bg = bg;
        _valid = true;
        uint32_t back[3], diff[3];
        pixelWords(bg, back);
        pixelWords(fg ^ bg, diff);
        if (_kernel == Kernel::Pie) {
                // 8 pixels: the 4-pixel words twice; the background at 0, the difference at 32.
                auto *p = reinterpret_cast<uint32_t *>(_pattern);
                for (unsigned i = 0; i < 6; ++i) {
                        p[i] = back[i % 3];
                        p[8 + i] = diff[i % 3];
                }
                return;
        }
        for (unsigned n = 0; n < 16; ++n)
                for (unsigned i = 0; i < 3; ++i) _table[n][i] = back[i] ^ (diff[i] & Nibbles.words[n][i]);
        return;
}

// @p glyph's rows, or (bold, underline) a styled copy of them in @p styled.
const uint8_t *GlyphRenderer::style(const Glyph &glyph, unsigned height, uint8_t flags, uint8_t *styled) const {
        const unsigned bytesPerRow = glyph.width / 8u;
        if (flags == 0 || bytesPerRow > 2) return glyph.rows;
        // Bold: each row ORed with itself a pixel right; underline: the last row set.
        std::memcpy(styled, glyph.rows, size_t(height) * bytesPerRow);
        if ((flags & TextCell::Bold) != 0) {
                for (unsigned r = 0; r < height; ++r) {
                        uint8_t *row = styled + r * bytesPerRow;
                        uint8_t  carry = 0;
                        for (unsigned b = 0; b < bytesPerRow; ++b) {
                                const uint8_t v = row[b];
                                row[b] = static_cast<uint8_t>(v | v >> 1 | carry);
                                carry = static_cast<uint8_t>(v << 7);
                        }
                }
        }
        if ((flags & TextCell::Underline) != 0 && height > 0)
                std::memset(styled + (height - 1) * bytesPerRow, 0xff, bytesPerRow);
        return styled;
}

void GlyphRenderer::draw(const Glyph &glyph, unsigned height, uint8_t flags, uint32_t fg, uint32_t bg, uint8_t *out,
                         size_t stride) {
        if (height > MaxHeight) height = MaxHeight;
        uint8_t        styled[MaxHeight * 2];
        const uint8_t *rows = style(glyph, height, flags, styled);
        setColors(fg, bg);
        begin();
        loadColors();
        put(rows, glyph.width / 8u, height, out, stride);
        return;
}

uint16_t GlyphRenderer::drawCells(Span<const TextCell> cells, const Font &font, uint8_t *out, size_t stride,
                                  uint16_t maxWidth, size_t &used) {
        const unsigned height = font.height() < MaxHeight ? font.height() : MaxHeight;
        uint8_t        styled[MaxHeight * 2];
        uint32_t       last = 0xffffffffu;
        Glyph          glyph;
        uint16_t       width = 0;
        used = 0;
        if (out == nullptr) { // measured only: the cells that fit, nothing drawn
                for (const TextCell &cell : cells) {
                        if (cell.codepoint != last) {
                                glyph = font.glyph(cell.codepoint);
                                last = cell.codepoint;
                        }
                        if (width + glyph.width > maxWidth) break;
                        width = static_cast<uint16_t>(width + glyph.width);
                        ++used;
                }
                return width;
        }
        begin();
        bool loaded = false;
        for (const TextCell &cell : cells) {
                if (cell.codepoint != last) {
                        glyph = font.glyph(cell.codepoint);
                        last = cell.codepoint;
                }
                if (width + glyph.width > maxWidth) break;
                if (!loaded || cell.fg != _fg || cell.bg != _bg) {
                        setColors(cell.fg, cell.bg);
                        loadColors();
                        loaded = true;
                }
                const uint8_t *rows = cell.flags == 0 ? glyph.rows : style(glyph, height, cell.flags, styled);
                put(rows, glyph.width / 8u, height, out + size_t(width) * 3, stride);
                width = static_cast<uint16_t>(width + glyph.width);
                ++used;
        }
        return width;
}

uint16_t GlyphRenderer::drawText(StringView utf8, const Font &font, uint32_t fg, uint32_t bg, uint8_t *out,
                                 size_t stride, uint16_t maxWidth, size_t &used) {
        const unsigned height = font.height() < MaxHeight ? font.height() : MaxHeight;
        uint16_t       width = 0;
        used = 0;
        if (out != nullptr) { // null: measured only
                setColors(fg, bg);
                begin();
                loadColors();
        }
        for (size_t i = 0; i < utf8.size();) {
                const Glyph glyph = font.glyph(Utf8::decode(utf8, i));
                if (width + glyph.width > maxWidth) break;
                if (out != nullptr) put(glyph.rows, glyph.width / 8u, height, out + size_t(width) * 3, stride);
                width = static_cast<uint16_t>(width + glyph.width);
                used = i;
        }
        return width;
}

void GlyphRenderer::drawScalar(const uint8_t *rows, unsigned bytesPerRow, unsigned height, uint8_t *out,
                               size_t stride) const {
        for (unsigned y = 0; y < height; ++y, out += stride) {
                auto *o = reinterpret_cast<uint32_t *>(out);
                for (unsigned b = 0; b < bytesPerRow; ++b, o += 6) {
                        const uint8_t   bits = *rows++;
                        const uint32_t *hi = _table[bits >> 4];
                        const uint32_t *lo = _table[bits & 15];
                        o[0] = hi[0];
                        o[1] = hi[1];
                        o[2] = hi[2];
                        o[3] = lo[0];
                        o[4] = lo[1];
                        o[5] = lo[2];
                }
        }
        return;
}

#if BMDS_GLYPHS_PIE

namespace {

        // A column of 8 pixels of a glyph (every @p Step-th byte of @p rows is
        // a row's), @p height rows, with the selectors in q0-q1, the
        // background in q2-q3 and the colors' difference in q4-q5: each row's
        // byte broadcast, compared into a mask of its pixels' bytes, blended
        // (bg ^ (mask & diff)) and stored as three 64-bit halves.
        //
        // A branch loop, not the hardware loop (`esp.lp.setup`): on this chip's revision a hardware loop
        // preempted part-way can resume wrong (ESP-IDF's HWLOOP_STATE_BUG; devplan/simd.md §1).
        template <unsigned Step> void pieColumn(const uint8_t *rows, unsigned height, uint8_t *out, size_t step) {
                asm volatile("1: esp.vldbc.8.ip q6, %[g], %[gs]\n"
                             "esp.andq q7, q6, q0\n" // bytes 0-15: their pixel's bit
                             "esp.vcmp.eq.u8 q7, q7, q0\n"
                             "esp.andq q6, q6, q1\n" // bytes 16-23
                             "esp.vcmp.eq.u8 q6, q6, q1\n"
                             "esp.andq q7, q7, q4\n"
                             "esp.xorq q7, q7, q2\n"
                             "esp.andq q6, q6, q5\n"
                             "esp.xorq q6, q6, q3\n"
                             "esp.vst.l.64.ip q7, %[o], 8\n"
                             "esp.vst.h.64.ip q7, %[o], 8\n"
                             "addi %[n], %[n], -1\n"
                             "esp.vst.l.64.xp q6, %[o], %[s]\n"
                             "bnez %[n], 1b\n"
                             : [g] "+cr"(rows), [o] "+cr"(out), [n] "+r"(height)
                             : [s] "cr"(step), [gs] "i"(Step)
                             : "memory");
                return;
        }

} // namespace

void GlyphRenderer::begin() {
        if (_kernel != Kernel::Pie) return;
        const uint8_t *sel = PieSelectors.bytes;
        asm volatile("esp.vld.128.ip q0, %[sel], 16\n"
                     "esp.vld.128.ip q1, %[sel], 16\n"
                     : [sel] "+cr"(sel)
                     :
                     : "memory");
        return;
}

void GlyphRenderer::loadColors() {
        if (_kernel != Kernel::Pie) return;
        const uint8_t *pat = _pattern;
        asm volatile("esp.vld.128.ip q2, %[pat], 16\n"
                     "esp.vld.128.ip q3, %[pat], 16\n"
                     "esp.vld.128.ip q4, %[pat], 16\n"
                     "esp.vld.128.ip q5, %[pat], 16\n"
                     : [pat] "+cr"(pat)
                     :
                     : "memory");
        return;
}

void GlyphRenderer::put(const uint8_t *rows, unsigned bytesPerRow, unsigned height, uint8_t *out, size_t stride) const {
        if (_kernel != Kernel::Pie) {
                drawScalar(rows, bytesPerRow, height, out, stride);
                return;
        }
        if (height == 0) return;
        const size_t step = stride - 16;
        if (bytesPerRow == 1) {
                pieColumn<1>(rows, height, out, step);
        } else {
                pieColumn<2>(rows, height, out, step);
                pieColumn<2>(rows + 1, height, out + RowBytes, step);
        }
        return;
}

#else

void GlyphRenderer::begin() {
        return;
}

void GlyphRenderer::loadColors() {
        return;
}

void GlyphRenderer::put(const uint8_t *rows, unsigned bytesPerRow, unsigned height, uint8_t *out, size_t stride) const {
        drawScalar(rows, bytesPerRow, height, out, stride);
        return;
}

#endif

BMDS_NAMESPACE_END
