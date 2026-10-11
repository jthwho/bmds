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
#include <bmds/surface.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t AlignMask = Surface::Align - 1u;
        constexpr size_t   MaxList = 32; // a scroll's working lists (on the window manager's stack; past them, redrawn)

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
// piece by piece of the clip, as one job.
template <typename Draw> Error Painter::run(int x, int y, uint16_t maxWidth, uint16_t height, Draw draw) {
        const int32_t sx = int32_t(x) + _originX, sy = int32_t(y) + _originY;
        if ((uint32_t(sx) & AlignMask) != 0 || _tools.strip == nullptr || _tools.renderer == nullptr ||
            height > _tools.height) {
                note(Error::Invalid);
                return _error;
        }
        const int32_t surfaceW = _surface.width(), surfaceH = _surface.height();
        if (sx >= surfaceW || sy >= surfaceH) return _error;
        // What is on the surface: the columns and rows left of and above it cut off.
        int32_t width = maxWidth;
        if (sx + width > surfaceW) width = surfaceW - sx;
        const int32_t rows = sy + int32_t(height) > surfaceH ? surfaceH - sy : int32_t(height);
        const int32_t skipY = sy < 0 ? -sy : 0;
        if (sx + width <= 0 || skipY >= rows) return _error;
        const int32_t left = sx < 0 ? 0 : sx;
        const Rect    whole{static_cast<uint16_t>(left), static_cast<uint16_t>(sy + skipY),
                         static_cast<uint16_t>(sx + width - left), static_cast<uint16_t>(rows - skipY)};
        bool          visible = false;
        for (const Rect &piece : _clip) visible |= piece.intersects(whole);
        if (!visible) return _error;

        const size_t stride = size_t(_tools.halfWidth) * Surface::BytesPerPixel;
        int32_t      done = 0;
        while (done < width) {
                const int  half = _tools.busyHalf == 0 ? 1 : 0; // not the one being copied
                uint8_t   *strip = _tools.strip + size_t(half) * stride * _tools.height;
                const auto room =
                        static_cast<uint16_t>(width - done < _tools.halfWidth ? width - done : _tools.halfWidth);
                bool           more = false;
                const uint16_t w = draw(strip, stride, room, more);
                if (w == 0) break;
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
                                const uint8_t *from =
                                        strip + size_t(skipY) * stride + size_t(cut) * Surface::BytesPerPixel;
                                const Error err = _surface.startBlit(chunk, from, stride, _clip, _timeoutMs);
                                note(err);
                                if (err.isOk()) _tools.busyHalf = static_cast<int8_t>(half);
                        }
                }
                done += w;
                if (!more) break;
        }
        return _error;
}

Error Painter::text(int x, int y, StringView utf8, const Font &font, uint32_t fg, uint32_t bg, uint16_t maxWidth) {
        size_t at = 0;
        return run(x, y, maxWidth, font.height(), [&](uint8_t *strip, size_t stride, uint16_t room, bool &more) {
                size_t         used = 0;
                const uint16_t w = _tools.renderer->drawText(utf8.mid(at), font, fg, bg, strip, stride, room, used);
                at += used;
                more = at < utf8.size();
                return w;
        });
}

Error Painter::pixels(int x, int y, const uint8_t *bgr, size_t stride, uint16_t width, uint16_t height) {
        const int32_t sx = int32_t(x) + _originX, sy = int32_t(y) + _originY;
        if ((uint32_t(sx) & AlignMask) != 0 || (width & AlignMask) != 0 || bgr == nullptr ||
            stride % Surface::BytesPerPixel != 0 || stride < size_t(width) * Surface::BytesPerPixel) {
                note(Error::Invalid);
                return _error;
        }
        // What of it is on the surface (cut in fours across: x, the width and the surface's are).
        const int32_t left = sx < 0 ? 0 : sx, top = sy < 0 ? 0 : sy;
        int32_t       right = sx + int32_t(width), bottom = sy + int32_t(height);
        if (right > int32_t(_surface.width())) right = int32_t(_surface.width()) & ~int32_t(AlignMask);
        if (bottom > int32_t(_surface.height())) bottom = _surface.height();
        if (right <= left || bottom <= top) return _error;
        const Rect to{uint16_t(left), uint16_t(top), uint16_t(right - left), uint16_t(bottom - top)};
        bool       shown = false;
        for (const Rect &piece : _clip) shown |= piece.intersects(to);
        if (!shown) return _error;
        const uint8_t *from = bgr + size_t(top - sy) * stride + size_t(left - sx) * Surface::BytesPerPixel;
        ++_calls;
        Error err = _surface.startBlit(to, from, stride, _clip, _timeoutMs);
        if (err.isOk()) err = _surface.wait(_timeoutMs); // the caller's picture is free to change after
        _tools.busyHalf = -1;                            // the strip's last copy is done too
        note(err);
        return _error;
}

Error Painter::image(int x, int y, const Icon &icon, uint32_t bg) {
        if (!icon.isValid()) return _error;
        const uint16_t width = static_cast<uint16_t>(icon.width & ~3u); // whole groups of 4
        for (uint16_t top = 0; top < icon.height && width != 0; top = static_cast<uint16_t>(top + _tools.height)) {
                const uint16_t rows =
                        static_cast<uint16_t>(icon.height - top < _tools.height ? icon.height - top : _tools.height);
                (void)run(x, y + top, width, rows, [&](uint8_t *strip, size_t stride, uint16_t room, bool &more) {
                        const uint16_t w = room < width ? static_cast<uint16_t>(room & ~3u) : width;
                        for (uint16_t r = 0; r < rows; ++r) // blendOver() a pixel, PIE's 16 at a time
                                Pixels::fromArgb(icon.pixels + size_t(top + r) * icon.width, w,
                                                 strip + size_t(r) * stride, PixelFormat::Bgr24, bg);
                        more = false;
                        return w;
                });
        }
        return _error;
}

Error Painter::cells(int x, int y, Span<const TextCell> cells, const Font &font, uint16_t maxWidth) {
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
