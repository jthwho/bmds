/**
 * @file      painter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Drawing on a surface through a window's visible pieces
 * (devplan/windows.md W8, §4a).
 */

#include <cstring>
#include <bmds/painter.h>
#include <bmds/surface.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t AlignMask = Surface::Align - 1u;

        // Four pixels of each pattern of 4 bits (the leftmost the highest):
        // 12 bytes, three words.
        struct NibbleTable {
                        uint32_t words[16][3];

                        NibbleTable(uint32_t fg, uint32_t bg) {
                                for (unsigned n = 0; n < 16; ++n) {
                                        uint8_t bytes[12];
                                        for (unsigned p = 0; p < 4; ++p) {
                                                const uint32_t c = (n & (8u >> p)) != 0 ? fg : bg;
                                                bytes[3 * p] = static_cast<uint8_t>(c);
                                                bytes[3 * p + 1] = static_cast<uint8_t>(c >> 8);
                                                bytes[3 * p + 2] = static_cast<uint8_t>(c >> 16);
                                        }
                                        std::memcpy(words[n], bytes, sizeof(bytes));
                                }
                        }
        };

        // @p rows of a glyph (@p bytesPerRow each) expanded at @p to (word
        // aligned), @p stride bytes a row.
        void expandGlyph(const NibbleTable &table, const uint8_t *rows, unsigned bytesPerRow, unsigned height,
                         uint8_t *to, size_t stride) {
                for (unsigned y = 0; y < height; ++y, to += stride) {
                        auto *out = reinterpret_cast<uint32_t *>(to);
                        for (unsigned b = 0; b < bytesPerRow; ++b, out += 6) {
                                const uint8_t   bits = *rows++;
                                const uint32_t *hi = table.words[bits >> 4];
                                const uint32_t *lo = table.words[bits & 15];
                                out[0] = hi[0];
                                out[1] = hi[1];
                                out[2] = hi[2];
                                out[3] = lo[0];
                                out[4] = lo[1];
                                out[5] = lo[2];
                        }
                }
                return;
        }

} // namespace

Rect Painter::clipBounds() const {
        Rect bounds;
        for (const Rect &piece : _clip) bounds = bounds.united(piece);
        if (bounds.isEmpty()) return bounds;
        // In the painter's coordinates (a window's clip is never left of or above its origin).
        bounds.x = static_cast<uint16_t>(bounds.x > _originX ? bounds.x - _originX : 0);
        bounds.y = static_cast<uint16_t>(bounds.y > _originY ? bounds.y - _originY : 0);
        return bounds;
}

bool Painter::isVisible(const Rect &rect) const {
        const Rect r{static_cast<uint16_t>(rect.x + _originX), static_cast<uint16_t>(rect.y + _originY), rect.width,
                     rect.height};
        for (const Rect &piece : _clip)
                if (piece.intersects(r)) return true;
        return false;
}

void Painter::flush(uint32_t rgb) {
        if (_batched == 0) return;
        ++_calls;
        note(_surface.fill(Span<const Rect>(_batch, _batched), rgb, _timeoutMs));
        _batched = 0;
        return;
}

Error Painter::fill(Span<const Rect> rects, uint32_t rgb) {
        for (const Rect &rect : rects) {
                if (rect.isEmpty()) continue;
                // On the surface, its sides out to 4-pixel columns.
                const uint32_t left = (uint32_t(rect.x) + _originX) & ~AlignMask;
                const uint32_t right = (uint32_t(rect.right()) + _originX + AlignMask) & ~AlignMask;
                const uint32_t top = uint32_t(rect.y) + _originY;
                const uint32_t bottom = uint32_t(rect.bottom()) + _originY;
                if (right > UINT16_MAX || bottom > UINT16_MAX) continue; // past any surface
                const Rect r{static_cast<uint16_t>(left), static_cast<uint16_t>(top),
                             static_cast<uint16_t>(right - left), static_cast<uint16_t>(bottom - top)};
                for (const Rect &piece : _clip) {
                        const Rect cut = r.intersected(piece);
                        if (cut.isEmpty()) continue;
                        if (_batched == BatchRects) flush(rgb);
                        _batch[_batched++] = cut;
                }
        }
        flush(rgb);
        return _error;
}

Error Painter::text(uint16_t x, uint16_t y, StringView utf8, const Font &font, uint32_t fg, uint32_t bg,
                    uint16_t maxWidth) {
        const uint32_t sx = uint32_t(x) + _originX, sy = uint32_t(y) + _originY;
        if ((sx & AlignMask) != 0 || _strip.height < font.height() || sx > UINT16_MAX || sy > UINT16_MAX) {
                note(Error::Invalid);
                return _error;
        }
        const uint16_t limit = maxWidth < _strip.width ? maxWidth : _strip.width;
        Rect           area{static_cast<uint16_t>(sx), static_cast<uint16_t>(sy), limit, font.height()};
        bool           visible = false;
        for (const Rect &piece : _clip) visible |= piece.intersects(area);
        if (!visible) return _error;

        // The glyphs into the strip, left to right.
        const NibbleTable table(fg, bg);
        const size_t      stride = size_t(_strip.width) * Surface::BytesPerPixel;
        uint16_t          width = 0;
        for (size_t i = 0; i < utf8.size();) {
                const Glyph glyph = font.glyph(Utf8::decode(utf8, i));
                if (width + glyph.width > limit) break;
                expandGlyph(table, glyph.rows, glyph.width / 8u, font.height(),
                            _strip.data + size_t(width) * Surface::BytesPerPixel, stride);
                width = static_cast<uint16_t>(width + glyph.width);
        }
        area.width = width;

        // Copied piece by piece.
        for (const Rect &piece : _clip) {
                const Rect cut = area.intersected(piece);
                if (cut.isEmpty()) continue;
                const uint8_t *src =
                        _strip.data + size_t(cut.y - area.y) * stride + size_t(cut.x - area.x) * Surface::BytesPerPixel;
                ++_calls;
                note(_surface.blit(cut, src, stride, _timeoutMs));
        }
        return _error;
}

BMDS_NAMESPACE_END
