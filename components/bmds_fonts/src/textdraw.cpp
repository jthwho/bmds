/**
 * @file      textdraw.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <new>
#include <bmds/bitmapfont.h>
#include <bmds/fontlibrary.h>
#include <bmds/glyphcache.h>
#include <bmds/textdraw.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The glyphs laid out and waiting to be drawn, a run of them fetched from the cache at once.
        constexpr size_t Batch = 32;

        // Columns a chunk at a time, and the band of rows composed at once (the scratch's).
        constexpr int    Chunk = 512;
        constexpr size_t Band = 2048;

        // Where a TrueType glyph at @p pen (1/64 px from the line's start) is drawn: smoothed, at the
        // nearest phase (FontFace::Phases a pixel) from its pixel; sharp, at the nearest pixel.
        struct Landing {
                        int     x = 0;
                        uint8_t phase = 0;
        };

        Landing landing(int64_t pen, bool sharp) {
                static_assert(FontFace::Phases == 4, "quarters: 16/64 px each");
                if (sharp) return Landing{int((pen + 32) >> 6), 0};
                const int64_t q = (pen + 8) >> 4; // the pen in quarters, rounded (floored below 0)
                return Landing{int(q >> 2), uint8_t(q & 3)};
        }

        // A glyph of the batch where it lands: its cached coverage, or a bitmap font's.
        struct Piece {
                        const GlyphCache::Glyph *cached = nullptr;
                        const Font::Placed      *placed = nullptr;
                        Glyph                    bitmap; // a bitmap font's glyph
                        int                      x0 = 0, y0 = 0, width = 0, height = 0;
        };

        struct Drawing {
                        Drawing(const TextTarget &t, const Font &f, const TextPaint &p, Simd::Kernel k)
                            : target(t), font(f), paint(p), kernel(k) {}

                        const TextTarget &target;
                        const Font       &font;
                        const TextPaint  &paint;
                        Simd::Kernel      kernel;
                        TextBlend::Ramp   ramp;               // opaque: background to color
                        bool              blendOnBox = false; // opaque: blended over the filled box instead (PIE)
                        FontFace::Depth   depth = FontFace::Depth::Gray4;
                        int               x = 0;
                        int               baseline = 0;
                        int               left = 0, top = 0, right = 0, bottom = 0; // the clip, inside the picture
                        int64_t           limit = INT64_MAX;                        // 1/64 px
                        int64_t           end = 0;                                  // the drawn line's advance
                        int               filledTo = INT32_MIN; // opaque: the box filled up to this column
                        Font::Placed     *batch = nullptr;      // the scratch's
                        Piece            *pieces = nullptr;     // and the batch's places
                        uint8_t          *band = nullptr;       // and a band's coverage (Band bytes)
                        size_t            count = 0;
                        Error             error;
        };

        // Combines @p p's rows from @p y0 to @p y1 between columns @p c0 and @p c1 into @p band
        // (rows of @p width bytes from (c0, y0)).
        void composePiece(const Drawing &d, const Piece &p, int c0, int c1, int y0, int y1, uint8_t *band, int width) {
                const int a = p.x0 > c0 ? p.x0 : c0, b = p.x0 + p.width < c1 ? p.x0 + p.width : c1;
                const int r0 = p.y0 > y0 ? p.y0 : y0, r1 = p.y0 + p.height < y1 ? p.y0 + p.height : y1;
                if (a >= b || r0 >= r1) return;
                uint8_t *out = band + size_t(r0 - y0) * size_t(width) + (a - c0);
                if (p.cached != nullptr) {
                        const uint8_t *cov = p.cached->rows + size_t(r0 - p.y0) * p.cached->stride;
                        for (int y = r0; y < r1; ++y, cov += p.cached->stride, out += width)
                                TextBlend::compose(cov, p.cached->depth, unsigned(a - p.x0), unsigned(b - a), out);
                        return;
                }
                // A bitmap font's: each bit a scale × scale block, a made-up bold one a pixel wider.
                const unsigned s = p.placed->scale, bytes = p.bitmap.width / 8u;
                const int      extra = d.font.isMadeUpBold() ? 1 : 0;
                for (int y = r0; y < r1; ++y, out += width) {
                        const uint8_t *bits = p.bitmap.rows + size_t(unsigned(y - p.y0) / s) * bytes;
                        for (unsigned px = 0; px < p.bitmap.width; ++px) {
                                if ((bits[px / 8] & (0x80 >> (px % 8))) == 0) continue;
                                int u = p.x0 + int(px * s), v = u + int(s) + extra;
                                if (u < a) u = a;
                                if (v > b) v = b;
                                for (int k = u; k < v; ++k) out[k - a] = 255;
                        }
                }
                return;
        }

        // Draws the batch: each run of one TrueType face looked up at once (misses rendered), then
        // the line's rows across the whole run, its glyphs' levels combined and drawn by one call.
        void flush(Drawing &d) {
                GlyphCache    &cache = d.font.library()->cache();
                Piece         *pieces = d.pieces;
                for (size_t i = 0; i < d.count; ++i) pieces[i] = Piece();
                const unsigned size = d.font.pixelSize();
                const bool     bold = d.font.isMadeUpBold();
                for (size_t i = 0; i < d.count && d.error.isOk();) {
                        const Font::Placed &p = d.batch[i];
                        if (p.bitmap != nullptr) {
                                Piece &piece = pieces[i];
                                piece.placed = &p;
                                piece.bitmap = p.bitmap->glyph(p.glyph);
                                piece.x0 = d.x + p.x;
                                piece.y0 = d.baseline - int(p.ascent);
                                piece.width = int(piece.bitmap.width * p.scale) + (bold ? 1 : 0);
                                piece.height = int(p.bitmap->height() * p.scale);
                                ++i;
                                continue;
                        }
                        size_t j = i;
                        while (j < d.count && d.batch[j].bitmap == nullptr && d.batch[j].faceIndex == p.faceIndex) ++j;
                        const bool               sharp = d.depth == FontFace::Depth::Mono;
                        uint16_t                 ids[Batch];
                        uint8_t                  phases[Batch];
                        int                      xs[Batch];
                        const GlyphCache::Glyph *found[Batch];
                        for (size_t k = i; k < j; ++k) {
                                const Landing at = landing(d.batch[k].pen, sharp);
                                ids[k - i] = uint16_t(d.batch[k].glyph);
                                phases[k - i] = at.phase;
                                xs[k - i] = at.x;
                        }
                        (void)cache.lookup(p.faceIndex, Span<const uint16_t>(ids, j - i), size, d.depth, bold,
                                           Span<const GlyphCache::Glyph *>(found, j - i),
                                           Span<const uint8_t>(phases, j - i));
                        for (size_t k = i; k < j; ++k) {
                                const GlyphCache::Glyph *g = found[k - i];
                                if (g == nullptr && d.error.isOk()) {
                                        auto [made, err] = cache.acquire(p.faceIndex, *p.face, ids[k - i], size,
                                                                         d.depth, bold, phases[k - i]);
                                        if (err.isError())
                                                d.error = err;
                                        else
                                                g = made;
                                }
                                Piece &piece = pieces[k];
                                piece.placed = &d.batch[k];
                                piece.cached = g;
                                if (g != nullptr) {
                                        piece.x0 = d.x + xs[k - i] + g->x;
                                        piece.y0 = d.baseline + g->y;
                                        piece.width = g->width;
                                        piece.height = g->height;
                                }
                        }
                        i = j;
                }

                // The run's box, clipped.
                int x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
                for (size_t i = 0; i < d.count; ++i) {
                        const Piece &p = pieces[i];
                        if (p.width == 0 || p.height == 0) continue;
                        if (p.x0 < x0) x0 = p.x0;
                        if (p.x0 + p.width > x1) x1 = p.x0 + p.width;
                        if (p.y0 < y0) y0 = p.y0;
                        if (p.y0 + p.height > y1) y1 = p.y0 + p.height;
                }
                if (x0 < d.left) x0 = d.left;
                if (x1 > d.right) x1 = d.right;
                if (y0 < d.top) y0 = d.top;
                if (y1 > d.bottom) y1 = d.bottom;
                const unsigned bpp = TextBlend::bytesPerPixel(d.target.format);
                if (d.paint.opaque && d.count > 0) {
                        // This run's part of the box: from where the last run's ended (its ink, maybe past its
                        // advance) to the end of this one's advance or ink, the line's height.
                        const Font::Placed &last = d.batch[d.count - 1];
                        int                 from = d.filledTo != INT32_MIN ? d.filledTo : d.x + d.batch[0].x;
                        int                 to = d.x + int((last.pen + last.advance + 63) >> 6);
                        if (x1 > to && x1 < INT32_MAX) to = x1;
                        d.filledTo = to;
                        if (from < d.left) from = d.left;
                        if (to > d.right) to = d.right;
                        const int lineTop = d.baseline - d.font.metrics().ascent;
                        int       r0 = lineTop < d.top ? d.top : lineTop;
                        const int r1 = lineTop + d.font.metrics().lineHeight > d.bottom
                                               ? d.bottom
                                               : lineTop + d.font.metrics().lineHeight;
                        for (; from < to && r0 < r1; ++r0)
                                TextBlend::fill(d.target.pixels + size_t(r0) * d.target.stride + size_t(from) * bpp,
                                                d.target.format, unsigned(to - from), d.paint.background);
                }
                // Bands of rows across a chunk of columns, composed a glyph at a time, then each row drawn
                // by one call: the scratch's band (Band bytes).
                uint8_t *band = d.band;
                for (int c0 = x0; d.error.isOk() && c0 < x1; c0 += Chunk) {
                        const int c1 = c0 + Chunk < x1 ? c0 + Chunk : x1, w = c1 - c0;
                        const int rows = int(Band) / w;
                        for (int b0 = y0; b0 < y1; b0 += rows) {
                                const int b1 = b0 + rows < y1 ? b0 + rows : y1;
                                std::memset(band, 0, size_t(b1 - b0) * size_t(w));
                                for (size_t i = 0; i < d.count; ++i)
                                        composePiece(d, pieces[i], c0, c1, b0, b1, band, w);
                                const uint8_t *alpha = band;
                                for (int y = b0; y < b1; ++y, alpha += w) {
                                        uint8_t *at = d.target.pixels + size_t(y) * d.target.stride + size_t(c0) * bpp;
                                        if (d.paint.opaque && !d.blendOnBox)
                                                TextBlend::paint(at, alpha, unsigned(w), d.ramp);
                                        else
                                                TextBlend::blend(at, d.target.format, alpha, unsigned(w), d.paint.color,
                                                                 d.kernel);
                                }
                        }
                }
                for (size_t i = 0; i < d.count; ++i) cache.release(pieces[i].cached);
                d.count = 0;
                return;
        }

        bool visit(void *arg, const Font::Placed &g) {
                auto *d = static_cast<Drawing *>(arg);
                if (g.pen + g.advance > d->limit) return false;
                d->end = g.pen + g.advance;
                d->batch[d->count++] = g;
                if (d->count == Batch) flush(*d);
                return d->error.isOk();
        }

} // namespace

struct TextDraw::Scratch {
                Font::Placed batch[Batch];
                Piece        pieces[Batch];
                uint8_t      band[Band];
};

size_t TextDraw::scratchBytes() {
        return sizeof(Scratch);
}

TextDraw::Scratch *TextDraw::makeScratch(void *memory) {
        return memory != nullptr ? ::new (memory) Scratch() : nullptr;
}

namespace {

        Result<uint32_t> drawLine(const TextTarget &target, const Font &font, StringView utf8, int x, int top,
                                  const TextPaint &paint, uint32_t maxWidth, Simd::Kernel kernel,
                                  TextDraw::Scratch &work);

        // line() without a scratch: one on this frame's stack, which callers with one never pay for.
        __attribute__((noinline)) Result<uint32_t> drawOnStack(const TextTarget &target, const Font &font,
                                                               StringView utf8, int x, int top, const TextPaint &paint,
                                                               uint32_t maxWidth, Simd::Kernel kernel) {
                TextDraw::Scratch own;
                return drawLine(target, font, utf8, x, top, paint, maxWidth, kernel, own);
        }

} // namespace

Result<uint32_t> TextDraw::line(const TextTarget &target, const Font &font, StringView utf8, int x, int top,
                                const TextPaint &paint, uint32_t maxWidth, Simd::Kernel kernel, Scratch *scratch) {
        if (!font.isValid() || target.pixels == nullptr) return makeError<uint32_t>(Error::Invalid);
        if (scratch == nullptr) return drawOnStack(target, font, utf8, x, top, paint, maxWidth, kernel);
        return drawLine(target, font, utf8, x, top, paint, maxWidth, kernel, *scratch);
}

namespace {

        Result<uint32_t> drawLine(const TextTarget &target, const Font &font, StringView utf8, int x, int top,
                                  const TextPaint &paint, uint32_t maxWidth, Simd::Kernel kernel,
                                  TextDraw::Scratch &work) {
                const bool sharp = !paint.smooth || target.format == TextBlend::Format::Index8;
                Drawing    d(target, font, paint, kernel);
                d.batch = work.batch;
                d.pieces = work.pieces;
                d.band = work.band;
                d.depth = sharp ? FontFace::Depth::Mono : FontFace::Depth::Gray4;
                d.x = x;
                d.baseline = top + font.metrics().ascent;
                d.left = target.clipLeft < 0 ? 0 : target.clipLeft;
                d.top = target.clipTop < 0 ? 0 : target.clipTop;
                d.right = target.clipRight > target.width ? target.width : target.clipRight;
                d.bottom = target.clipBottom > target.height ? target.height : target.clipBottom;
                d.limit = maxWidth == 0 ? INT64_MAX : int64_t(maxWidth) * 64;
                if (paint.opaque) TextBlend::makeRamp(target.format, paint.color, paint.background, d.ramp);
                // On a box the ramp writes what a blend over the filled box would; where PIE blends, it is faster.
                d.blendOnBox = paint.opaque && Simd::usable(kernel) == Simd::Kernel::Pie &&
                               (target.format == TextBlend::Format::Bgr24 ||
                                target.format == TextBlend::Format::Rgb24 || target.format == TextBlend::Format::Gray8);
                (void)font.layout(utf8, visit, &d);
                if (d.error.isOk()) flush(d);
                if (d.error.isError()) return makeError<uint32_t>(d.error);
                return makeResult(uint32_t((d.end + 63) >> 6));
        }

} // namespace

BMDS_NAMESPACE_END
