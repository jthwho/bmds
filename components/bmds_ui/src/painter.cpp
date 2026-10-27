/**
 * @file      painter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Drawing on a surface through a window's visible pieces
 * (devplan/windows.md W8, §4a, §4d).
 */

#include <bmds/image.h>
#include <bmds/painter.h>
#include <bmds/simd.h>
#include <bmds/surface.h>
#include <bmds/textdraw.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t AlignMask = Surface::Align - 1u;
        constexpr size_t   MaxList = 32; // a scroll's working lists (on the window manager's stack; past them, redrawn)
        constexpr int32_t  MinPiece = 160; // columns a piece is given at least in the strip (if it needs them)
        constexpr int32_t  MinRoom = 32;   // and at most past the clip's span: room for a glyph, wide ones too

        // @p from minus every rectangle of @p minus into @p out (disjoint).
        // Returns false if more than @p MaxList rectangles were needed.
        bool subtractAll(Span<const Rect> from, Span<const Rect> minus, Rect (&out)[MaxList], size_t &count) {
                Rect   work[MaxList];
                Rect  *cur = out, *next = work;
                size_t n = 0;
                for (const Rect &r : from) {
                        if (r.isEmpty()) continue;
                        if (n == MaxList) return false;
                        cur[n++] = r;
                }
                for (const Rect &m : minus) {
                        size_t k = 0;
                        for (size_t i = 0; i < n; ++i) {
                                if (!cur[i].intersects(m)) {
                                        if (k == MaxList) return false;
                                        next[k++] = cur[i];
                                        continue;
                                }
                                Rect         parts[4];
                                const size_t p = subtract(cur[i], m, parts);
                                for (size_t j = 0; j < p; ++j) {
                                        if (k == MaxList) return false;
                                        next[k++] = parts[j];
                                }
                        }
                        Rect *t = cur;
                        cur = next;
                        next = t;
                        n = k;
                }
                if (cur != out)
                        for (size_t i = 0; i < n; ++i) out[i] = cur[i];
                count = n;
                return true;
        }

} // namespace

Painter::Painter(Painter &outer, const Rect &limit, Span<Rect> storage)
    : _surface(outer._surface), _tools(outer._tools), _timeoutMs(outer._timeoutMs), _originX(outer._originX),
      _originY(outer._originY), _outer(&outer) {
        // The limit on the surface, its sides on columns (inward).
        const uint32_t left = (uint32_t(limit.x) + _originX + AlignMask) & ~AlignMask;
        const uint32_t right = (uint32_t(limit.right()) + _originX) & ~AlignMask;
        const uint32_t bottom = uint32_t(limit.bottom()) + _originY;
        Rect           l;
        if (!limit.isEmpty() && right > left && right <= UINT16_MAX && bottom <= UINT16_MAX)
                l = Rect{static_cast<uint16_t>(left), static_cast<uint16_t>(limit.y + _originY),
                         static_cast<uint16_t>(right - left), limit.height};
        size_t     n = 0;
        const auto cut = [&](Span<const Rect> from) {
                const size_t start = n;
                for (const Rect &piece : from) {
                        const Rect r = piece.intersected(l);
                        if (r.isEmpty()) continue;
                        if (n == storage.size()) {
                                note(Error::Full);
                                break;
                        }
                        storage[n++] = r;
                }
                return Span<const Rect>(storage.data() + start, n - start);
        };
        _clip = cut(outer._clip);
        _damage = cut(outer._damage);
}

Painter::~Painter() {
        if (_outer == nullptr) return;
        _outer->_calls += _calls;
        if (_error.isError()) _outer->note(_error);
        if (_deferred) _outer->defer();
}

Rect Painter::bounds(Span<const Rect> rects) {
        Rect b;
        for (const Rect &r : rects) b = b.united(r);
        return b;
}

