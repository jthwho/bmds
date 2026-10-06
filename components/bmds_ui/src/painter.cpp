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
template <typename Draw> Error Painter::run(uint16_t x, uint16_t y, uint16_t maxWidth, uint16_t height, Draw draw) {
        const uint32_t sx = uint32_t(x) + _originX, sy = uint32_t(y) + _originY;
        if ((sx & AlignMask) != 0 || _tools.strip == nullptr || _tools.renderer == nullptr || height > _tools.height) {
                note(Error::Invalid);
                return _error;
        }
        if (sx >= _surface.width() || sy >= _surface.height()) return _error;
        if (maxWidth > _surface.width() - sx) maxWidth = static_cast<uint16_t>(_surface.width() - sx);
        const auto rows = static_cast<uint16_t>(height < _surface.height() - sy ? height : _surface.height() - sy);
        const Rect whole{static_cast<uint16_t>(sx), static_cast<uint16_t>(sy), maxWidth, rows};
        bool       visible = false;
        for (const Rect &piece : _clip) visible |= piece.intersects(whole);
        if (!visible) return _error;

        const size_t stride = size_t(_tools.halfWidth) * Surface::BytesPerPixel;
        uint16_t     done = 0;
        while (done < maxWidth) {
                const int  half = _tools.busyHalf == 0 ? 1 : 0; // not the one being copied
                uint8_t   *strip = _tools.strip + size_t(half) * stride * _tools.height;
                const auto room =
                        static_cast<uint16_t>(maxWidth - done < _tools.halfWidth ? maxWidth - done : _tools.halfWidth);
                bool           more = false;
                const uint16_t w = draw(strip, stride, room, more);
                if (w == 0) break;
                const Rect chunk{static_cast<uint16_t>(sx + done), static_cast<uint16_t>(sy), w, rows};
                bool       shown = false;
                for (const Rect &piece : _clip) shown |= piece.intersects(chunk);
                if (shown) {
                        ++_calls;
                        const Error err = _surface.startBlit(chunk, strip, stride, _clip, _timeoutMs);
                        note(err);
                        if (err.isOk()) _tools.busyHalf = static_cast<int8_t>(half);
                }
                done = static_cast<uint16_t>(done + w);
                if (!more) break;
        }
        return _error;
}

Error Painter::text(uint16_t x, uint16_t y, StringView utf8, const Font &font, uint32_t fg, uint32_t bg,
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

Error Painter::cells(uint16_t x, uint16_t y, Span<const TextCell> cells, const Font &font, uint16_t maxWidth) {
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