Rect Painter::local(const Rect &r) const {
        if (r.isEmpty()) return r;
        // A window's clip and damage are never left of or above its origin.
        return Rect{static_cast<uint16_t>(r.x > _originX ? r.x - _originX : 0),
                    static_cast<uint16_t>(r.y > _originY ? r.y - _originY : 0), r.width, r.height};
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
                const uint32_t right = uint32_t(rect.right()) + _originX;
                const uint32_t bottom = uint32_t(rect.bottom()) + _originY;
                if (right > UINT16_MAX || bottom > UINT16_MAX) continue; // past any surface
                const Rect r{static_cast<uint16_t>(rect.x + _originX), static_cast<uint16_t>(rect.y + _originY),
                             rect.width, rect.height};
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

// Text in chunks of a strip half: @p draw expands the next chunk into a half
// (while the 2D-DMA copies the other into the surface), then it is copied
// piece by piece of the clip, as one job. Each of the @p height rows made shows @p repeat times
// down (the 2D-DMA reading it again).
template <typename Draw>
Error Painter::run(int x, int y, uint16_t maxWidth, uint16_t height, Draw draw, bool rgb565, uint16_t repeat) {
        const int32_t sx = int32_t(x) + _originX, sy = int32_t(y) + _originY;
        if ((uint32_t(sx) & AlignMask) != 0 || _tools.strip == nullptr || _tools.renderer == nullptr ||
            height > _tools.height || repeat == 0) {
                note(Error::Invalid);
                return _error;
        }
        const int32_t surfaceW = _surface.width(), surfaceH = _surface.height();
        if (sx >= surfaceW || sy >= surfaceH) return _error;
        // What is on the surface: the columns and rows left of and above it cut off.
        int32_t width = maxWidth;
        if (sx + width > surfaceW) width = surfaceW - sx;
        const int32_t tall = int32_t(height) * repeat;
        const int32_t rows = sy + tall > surfaceH ? surfaceH - sy : tall;
        const int32_t skipY = sy < 0 ? -sy : 0;
        if (sx + width <= 0 || skipY >= rows) return _error;
        const int32_t left = sx < 0 ? 0 : sx;
        const Rect    whole{static_cast<uint16_t>(left), static_cast<uint16_t>(sy + skipY),
                         static_cast<uint16_t>(sx + width - left), static_cast<uint16_t>(rows - skipY)};
        bool          visible = false;
        for (const Rect &piece : _clip) visible |= piece.intersects(whole);
        if (!visible) return _error;
        // The clip's next span of these rows from column @p c: [first, end), first INT32_MAX if none.
        // Pieces are made only for spans (a thin strip of a long label draws a glyph or two).
        const auto span = [&](int32_t c, int32_t &first, int32_t &end) {
                first = INT32_MAX;
                end = 0;
                for (const Rect &piece : _clip) {
                        const Rect cut = piece.intersected(whole);
                        if (cut.isEmpty() || int32_t(cut.right()) <= c) continue;
                        const int32_t from = int32_t(cut.x) > c ? int32_t(cut.x) : c;
                        if (from < first || (from == first && int32_t(cut.right()) > end)) {
                                first = from;
                                end = cut.right();
                        }
                }
                return;
        };

        // A half's size is the BGR one's; RGB565 rows are packed closer in it.
        const Surface::Source source = rgb565 ? Surface::Source::Rgb565 : Surface::Source::Bgr24;
        const size_t          pixel = rgb565 ? 2 : Surface::BytesPerPixel;
        const size_t          stride = size_t(_tools.halfWidth) * pixel;
        const size_t          halfBytes = size_t(_tools.halfWidth) * Surface::BytesPerPixel * _tools.height;
        int32_t               done = 0;
        while (done < width) {
                const int32_t col = sx + done;
                int32_t       first = 0, end = 0;
                span(col < 0 ? 0 : col, first, end);
                if (first == INT32_MAX) break; // nothing more of it shows
                if (first >= col + int32_t(Surface::Align)) {
                        // Up to the span: passed over, measured (no strip, nothing drawn).
                        const int32_t  gap = (first - col) & ~int32_t(AlignMask);
                        bool           more = false;
                        const uint16_t w = draw(nullptr, 0, static_cast<uint16_t>(gap < 0xfff0 ? gap : 0xfff0), more);
                        if (w != 0) {
                                done += w;
                                if (!more) break;
                                continue;
                        }
                        // Not even a glyph's room: drawn from here.
                }
                // A piece no wider than the span (but a glyph or two at least).
                const int32_t desired = width - done < (end - col > MinRoom ? end - col : MinRoom)
                                                ? width - done
                                                : (end - col > MinRoom ? end - col : MinRoom);
                // The next free columns: past what earlier pieces use, which the 2D-DMA may not have
                // read yet. Too few left in a half for the piece (a text's width isn't known before:
                // at least MinPiece), the next half; both used, the surface waited for, from the start.
                const int32_t wanted = desired < MinPiece ? desired : MinPiece;
                if (_tools.stripHalf < 2 && int32_t(_tools.halfWidth) - _tools.stripColumn < wanted) {
                        ++_tools.stripHalf;
                        _tools.stripColumn = 0;
                }
                if (_tools.stripHalf >= 2) {
                        note(_surface.wait(_timeoutMs));
                        _tools.stripHalf = 0;
                        _tools.stripColumn = 0;
                }
                uint8_t *strip =
                        _tools.strip + size_t(_tools.stripHalf) * halfBytes + size_t(_tools.stripColumn) * pixel;
                const int32_t  free = int32_t(_tools.halfWidth) - _tools.stripColumn;
                const auto     room = static_cast<uint16_t>(desired < free ? desired : free);
                bool           more = false;
                const uint16_t w = draw(strip, stride, room, more);
                if (w == 0) break;
                _tools.stripColumn = static_cast<uint16_t>(_tools.stripColumn + ((w + AlignMask) & ~AlignMask));
                // The chunk's part on the surface (columns cut on the left come in fours, as x).
                const int32_t at = sx + done;
                const int32_t cut = at < 0 ? -at : 0;
                if (cut < w) {
                        const Rect chunk{static_cast<uint16_t>(at + cut), static_cast<uint16_t>(sy + skipY),
                                         static_cast<uint16_t>(w - cut), static_cast<uint16_t>(rows - skipY)};
                        bool       shown = false;
                        for (const Rect &piece : _clip) shown |= piece.intersects(chunk);
                        if (shown) {
                                ++_calls;
                                const uint8_t *from = strip + size_t(skipY / repeat) * stride + size_t(cut) * pixel;
                                const Error    err = _surface.startBlit(chunk, from, stride, _clip, source, repeat,
                                                                        uint16_t(skipY % repeat), _timeoutMs);
                                note(err);
                        }
                }
                done += w;
                if (!more) break;
        }
        return _error;
}

Error Painter::text(int x, int y, StringView utf8, const BitmapFont &font, uint32_t fg, uint32_t bg,
                    uint16_t maxWidth) {
        size_t at = 0;
        return run(x, y, maxWidth, font.height(), [&](uint8_t *strip, size_t stride, uint16_t room, bool &more) {
                size_t         used = 0;
                const uint16_t w = _tools.renderer->drawText(utf8.mid(at), font, fg, bg, strip, stride, room, used);
                at += used;
                more = at < utf8.size();
                return w;
        });
}

Error Painter::text(int x, int y, StringView utf8, const Font &font, uint32_t fg, uint32_t bg, uint16_t maxWidth) {
        const TextRun one{uint32_t(utf8.size()), fg, bg};
        return text(x, y, utf8, font, Span<const TextRun>(&one, 1), maxWidth);
}

Error Painter::text(int x, int y, StringView utf8, const Font &font, Span<const TextRun> runs, uint16_t maxWidth) {
        if (!font.isValid() || runs.isEmpty() || runs.size() > MaxRuns) {
                note(Error::Invalid);
                return _error;
        }
        // Off a column, the box starts on the one left of it, the first run's bg in the pixels between.
        const auto        lead = static_cast<uint16_t>(uint32_t(int32_t(x) + _originX) & AlignMask);
        const BitmapFont *bitmap = font.bitmap();
        if (bitmap != nullptr && font.scale() == 1 && !font.isMadeUpBold() && lead == 0) {
                // Cells of 8: each run starts on a column of its own.
                uint32_t at = 0, from = 0;
                for (const TextRun &r : runs) {
                        const uint32_t to = r.end < utf8.size() ? r.end : uint32_t(utf8.size());
                        if (to <= from) continue;
                        const StringView piece = utf8.mid(from, to - from);
                        if (at >= maxWidth) break;
                        (void)text(int(x + at), y, piece, *bitmap, r.fg, r.bg, uint16_t(maxWidth - at));
                        at += bitmap->textWidth(piece);
                        from = to;
                }
                return _error;
        }
        const StringView line = utf8.left(font.fitting(utf8, maxWidth));
        const uint32_t   advance = font.textWidth(line);
        // The box ends on a 4-pixel column (the strip's copies are in fours), the last run's bg past
        // the advance.
        const auto width =
                static_cast<uint16_t>((lead + (advance < maxWidth ? advance : maxWidth) + AlignMask) & ~AlignMask);
        const int  height = font.metrics().lineHeight;
        if (width == 0 || height <= 0) return _error;
        // Each run's columns: from the last one's end to its text's advance (the last to the box's end).
        uint16_t ends[MaxRuns];
        for (size_t i = 0; i < runs.size(); ++i) {
                const uint32_t to = runs[i].end < line.size() ? runs[i].end : uint32_t(line.size());
                const uint32_t e = i + 1 == runs.size() ? width : lead + font.textWidth(line.left(to));
                ends[i] = static_cast<uint16_t>(e < width ? e : width);
        }
        // Each run's part of a piece filled with its bg, then the line blended over it in its fg from
        // minus the piece's column, clipped to the part: what TextDraw's Opaque writes on PIE.
        TextPaint paint;
        paint.smooth = _tools.smooth;
        const Simd::Kernel kernel = Simd::best();
        for (int band = 0; band < height && _tools.height != 0; band += _tools.height) {
                const auto rows = static_cast<uint16_t>(height - band < _tools.height ? height - band : _tools.height);
                uint16_t   column = 0;
                (void)run(
                        x - lead, y + band, width, rows, [&](uint8_t *strip, size_t stride, uint16_t room, bool &more) {
                                const auto left = static_cast<uint16_t>(width - column);
                                const auto w = static_cast<uint16_t>(room < left ? room & ~AlignMask : left);
                                if (w == 0) return w;
                                uint16_t start = 0;
                                for (size_t i = 0; strip != nullptr && i < runs.size(); start = ends[i], ++i) {
                                        // The run's columns in the piece.
                                        const int a = (start > column ? start : column) - column;
                                        const int b = (ends[i] < column + w ? ends[i] : column + w) - column;
                                        if (a >= b) continue;
                                        for (uint16_t r = 0; r < rows; ++r)
                                                TextBlend::fill(strip + size_t(r) * stride + size_t(a) * 3,
                                                                TextBlend::Format::Bgr24, unsigned(b - a), runs[i].bg);
                                        TextTarget t;
                                        t.pixels = strip;
                                        t.format = TextBlend::Format::Bgr24;
                                        t.width = w;
                                        t.height = rows;
                                        t.stride = stride;
                                        t.clipLeft = a;
                                        t.clipRight = b;
                                        paint.color = runs[i].fg;
                                        const Result<uint32_t> drawn =
                                                TextDraw::line(t, font, line, lead - int(column), -band, paint, 0,
                                                               kernel, _tools.textScratch);
                                        if (drawn.isError()) note(drawn.error());
                                }
                                column = static_cast<uint16_t>(column + w);
                                more = column < width;
                                return w;
                        });
        }
        return _error;
}

Error Painter::rows(int x, int y, uint16_t width, uint16_t height, uint16_t repeat, bool rgb565, RowMaker make,
                    void *arg) {
        if (make == nullptr || (width & AlignMask) != 0) {
                note(Error::Invalid);
                return _error;
        }
        uint16_t column = 0;
        return run(
                x, y, width, height,
                [&](uint8_t *strip, size_t stride, uint16_t room, bool &more) {
                        const uint16_t w = strip != nullptr        ? make(arg, strip, stride, column, room)
                                           : room < width - column ? static_cast<uint16_t>(room & ~AlignMask)
                                                                   : static_cast<uint16_t>(width - column);
                        column = uint16_t(column + w);
                        more = column < width;
                        return w;
                },
                rgb565, repeat);
}

Error Painter::tiled(const Rect &rect, const uint8_t *bgr, size_t stride, uint16_t width, uint16_t height,
                     uint16_t repeat, int tileX, int tileY) {
        const int32_t sx = int32_t(rect.x) + _originX, sy = int32_t(rect.y) + _originY;
        if (bgr == nullptr || width == 0 || height == 0 || repeat == 0 || (width & AlignMask) != 0 ||
            (uint32_t(sx) & AlignMask) != 0 || (rect.width & AlignMask) != 0 ||
            (uint32_t(int32_t(rect.x) - tileX) & AlignMask) != 0) {
                note(Error::Invalid);
                return _error;
        }
        // What is on the surface (x stays on a column: the surface's width is one).
        int32_t right = sx + rect.width, bottom = sy + rect.height;
        if (right > int32_t(_surface.width())) right = _surface.width();
        if (bottom > int32_t(_surface.height())) bottom = _surface.height();
        const int32_t left = sx < 0 ? 0 : sx, top = sy < 0 ? 0 : sy;
        if (rect.isEmpty() || right <= left || bottom <= top) return _error;
        const Rect to{uint16_t(left), uint16_t(top), uint16_t(right - left), uint16_t(bottom - top)};
        bool       shown = false;
        for (const Rect &piece : _clip) shown |= piece.intersects(to);
        if (!shown) return _error;
        // The tile's column and row (in repeats) at the top-left corner.
        const auto wrap = [](int64_t v, uint32_t period) {
                const int64_t m = v % int64_t(period);
                return uint32_t(m < 0 ? m + period : m);
        };
        const uint32_t      x = wrap(int64_t(left) - _originX - tileX, width);
        const uint32_t      down = wrap(int64_t(top) - _originY - tileY, uint32_t(height) * repeat);
        const Surface::Tile tile{bgr, stride, width, height, repeat};
        ++_calls;
        Error err = _surface.startTile(to, tile, uint16_t(x), down, _clip, _timeoutMs);
        if (err.isOk()) err = _surface.wait(_timeoutMs); // the caller's tile may change after
        if (err.isOk()) {                                // the strip's copies are done too
                _tools.stripHalf = 0;
                _tools.stripColumn = 0;
        }
        note(err);
        return _error;
}

Error Painter::cover(const Rect &rect) {
        const int32_t sx = int32_t(rect.x) + _originX, sy = int32_t(rect.y) + _originY;
        const Rect    on{uint16_t(sx < 0 ? 0 : sx), uint16_t(sy < 0 ? 0 : sy), rect.width, rect.height};
        Rect          pieces[32];
        size_t        n = 0;
        for (const Rect &piece : _clip) {
                const Rect cut = on.intersected(piece);
                if (cut.isEmpty()) continue;
                if (n == 32) break; // the rest copied forward as they are drawn
                pieces[n++] = cut;
        }
        if (n > 0) note(_surface.willDraw(Span<const Rect>(pieces, n), _timeoutMs));
        return _error;
}

Error Painter::pixels(int x, int y, const uint8_t *bgr, size_t stride, uint16_t width, uint16_t height) {
        return copyPixels(x, y, bgr, stride, width, height, false);
}

Error Painter::pixels565(int x, int y, const uint8_t *rgb565, size_t stride, uint16_t width, uint16_t height) {
        return copyPixels(x, y, rgb565, stride, width, height, true);
}

Error Painter::copyPixels(int x, int y, const uint8_t *data, size_t stride, uint16_t width, uint16_t height,
                          bool rgb565) {
        const Surface::Source source = rgb565 ? Surface::Source::Rgb565 : Surface::Source::Bgr24;
        const size_t          pixel = rgb565 ? 2 : Surface::BytesPerPixel;
        const int32_t         sx = int32_t(x) + _originX, sy = int32_t(y) + _originY;
        if (data == nullptr || stride % pixel != 0 || stride < size_t(width) * pixel) {
                note(Error::Invalid);
                return _error;
        }
        // What of it is on the surface.
        const int32_t left = sx < 0 ? 0 : sx, top = sy < 0 ? 0 : sy;
        int32_t       right = sx + int32_t(width), bottom = sy + int32_t(height);
        if (right > int32_t(_surface.width())) right = int32_t(_surface.width());
        if (bottom > int32_t(_surface.height())) bottom = _surface.height();
        if (right <= left || bottom <= top) return _error;
        // Its whole 4-pixel columns copied by the 2D-DMA; the few columns at either edge patched.
        const int32_t innerLeft = int32_t((uint32_t(left) + AlignMask) & ~AlignMask);
        const int32_t innerRight = int32_t(uint32_t(right) & ~AlignMask);
        if (innerRight > innerLeft) {
                const Rect to{uint16_t(innerLeft), uint16_t(top), uint16_t(innerRight - innerLeft),
                              uint16_t(bottom - top)};
                bool       shown = false;
                for (const Rect &piece : _clip) shown |= piece.intersects(to);
                if (shown) {
                        const uint8_t *from = data + size_t(top - sy) * stride + size_t(innerLeft - sx) * pixel;
                        ++_calls;
                        Error err = _surface.startBlit(to, from, stride, _clip, source, _timeoutMs);
                        if (err.isOk()) err = _surface.wait(_timeoutMs); // the caller's picture may change after
                        if (err.isOk()) {                                // the strip's copies are done too
                                _tools.stripHalf = 0;
                                _tools.stripColumn = 0;
                        }
                        note(err);
                }
        }
        if (innerRight <= innerLeft) {
                patchPixels(Rect{uint16_t(left), uint16_t(top), uint16_t(right - left), uint16_t(bottom - top)}, sx, sy,
                            data, stride, rgb565);
        } else {
                if (left < innerLeft)
                        patchPixels(
                                Rect{uint16_t(left), uint16_t(top), uint16_t(innerLeft - left), uint16_t(bottom - top)},
                                sx, sy, data, stride, rgb565);
                if (innerRight < right)
                        patchPixels(Rect{uint16_t(innerRight), uint16_t(top), uint16_t(right - innerRight),
                                         uint16_t(bottom - top)},
                                    sx, sy, data, stride, rgb565);
        }
        return _error;
}

namespace {

        // A picture's part written into a patched rectangle (Surface::patch()'s patcher).
        struct PatchSource {
                        const uint8_t *from;   // the rectangle's first pixel in the picture
                        size_t         stride; // the picture's bytes a row
                        uint16_t       width;
                        uint16_t       height;
                        bool           rgb565;
        };

        void copyIntoPatch(void *arg, uint8_t *pixels, size_t stride) {
                const auto &p = *static_cast<const PatchSource *>(arg);
                for (uint16_t row = 0; row < p.height; ++row) {
                        const uint8_t *s = p.from + size_t(row) * p.stride;
                        uint8_t       *d = pixels + size_t(row) * stride;
                        for (uint16_t i = 0; i < p.width; ++i, d += 3) {
                                if (p.rgb565) {
                                        const uint32_t v = uint32_t(s[0]) | uint32_t(s[1]) << 8;
                                        const uint32_t r = v >> 11, g = (v >> 5) & 0x3f, b = v & 0x1f;
                                        d[0] = uint8_t(b << 3 | b >> 2);
                                        d[1] = uint8_t(g << 2 | g >> 4);
                                        d[2] = uint8_t(r << 3 | r >> 2);
                                        s += 2;
                                } else {
                                        d[0] = s[0];
                                        d[1] = s[1];
                                        d[2] = s[2];
                                        s += 3;
                                }
                        }
                }
                return;
        }

} // namespace

void Painter::patchPixels(const Rect &edge, int32_t sx, int32_t sy, const uint8_t *data, size_t stride, bool rgb565) {
        const size_t pixel = rgb565 ? 2 : Surface::BytesPerPixel;
        for (const Rect &piece : _clip) {
                const Rect r = piece.intersected(edge);
                if (r.isEmpty()) continue;
                // A patch is at most a surface row's bytes: its 4-pixel groups, a row each.
                const uint32_t span =
                        ((uint32_t(r.x) + r.width + AlignMask) & ~AlignMask) - (uint32_t(r.x) & ~AlignMask);
                const uint32_t rows = uint32_t(_surface.stride() / (span * Surface::BytesPerPixel));
                for (uint16_t y = r.y; y < r.y + r.height && rows != 0;) {
                        const uint16_t n = uint16_t(r.y + r.height - y < int(rows) ? r.y + r.height - y : rows);
                        PatchSource    p{data + size_t(y - sy) * stride + size_t(r.x - sx) * pixel, stride, r.width, n,
                                      rgb565};
                        ++_calls;
                        note(_surface.patch(Rect{r.x, y, r.width, n}, copyIntoPatch, &p, _timeoutMs));
                        y = uint16_t(y + n);
                }
        }
        return;
}

Error Painter::image(int x, int y, const Icon &icon, uint32_t bg) {
        if (!icon.isValid()) return _error;
        const uint16_t width = static_cast<uint16_t>(icon.width & ~3u); // whole groups of 4
        for (uint16_t top = 0; top < icon.height && width != 0; top = static_cast<uint16_t>(top + _tools.height)) {
                const uint16_t rows =
                        static_cast<uint16_t>(icon.height - top < _tools.height ? icon.height - top : _tools.height);
                uint16_t column = 0; // the icon's, a piece's first
                (void)run(x, y + top, width, rows, [&](uint8_t *strip, size_t stride, uint16_t room, bool &more) {
                        const uint16_t w =
                                room < width - column ? static_cast<uint16_t>(room & ~3u) : uint16_t(width - column);
                        for (uint16_t r = 0; strip != nullptr && r < rows; ++r) // blendOver(), PIE's 16 at a time
                                Pixels::fromArgb(icon.pixels + size_t(top + r) * icon.width + column, w,
                                                 strip + size_t(r) * stride, PixelFormat::Bgr24, bg);
                        column = static_cast<uint16_t>(column + w);
                        more = column < width;
                        return w;
                });
        }
        return _error;
}

Error Painter::cells(int x, int y, Span<const TextCell> cells, const BitmapFont &font, uint16_t maxWidth) {
        size_t at = 0;
        return run(x, y, maxWidth, font.height(), [&](uint8_t *strip, size_t stride, uint16_t room, bool &more) {
                size_t         used = 0;
                const uint16_t w = _tools.renderer->drawCells(
                        Span<const TextCell>(cells.data() + at, cells.size() - at), font, strip, stride, room, used);
                at += used;
                more = at < cells.size();
                return w;
        });
}

Region Painter::scroll(const Rect &area, int dy) {
        Region exposed;
        if (area.isEmpty()) return exposed;
        const Rect a{static_cast<uint16_t>(area.x + _originX), static_cast<uint16_t>(area.y + _originY), area.width,
                     area.height};
        // Where pixels go: what shows of the area. Where they may come from:
        // that, less the damage.
        Rect   dests[MaxList], valid[MaxList];
        size_t nd = 0, nv = 0;
        for (const Rect &piece : _clip) {
                const Rect d = a.intersected(piece);
                if (d.isEmpty()) continue;
                if (nd == MaxList) {
                        exposed.add(local(d));
                        continue;
                }
                dests[nd++] = d;
        }
        if (dy == 0 || !subtractAll(Span<const Rect>(dests, nd), _damage, valid, nv)) nv = 0;

        // Each destination's parts whose source is valid.
        Surface::Copy copies[MaxList];
        size_t        nc = 0;
        for (size_t i = 0; i < nd; ++i) {
                for (size_t j = 0; j < nv && nc < MaxList; ++j) {
                        const int32_t  top = int32_t(valid[j].y) + dy, bottom = top + valid[j].height;
                        const int32_t  t = top > dests[i].y ? top : int32_t(dests[i].y);
                        const int32_t  b = bottom < int32_t(dests[i].bottom()) ? bottom : int32_t(dests[i].bottom());
                        const uint32_t l = valid[j].x > dests[i].x ? valid[j].x : dests[i].x;
                        const uint32_t r = valid[j].right() < dests[i].right() ? valid[j].right() : dests[i].right();
                        if (t >= b || l >= r) continue;
                        copies[nc++] = Surface::Copy{Rect{static_cast<uint16_t>(l), static_cast<uint16_t>(t - dy),
                                                          static_cast<uint16_t>(r - l), static_cast<uint16_t>(b - t)},
                                                     static_cast<uint16_t>(l), static_cast<uint16_t>(t)};
                }
        }

        // The rest of the destinations is to redraw.
        Rect moved[MaxList];
        for (size_t i = 0; i < nc; ++i)
                moved[i] = Rect{copies[i].toX, copies[i].toY, copies[i].from.width, copies[i].from.height};
        for (size_t i = 0; i < nd; ++i) {
                Rect   rest[MaxList];
                size_t nr = 0;
                if (!subtractAll(Span<const Rect>(&dests[i], 1), Span<const Rect>(moved, nc), rest, nr)) {
                        exposed.add(local(dests[i]));
                        continue;
                }
                for (size_t k = 0; k < nr; ++k) exposed.add(local(rest[k]));
        }
        if (nc == 0) return exposed;

        // In an order that never reads a row already written: moving up, the
        // highest first; moving down, the lowest first, in bands no taller
        // than the move.
        for (size_t i = 1; i < nc; ++i) {
                const Surface::Copy c = copies[i];
                size_t              k = i;
                for (; k > 0 && (dy < 0 ? copies[k - 1].toY > c.toY : copies[k - 1].toY < c.toY); --k)
                        copies[k] = copies[k - 1];
                copies[k] = c;
        }
        if (dy < 0) {
                ++_calls;
                note(_surface.copy(Span<const Surface::Copy>(copies, nc), _timeoutMs));
                return exposed;
        }
        Surface::Copy bands[MaxList];
        size_t        nb = 0;
        const auto    band = static_cast<uint16_t>(dy);
        for (size_t i = 0; i < nc; ++i) {
                const Surface::Copy &c = copies[i];
                for (uint32_t done = 0; done < c.from.height;) {
                        const auto h = static_cast<uint16_t>(c.from.height - done < band ? c.from.height - done : band);
                        done += h;
                        const auto off = static_cast<uint16_t>(c.from.height - done); // from the bottom up
                        if (nb == MaxList) {
                                ++_calls;
                                note(_surface.copy(Span<const Surface::Copy>(bands, nb), _timeoutMs));
                                nb = 0;
                        }
                        bands[nb++] =
                                Surface::Copy{Rect{c.from.x, static_cast<uint16_t>(c.from.y + off), c.from.width, h},
                                              c.toX, static_cast<uint16_t>(c.toY + off)};
                }
        }
        ++_calls;
        note(_surface.copy(Span<const Surface::Copy>(bands, nb), _timeoutMs));
        return exposed;
}

BMDS_NAMESPACE_END
