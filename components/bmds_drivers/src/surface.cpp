/**
 * @file      surface.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An RGB888 picture in PSRAM, drawn by 2D-DMA, swapped at a frame's start
 * when double-buffered (devplan/display.md §5c).
 */

#include <esp_cache.h>
#include <cstring>
#include <esp_timer.h>
#include <bmds/deadline.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/surface.h>

BMDS_NAMESPACE_BEGIN


namespace {

        constexpr size_t BufferAlign = 128;          // the L2 cache line; the 2D-DMA's PSRAM alignment
        constexpr size_t LineAlign = 64;             // the L1 cache line (internal RAM)
        constexpr size_t WriteBackChunk = 64 * 1024; // a cache operation runs with interrupts off
        constexpr size_t MaxPieces = 32;             // a job's copies
        constexpr size_t MaxPrepareRects = 32;       // rectangles a drawing tells prepareFor()
        constexpr size_t MaxFillRows = 256;          // rows fills send in one job (24 B each, internal)
        constexpr size_t MaxFillLineBytes = 1536;    // a batched fill's lines of its colour, at most
        constexpr size_t MaxFillLineRows = 32;       // and their rows (a descriptor sends them all)
        constexpr size_t MaxWidenRows = 128;         // repeated RGB565 rows in one job (the same)

        // What's left of @p deadline, as a timeout (at least 1 ms).
        unsigned int remainingMs(const Deadline &deadline) {
                const int64_t us = deadline.remainingUs();
                if (us == Deadline::Forever) return WaitForever;
                return static_cast<unsigned int>(us / 1000 + 1);
        }

} // namespace

Error Surface::start(uint16_t width, uint16_t height, Buffering buffering) {
        if (isStarted()) return Error::InvalidState;
        if (width == 0 || height == 0 || width % Align != 0) return Error::Invalid;
        _width = width;
        _height = height;
        _buffering = buffering;
        const size_t bytes = stride() * height;
        const int    count = buffering == Buffering::Double ? 2 : 1;
        for (int i = 0; i < count; ++i) {
                _buffers[i] = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, bytes, BufferAlign));
                if (_buffers[i] == nullptr) {
                        stop();
                        return Error::NoMem;
                }
        }
        if (count == 2) {
                const uint16_t columns = static_cast<uint16_t>((width + TileWidth - 1) / TileWidth);
                _tileWords = static_cast<uint16_t>((columns + 31) / 32);
                _tileRows = static_cast<uint16_t>((height + TileHeight - 1) / TileHeight);
                const size_t words = size_t(_tileWords) * _tileRows;
                _damage = static_cast<uint32_t *>(Memory::alloc(MemSpace::Internal, words * 3 * sizeof(uint32_t)));
                if (_damage == nullptr) {
                        stop();
                        return Error::NoMem;
                }
                _forward = _damage + words;
                _select = _forward + words;
                std::memset(_damage, 0, words * 3 * sizeof(uint32_t));
        }
        const size_t lineBytes = (stride() + LineAlign - 1) / LineAlign * LineAlign;
        _line = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, lineBytes, LineAlign));
        if (_line == nullptr) {
                stop();
                return Error::NoMem;
        }
        Error err = _chain.start(BlitChain::Conversion::None, MaxPieces, MaxFillRows);
        if (err.isError()) {
                stop();
                return err;
        }
        _front.store(0, std::memory_order_release);
        _asked.store(false, std::memory_order_release);
        _frames.store(0, std::memory_order_release);
        _shown.store(0, std::memory_order_release);
        _presents = 0;
        _forwardPending = false;
        _stats = Stats();
        // Black, by the DMA: the back buffer, then (double) the other too.
        err = fill(bounds(), 0x000000, WaitForever);
        if (err.isOk() && count == 2) {
                _front.store(1, std::memory_order_release);
                err = fill(bounds(), 0x000000, WaitForever);
                _front.store(0, std::memory_order_release);
        }
        if (_damage != nullptr) std::memset(_damage, 0, size_t(_tileWords) * _tileRows * sizeof(uint32_t));
        if (err.isError()) stop();
        return err;
}

void Surface::stop() {
        _chain.stop();
        _widen.stop();
        for (uint8_t *&buffer : _buffers) {
                Memory::free(buffer);
                buffer = nullptr;
        }
        Memory::free(_line);
        _line = nullptr;
        Memory::free(_lines);
        _lines = nullptr;
        _linesBytes = 0;
        _linesUsed = 0;
        _fillLineCount = 0;
        _edgeCount = 0;
        _edgeRows = 0;
        _batching = false;
        Memory::free(_damage);
        _damage = nullptr;
        _forward = nullptr;
        _select = nullptr;
        _width = 0;
        _height = 0;
        return;
}

uint8_t *Surface::back() const {
        if (_buffering == Buffering::Single) return _buffers[0];
        return _buffers[_front.load(std::memory_order_acquire) ^ 1];
}

BlitChain::Picture Surface::picture(const uint8_t *buffer) const {
        return BlitChain::Picture{buffer, _width, _height, BytesPerPixel};
}

bool Surface::inside(const Rect &rect) const {
        return !rect.isEmpty() && rect.right() <= _width && rect.bottom() <= _height;
}

bool Surface::fits(const Rect &rect) const {
        return inside(rect) && rect.x % Align == 0 && rect.width % Align == 0;
}

// Before drawing into the back buffer: a pending swap made, and what the
// last present drew copied into it from the one now shown.
Error Surface::prepare(const Deadline &deadline) {
        // A widening copy under way finishes before anything else draws (its own chain).
        if (_widen.isBusy()) {
                const Error err = _widen.wait(remainingMs(deadline));
                if (err.isError()) return err;
        }
        if (_buffering == Buffering::Single) return Error::Ok;
        if (_asked.load(std::memory_order_acquire)) {
                const int64_t start = esp_timer_get_time();
                const Error   err = waitUntil([this] { return !_asked.load(std::memory_order_acquire); }, deadline);
                const auto    us = static_cast<uint32_t>(esp_timer_get_time() - start);
                _stats.waitUs += us;
                if (us > _stats.waitUsMax) _stats.waitUsMax = us;
                if (err.isError()) return err;
        }
        if (!_forwardPending) return Error::Ok;
        return copyForward(_forward, deadline);
}

// Before drawing that writes @p writes and reads @p reads of the back buffer: a pending swap made,
// and of the last present's changes still to copy forward (`_forward`), those the drawing reads or
// covers only in part copied now, those it covers whole dropped (it overwrites them: a window
// redrawn every frame is never copied twice), the rest left for later drawing or the present.
// More rectangles than kept: everything forwarded, as before.
Error Surface::prepareFor(const Deadline &deadline, Span<const Rect> writes, Span<const Rect> reads) {
        if (_buffering == Buffering::Single || !_forwardPending) return prepare(deadline);
        // The swap first (prepare() would forward everything after it: not yet).
        _forwardPending = false;
        Error err = prepare(deadline);
        _forwardPending = true;
        if (err.isError()) return err;
        const size_t words = size_t(_tileWords) * _tileRows;
        std::memset(_select, 0, words * sizeof(uint32_t));
        bool any = false;
        // Tiles a rectangle touches: [c0, c1] × [r0, r1].
        auto span = [&](const Rect &rect, uint32_t &c0, uint32_t &c1, uint32_t &r0, uint32_t &r1) {
                c0 = rect.x / TileWidth;
                c1 = (rect.right() - 1) / TileWidth;
                r0 = rect.y / TileHeight;
                r1 = (rect.bottom() - 1) / TileHeight;
                return;
        };
        auto select = [&](const Rect &rect, bool partOnly) {
                if (rect.isEmpty()) return;
                uint32_t c0, c1, r0, r1;
                span(rect, c0, c1, r0, r1);
                for (uint32_t r = r0; r <= r1; ++r) {
                        const uint32_t *pending = _forward + size_t(r) * _tileWords;
                        uint32_t       *chosen = _select + size_t(r) * _tileWords;
                        for (uint32_t c = c0; c <= c1; ++c) {
                                const uint32_t bit = 1u << (c % 32);
                                if ((pending[c / 32] & bit) == 0) continue;
                                if (partOnly) {
                                        const uint32_t x = c * TileWidth, y = r * TileHeight;
                                        const uint32_t right = x + TileWidth < _width ? x + TileWidth : _width;
                                        const uint32_t bottom = y + TileHeight < _height ? y + TileHeight : _height;
                                        if (rect.x <= x && rect.right() >= right && rect.y <= y &&
                                            rect.bottom() >= bottom)
                                                continue; // covered whole: dropped below unless read
                                }
                                chosen[c / 32] |= bit;
                                any = true;
                        }
                }
                return;
        };
        for (const Rect &rect : reads) select(rect, false);
        for (const Rect &rect : writes) select(rect, true);
        // Tiles covered whole and not chosen: dropped.
        for (const Rect &rect : writes) {
                if (rect.isEmpty()) continue;
                uint32_t c0, c1, r0, r1;
                span(rect, c0, c1, r0, r1);
                for (uint32_t r = r0; r <= r1; ++r) {
                        uint32_t       *pending = _forward + size_t(r) * _tileWords;
                        const uint32_t *chosen = _select + size_t(r) * _tileWords;
                        for (uint32_t c = c0; c <= c1; ++c) {
                                const uint32_t bit = 1u << (c % 32);
                                if ((chosen[c / 32] & bit) == 0) pending[c / 32] &= ~bit;
                        }
                }
        }
        if (any) {
                for (size_t i = 0; i < words; ++i) _forward[i] &= ~_select[i];
                err = copyForward(_select, deadline);
        }
        uint32_t left = 0;
        for (size_t i = 0; i < words; ++i) left |= _forward[i];
        _forwardPending = left != 0;
        return err;
}

// The tiles @p rect touches, marked drawn.
void Surface::markDamage(const Rect &rect) {
        const uint32_t c0 = rect.x / TileWidth, c1 = (rect.right() - 1) / TileWidth;
        const uint32_t r0 = rect.y / TileHeight, r1 = (rect.bottom() - 1) / TileHeight;
        for (uint32_t r = r0; r <= r1; ++r) {
                uint32_t *row = _damage + size_t(r) * _tileWords;
                for (uint32_t c = c0; c <= c1; ++c) row[c / 32] |= 1u << (c % 32);
        }
        return;
}

// The last present's tiles copied from the shown buffer into the one drawn
// next: each row's runs of tiles, a run under one of the same columns
// growing its rectangle down.
Error Surface::copyForward(const uint32_t *tiles, const Deadline &deadline) {
        struct Open {
                        uint16_t c0, c1, r0; // columns [c0, c1), from row r0
        };
        constexpr size_t         MaxOpen = 64;
        const int64_t            started = esp_timer_get_time();
        const BlitChain::Picture from = picture(shown());
        const BlitChain::Picture to = picture(back());
        const uint16_t           columns = static_cast<uint16_t>((_width + TileWidth - 1) / TileWidth);
        Open                     open[MaxOpen];
        size_t                   opened = 0;
        Error                    err;
        auto                     emit = [&](const Open &o, uint16_t r1) {
                const uint32_t right = uint32_t(o.c1) * TileWidth < _width ? uint32_t(o.c1) * TileWidth : _width;
                const uint32_t bottom = uint32_t(r1) * TileHeight < _height ? uint32_t(r1) * TileHeight : _height;
                const Rect     r{static_cast<uint16_t>(o.c0 * TileWidth), static_cast<uint16_t>(o.r0 * TileHeight),
                             static_cast<uint16_t>(right - o.c0 * TileWidth),
                             static_cast<uint16_t>(bottom - o.r0 * TileHeight)};
                if (err.isOk()) err = _chain.add(from, r.x, r.y, to, r.x, r.y, r.width, r.height);
                if (err == Error::Full) { // its pieces or rows (a batch's fills): the job so far run first
                        err = runChain(deadline);
                        if (err.isOk()) err = _chain.add(from, r.x, r.y, to, r.x, r.y, r.width, r.height);
                }
                ++_stats.forwards;
                _stats.forwardPixels += r.area();
        };
        for (uint16_t row = 0; row <= _tileRows; ++row) {
                // This row's runs; past the last row none, so everything closes.
                Open   runs[MaxOpen];
                size_t count = 0;
                if (row < _tileRows) {
                        const uint32_t *bits = tiles + size_t(row) * _tileWords;
                        for (uint16_t c = 0; c < columns;) {
                                if ((bits[c / 32] & (1u << (c % 32))) == 0) {
                                        ++c;
                                        continue;
                                }
                                const uint16_t c0 = c;
                                while (c < columns && (bits[c / 32] & (1u << (c % 32))) != 0) ++c;
                                runs[count++] = Open{c0, c, row}; // at most columns / 2 runs: fits
                        }
                }
                Open   next[MaxOpen];
                size_t nextCount = 0;
                for (size_t i = 0; i < count; ++i) {
                        bool grown = false;
                        for (size_t k = 0; k < opened && !grown; ++k) {
                                if (open[k].c1 == 0 || open[k].c0 != runs[i].c0 || open[k].c1 != runs[i].c1) continue;
                                next[nextCount++] = open[k];
                                open[k].c1 = 0; // continued
                                grown = true;
                        }
                        if (!grown) next[nextCount++] = runs[i];
                }
                for (size_t k = 0; k < opened; ++k)
                        if (open[k].c1 != 0) emit(open[k], row);
                for (size_t i = 0; i < nextCount; ++i) open[i] = next[i];
                opened = nextCount;
        }
        if (tiles == _forward) {
                std::memset(_forward, 0, size_t(_tileWords) * _tileRows * sizeof(uint32_t));
                _forwardPending = false;
        }
        // Batched, the copies go with the drawing after them (they read the shown buffer, which
        // nothing in the job writes; the drawing lands after them in order).
        if (err.isOk() && !_batching) err = runChain(deadline);
        const auto us = static_cast<uint32_t>(esp_timer_get_time() - started);
        _stats.forwardUs += us;
        if (us > _stats.forwardUsMax) _stats.forwardUsMax = us;
        return err;
}

Error Surface::fill(const Rect &rect, uint32_t rgb, unsigned int timeoutMs) {
        return fill(Span<const Rect>(&rect, 1), rgb, timeoutMs);
}

Error Surface::fill(Span<const Rect> rects, uint32_t rgb, unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        constexpr uint32_t Mask = Align - 1u;
        uint16_t           widest = 0; // the widest middle: whole groups of 4
        bool               edges = false;
        for (const Rect &rect : rects) {
                if (!inside(rect)) return Error::Invalid;
                const uint32_t left = (rect.x + Mask) & ~Mask, right = rect.right() & ~Mask;
                if (right > left && right - left > widest) widest = static_cast<uint16_t>(right - left);
                edges |= ((rect.x | rect.width) & Mask) != 0;
        }
        const Deadline deadline(timeoutMs);
        Error          err = prepareFor(deadline, rects, Span<const Rect>());
        if (err.isError()) return err;
        if (widest > 0 && _batching) {
                // Added to the batch, each middle from a line of the colour kept until the job runs.
                const BlitChain::Picture to = picture(back());
                for (const Rect &rect : rects) {
                        const uint32_t left = (rect.x + Mask) & ~Mask, right = rect.right() & ~Mask;
                        if (right <= left) continue;
                        const auto x = static_cast<uint16_t>(left), w = static_cast<uint16_t>(right - left);
                        // Pending edge rows under it: dropped (their writes would undo it).
                        err = coverEdges(Rect{x, rect.y, w, rect.height}, deadline);
                        if (err.isError()) return err;
                        const FillLine *line = fillLine(rgb, w);
                        if (line == nullptr) { // no room: the batch so far run, its lines free
                                err = runChain(deadline);
                                line = err.isOk() ? fillLine(rgb, w) : nullptr;
                                if (err.isOk() && line == nullptr) err = Error::NoMem;
                        }
                        for (uint32_t y = rect.y; err.isOk() && y < rect.bottom(); y += MaxFillRows) {
                                const uint16_t rows = static_cast<uint16_t>(
                                        rect.bottom() - y < MaxFillRows ? rect.bottom() - y : MaxFillRows);
                                err = _chain.addFill(line->line, line->rows, to, x, static_cast<uint16_t>(y), w, rows);
                                if (err == Error::Full) {
                                        err = runChain(deadline);
                                        line = err.isOk() ? fillLine(rgb, w) : nullptr;
                                        if (line != nullptr)
                                                err = _chain.addFill(line->line, line->rows, to, x,
                                                                     static_cast<uint16_t>(y), w, rows);
                                }
                        }
                        if (err.isError()) {
                                _chain.clear();
                                return err;
                        }
                }
        } else if (widest > 0) {
                // One line of the color, the widest middle's length, sent once a row by each.
                const uint8_t b = static_cast<uint8_t>(rgb), g = static_cast<uint8_t>(rgb >> 8),
                              r = static_cast<uint8_t>(rgb >> 16);
                const uint32_t w0 = uint32_t(b) | uint32_t(g) << 8 | uint32_t(r) << 16 | uint32_t(b) << 24;
                const uint32_t w1 = uint32_t(g) | uint32_t(r) << 8 | uint32_t(b) << 16 | uint32_t(g) << 24;
                const uint32_t w2 = uint32_t(r) | uint32_t(b) << 8 | uint32_t(g) << 16 | uint32_t(r) << 24;
                auto          *words = reinterpret_cast<uint32_t *>(_line); // 4 pixels: 3 words
                for (size_t i = 0; i < size_t(widest) / Align; ++i) {
                        words[3 * i] = w0;
                        words[3 * i + 1] = w1;
                        words[3 * i + 2] = w2;
                }
                const size_t lineBytes = (size_t(widest) * BytesPerPixel + LineAlign - 1) / LineAlign * LineAlign;
                (void)esp_cache_msync(_line, lineBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                const BlitChain::Picture to = picture(back());
                for (const Rect &rect : rects) {
                        const uint32_t left = (rect.x + Mask) & ~Mask, right = rect.right() & ~Mask;
                        if (right <= left) continue;
                        const auto x = static_cast<uint16_t>(left), w = static_cast<uint16_t>(right - left);
                        for (uint32_t y = rect.y; y < rect.bottom(); y += MaxFillRows) {
                                const uint16_t rows = static_cast<uint16_t>(
                                        rect.bottom() - y < MaxFillRows ? rect.bottom() - y : MaxFillRows);
                                err = _chain.addFill(_line, to, x, static_cast<uint16_t>(y), w, rows);
                                if (err == Error::Full) {
                                        err = runChain(deadline);
                                        if (err.isOk())
                                                err = _chain.addFill(_line, to, x, static_cast<uint16_t>(y), w, rows);
                                }
                                if (err.isError()) {
                                        _chain.clear();
                                        return err;
                                }
                        }
                }
                err = runChain(deadline);
        }
        if (err.isOk() && edges && _batching) {
                // The sides' groups pending in the batch, read and patched with others' later.
                for (const Rect &rect : rects) {
                        const uint32_t right = rect.right();
                        const uint32_t leftGroup = rect.x & ~Mask, rightGroup = right & ~Mask;
                        if (err.isOk() && (rect.x & Mask) != 0) {
                                const uint32_t to = right < leftGroup + Align ? right : leftGroup + Align;
                                err = addEdge(static_cast<uint16_t>(leftGroup), rect.y, rect.height,
                                              static_cast<uint8_t>(rect.x - leftGroup),
                                              static_cast<uint8_t>(to - leftGroup), rgb, deadline);
                        }
                        if (err.isOk() && (right & Mask) != 0 && !((rect.x & Mask) != 0 && rightGroup == leftGroup)) {
                                const uint32_t from = rect.x > rightGroup ? rect.x : rightGroup;
                                err = addEdge(static_cast<uint16_t>(rightGroup), rect.y, rect.height,
                                              static_cast<uint8_t>(from - rightGroup),
                                              static_cast<uint8_t>(right - rightGroup), rgb, deadline);
                        }
                }
        } else if (err.isOk() && edges) {
                err = fillEdges(rects, rgb, deadline);
        }
        if (_buffering == Buffering::Double)
                for (const Rect &rect : rects) markDamage(rect);
        return err;
}

// The groups of 4 pixels a fill's sides cut: read into internal RAM (the
// fill's line, free again), the fill's pixels set, written back.
Error Surface::fillEdges(Span<const Rect> rects, uint32_t rgb, const Deadline &deadline) {
        struct Edge {
                        uint16_t x; // the group's first column
                        uint16_t y;
                        uint16_t rows;
                        uint8_t  from; // the pixels set: [from, to) of the group
                        uint8_t  to;
                        uint16_t offset; // its first row in the buffer
        };
        constexpr uint32_t       Mask = Align - 1u;
        constexpr size_t         GroupBytes = Align * BytesPerPixel;
        const size_t             capacity = ((stride() + LineAlign - 1) / LineAlign * LineAlign) / GroupBytes; // rows
        const BlitChain::Picture surface = picture(back());
        const BlitChain::Picture buffer{_line, Align, static_cast<uint16_t>(capacity), BytesPerPixel};
        Edge                     batch[MaxPieces];
        size_t                   count = 0, used = 0;
        Error                    err;

        auto flush = [&]() {
                if (count == 0 || err.isError()) return;
                for (size_t i = 0; i < count && err.isOk(); ++i) {
                        const Edge &e = batch[i];
                        err = _chain.add(surface, e.x, e.y, buffer, 0, e.offset, Align, e.rows);
                }
                if (err.isOk()) err = runChain(deadline);
                const size_t bytes = (used * GroupBytes + LineAlign - 1) / LineAlign * LineAlign;
                if (err.isOk()) (void)esp_cache_msync(_line, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                for (size_t i = 0; i < count && err.isOk(); ++i) {
                        const Edge &e = batch[i];
                        for (uint16_t r = 0; r < e.rows; ++r) {
                                uint8_t *p =
                                        _line + (size_t(e.offset) + r) * GroupBytes + size_t(e.from) * BytesPerPixel;
                                for (uint8_t x = e.from; x < e.to; ++x, p += BytesPerPixel) {
                                        p[0] = static_cast<uint8_t>(rgb);
                                        p[1] = static_cast<uint8_t>(rgb >> 8);
                                        p[2] = static_cast<uint8_t>(rgb >> 16);
                                }
                        }
                }
                if (err.isOk()) (void)esp_cache_msync(_line, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                for (size_t i = 0; i < count && err.isOk(); ++i) {
                        const Edge &e = batch[i];
                        err = _chain.add(buffer, 0, e.offset, surface, e.x, e.y, Align, e.rows);
                }
                if (err.isOk()) err = runChain(deadline);
                if (err.isError()) _chain.clear();
                count = 0;
                used = 0;
        };
        // Two edges of one group on the same rows in one batch would both read its old pixels, and
        // the second's write would undo the first's (neighbouring fills: a graph's bars): the batch
        // goes first.
        auto clashes = [&](uint16_t x, uint16_t y, uint16_t rows) {
                for (size_t i = 0; i < count; ++i) {
                        const Edge &e = batch[i];
                        if (e.x == x && y < e.y + e.rows && e.y < y + rows) return true;
                }
                return false;
        };
        auto addEdge = [&](uint16_t x, uint16_t y, uint16_t rows, uint8_t from, uint8_t to) {
                while (rows > 0 && err.isOk()) {
                        if (count == MaxPieces || used == capacity || clashes(x, y, rows)) flush();
                        const auto n = static_cast<uint16_t>(rows < capacity - used ? rows : capacity - used);
                        batch[count++] = Edge{x, y, n, from, to, static_cast<uint16_t>(used)};
                        used += n;
                        y = static_cast<uint16_t>(y + n);
                        rows = static_cast<uint16_t>(rows - n);
                }
        };
        for (const Rect &rect : rects) {
                const uint32_t right = rect.right();
                const uint32_t leftGroup = rect.x & ~Mask, rightGroup = right & ~Mask;
                if ((rect.x & Mask) != 0) {
                        const uint32_t to = right < leftGroup + Align ? right : leftGroup + Align;
                        addEdge(static_cast<uint16_t>(leftGroup), rect.y, rect.height,
                                static_cast<uint8_t>(rect.x - leftGroup), static_cast<uint8_t>(to - leftGroup));
                }
                if ((right & Mask) != 0 && !((rect.x & Mask) != 0 && rightGroup == leftGroup)) {
                        const uint32_t from = rect.x > rightGroup ? rect.x : rightGroup;
                        addEdge(static_cast<uint16_t>(rightGroup), rect.y, rect.height,
                                static_cast<uint8_t>(from - rightGroup), static_cast<uint8_t>(right - rightGroup));
                }
        }
        flush();
        return err;
}

Error Surface::patch(const Rect &rect, Patcher patcher, void *arg, unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        if (!inside(rect) || patcher == nullptr) return Error::Invalid;
        constexpr uint32_t Mask = Align - 1u;
        const auto         left = static_cast<uint16_t>(rect.x & ~Mask);
        const auto         width = static_cast<uint16_t>(((rect.right() + Mask) & ~Mask) - left);
        const size_t       rowBytes = size_t(width) * BytesPerPixel;
        const size_t       bytes = (rowBytes * rect.height + LineAlign - 1) / LineAlign * LineAlign;
        if (bytes > (stride() + LineAlign - 1) / LineAlign * LineAlign) return Error::Invalid;
        const Deadline deadline(timeoutMs);
        const Rect     group{left, rect.y, width, rect.height};
        Error          err = prepareFor(deadline, Span<const Rect>(), Span<const Rect>(&group, 1));
        if (err.isOk() && _batching) err = flushBatch(deadline); // it reads what the batch writes
        if (err.isError()) return err;
        const BlitChain::Picture surface = picture(back());
        const BlitChain::Picture buffer{_line, width, rect.height, BytesPerPixel};
        err = _chain.add(surface, left, rect.y, buffer, 0, 0, width, rect.height);
        if (err.isOk()) err = runChain(deadline);
        if (err.isOk()) {
                (void)esp_cache_msync(_line, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                patcher(arg, _line + size_t(rect.x - left) * BytesPerPixel, rowBytes);
                (void)esp_cache_msync(_line, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                err = _chain.add(buffer, 0, 0, surface, left, rect.y, width, rect.height);
        }
        if (err.isOk()) err = runChain(deadline);
        if (err.isError()) _chain.clear();
        if (err.isOk() && _buffering == Buffering::Double) markDamage(Rect{left, rect.y, width, rect.height});
        return err;
}

Error Surface::blit(const Rect &to, const uint8_t *src, size_t srcStride, unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        if (!fits(to) || src == nullptr || srcStride % BytesPerPixel != 0 ||
            srcStride < size_t(to.width) * BytesPerPixel || srcStride / BytesPerPixel > UINT16_MAX)
                return Error::Invalid;
        const Deadline deadline(timeoutMs);
        Error          err = prepareFor(deadline, Span<const Rect>(&to, 1), Span<const Rect>());
        if (err.isOk() && _batching) err = coverEdges(to, deadline);
        if (err.isError()) return err;
        // The source out of the cache for the DMA, in pieces.
        const size_t bytes = srcStride * (to.height - 1) + size_t(to.width) * BytesPerPixel;
        for (size_t off = 0; off < bytes; off += WriteBackChunk)
                (void)esp_cache_msync(const_cast<uint8_t *>(src) + off,
                                      bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        const BlitChain::Picture from{src, static_cast<uint16_t>(srcStride / BytesPerPixel), to.height, BytesPerPixel};
        err = _chain.add(from, 0, 0, picture(back()), to.x, to.y, to.width, to.height);
        if (err == Error::Full) { // a full batch run first
                err = runChain(deadline);
                if (err.isOk()) err = _chain.add(from, 0, 0, picture(back()), to.x, to.y, to.width, to.height);
        }
        if (err.isOk()) err = runChain(deadline);
        if (err.isOk() && _buffering == Buffering::Double) markDamage(to);
        return err;
}

Error Surface::startBlit(const Rect &to, const uint8_t *src, size_t srcStride, Span<const Rect> clip, Source source,
                         uint16_t repeat, uint16_t first, unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        const size_t pixel = source == Source::Rgb565 ? 2 : BytesPerPixel;
        if (!fits(to) || src == nullptr || srcStride % pixel != 0 || srcStride < size_t(to.width) * pixel ||
            srcStride / pixel > UINT16_MAX || repeat == 0 || first >= repeat)
                return Error::Invalid;
        const uint16_t rows = uint16_t((uint32_t(first) + to.height - 1) / repeat + 1); // of the source
        for (const Rect &piece : clip)
                if (to.intersects(piece) && !fits(to.intersected(piece))) return Error::Invalid;
        if (source == Source::Rgb565 && !_widen.isStarted()) {
                const Error err = _widen.start(BlitChain::Conversion::Rgb565ToRgb888, MaxPieces, MaxWidenRows);
                if (err.isError()) return err;
        }
        BlitChain     &chain = source == Source::Rgb565 ? _widen : _chain;
        const Deadline deadline(timeoutMs);
        // What it writes: its clip's pieces (more than kept: everything forwarded first).
        Rect   pieces[MaxPrepareRects];
        size_t n = 0;
        bool   kept = true;
        for (const Rect &piece : clip) {
                const Rect cut = to.intersected(piece);
                if (cut.isEmpty()) continue;
                if (n == MaxPrepareRects) {
                        kept = false;
                        break;
                }
                pieces[n++] = cut;
        }
        Error err = kept ? prepareFor(deadline, Span<const Rect>(pieces, n), Span<const Rect>()) : prepare(deadline);
        const bool batched = _batching && &chain == &_chain;
        // The source of a job under way may be ours; the other chain's job lands first (a batch's
        // too: the widening goes over what it drew).
        if (err.isOk() && !batched) err = _batching ? flushBatch(deadline) : _chain.wait(remainingMs(deadline));
        if (err.isError()) return err;
        const size_t bytes = srcStride * (rows - 1) + size_t(to.width) * pixel;
        for (size_t off = 0; off < bytes; off += WriteBackChunk)
                (void)esp_cache_msync(const_cast<uint8_t *>(src) + off,
                                      bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        const BlitChain::Picture from{src, static_cast<uint16_t>(srcStride / pixel), rows, uint8_t(pixel)};
        const BlitChain::Picture into = picture(back());
        for (const Rect &piece : clip) {
                const Rect cut = to.intersected(piece);
                if (cut.isEmpty()) continue;
                const uint32_t down = first + uint32_t(cut.y - to.y); // the cut's first row, in repeats
                const auto     add = [&] {
                        return chain.addRepeated(from, static_cast<uint16_t>(cut.x - to.x), uint16_t(down / repeat),
                                                     repeat, uint16_t(down % repeat), into, cut.x, cut.y, cut.width,
                                                     cut.height);
                };
                if (batched) err = coverEdges(cut, deadline);
                if (err.isOk() && chain.count() == chain.capacity())
                        err = batched ? runChain(deadline) : chain.run(remainingMs(deadline));
                if (err.isOk()) err = add();
                if (err == Error::Full) { // the rows' descriptors: the job so far sent first
                        err = batched ? runChain(deadline) : chain.run(remainingMs(deadline));
                        if (err.isOk()) err = add();
                }
                if (err.isError()) {
                        chain.clear();
                        return err;
                }
                if (_buffering == Buffering::Double) markDamage(cut);
        }
        return batched ? Error::Ok : chain.submit();
}

Error Surface::startTile(const Rect &to, const Tile &tile, uint16_t x, uint32_t down, Span<const Rect> clip,
                         unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        if (!fits(to) || tile.pixels == nullptr || tile.stride % BytesPerPixel != 0 || tile.width == 0 ||
            tile.width % Align != 0 || tile.stride < size_t(tile.width) * BytesPerPixel ||
            tile.stride / BytesPerPixel > UINT16_MAX || tile.height == 0 || tile.repeat == 0 || x >= tile.width ||
            x % Align != 0)
                return Error::Invalid;
        for (const Rect &piece : clip)
                if (to.intersects(piece) && !fits(to.intersected(piece))) return Error::Invalid;
        const Deadline deadline(timeoutMs);
        Rect           pieces[MaxPrepareRects];
        size_t         n = 0;
        bool           kept = true;
        for (const Rect &piece : clip) {
                const Rect cut = to.intersected(piece);
                if (cut.isEmpty()) continue;
                if (n == MaxPrepareRects) {
                        kept = false;
                        break;
                }
                pieces[n++] = cut;
        }
        if (n == 0 && kept) return Error::Ok;
        Error err = kept ? prepareFor(deadline, Span<const Rect>(pieces, n), Span<const Rect>()) : prepare(deadline);
        if (err.isOk() && !_batching) err = _chain.wait(remainingMs(deadline));
        if (err.isError()) return err;
        const size_t bytes = tile.stride * (tile.height - 1u) + size_t(tile.width) * BytesPerPixel;
        for (size_t off = 0; off < bytes; off += WriteBackChunk)
                (void)esp_cache_msync(const_cast<uint8_t *>(tile.pixels) + off,
                                      bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        const BlitChain::Picture from{tile.pixels, static_cast<uint16_t>(tile.stride / BytesPerPixel), tile.height,
                                      uint8_t(BytesPerPixel)};
        const BlitChain::Picture into = picture(back());
        // Bands down that a chain takes whole: a run of rows a descriptor, or a row each.
        const uint32_t band = tile.repeat == 1 ? uint32_t(tile.height) * 32u : 64u;
        const auto     add = [&](uint16_t fromX, uint32_t at, uint16_t toX, uint16_t toY, uint16_t w, uint16_t h) {
                Error e = Error::Ok;
                if (_chain.count() == _chain.capacity())
                        e = _batching ? runChain(deadline) : _chain.run(remainingMs(deadline));
                if (e.isOk()) e = _chain.addTiled(from, fromX, at, tile.repeat, into, toX, toY, w, h);
                if (e == Error::Full) {
                        e = _batching ? runChain(deadline) : _chain.run(remainingMs(deadline));
                        if (e.isOk()) e = _chain.addTiled(from, fromX, at, tile.repeat, into, toX, toY, w, h);
                }
                return e;
        };
        for (const Rect &piece : clip) {
                const Rect cut = to.intersected(piece);
                if (cut.isEmpty()) continue;
                if (_batching) err = coverEdges(cut, deadline);
                for (uint32_t r = 0; err.isOk() && r < cut.height; r += band) {
                        const auto     h = static_cast<uint16_t>(cut.height - r < band ? cut.height - r : band);
                        const uint32_t at = down + uint32_t(cut.y - to.y) + r;
                        // From the tile's column there to its end, then whole widths.
                        uint32_t column = (uint32_t(x) + uint32_t(cut.x - to.x)) % tile.width;
                        for (uint32_t c = 0; err.isOk() && c < cut.width; column = 0) {
                                const uint32_t w =
                                        cut.width - c < tile.width - column ? cut.width - c : tile.width - column;
                                err = add(uint16_t(column), at, uint16_t(cut.x + c), uint16_t(cut.y + r), uint16_t(w),
                                          h);
                                c += w;
                        }
                }
                if (err.isError()) {
                        _chain.clear();
                        return err;
                }
                if (_buffering == Buffering::Double) markDamage(cut);
        }
        return _batching ? Error::Ok : _chain.submit();
}

Error Surface::willDraw(Span<const Rect> rects, unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        for (const Rect &rect : rects)
                if (!inside(rect)) return Error::Invalid;
        const Deadline deadline(timeoutMs);
        return prepareFor(deadline, rects, Span<const Rect>());
}

Error Surface::wait(unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        const Error    err = _widen.wait(remainingMs(deadline));
        return err.isError() ? err : flushBatch(deadline); // a batch run too
}

Error Surface::flushBatch(const Deadline &deadline) {
        const Error err = resolveEdges(deadline);
        return err.isError() ? err : runChain(deadline);
}

// Pending edge i cut in two at @p row (inside it): false if there is no slot for the second part.
bool Surface::splitEdge(size_t i, uint16_t row) {
        PendingEdge &e = _edgesPending[i];
        if (row <= e.y || row >= e.y + e.rows) return true;
        if (_edgeCount == MaxPendingEdges) return false;
        PendingEdge &rest = _edgesPending[_edgeCount++];
        rest = e;
        rest.y = row;
        rest.rows = static_cast<uint16_t>(e.y + e.rows - row);
        e.rows = static_cast<uint16_t>(row - e.y);
        return true;
}

// @p rect, about to be written whole (4-pixel columns: every group it meets, whole), takes the
// pending edges' rows it covers: their writes would undo it, and it sets those pixels anyway.
// Out of slots to cut them, they are resolved instead.
Error Surface::coverEdges(const Rect &rect, const Deadline &deadline) {
        for (size_t i = 0; i < _edgeCount;) {
                PendingEdge &e = _edgesPending[i];
                if (!Rect{e.x, e.y, Align, e.rows}.intersects(rect)) {
                        ++i;
                        continue;
                }
                if (e.x < rect.x || e.x + Align > rect.right()) return resolveEdges(deadline); // not whole
                if (!splitEdge(i, rect.y) || !splitEdge(i, rect.bottom())) return resolveEdges(deadline);
                if (e.y >= rect.y && e.y + e.rows <= rect.bottom()) { // inside: dropped
                        _edgeRows = static_cast<uint16_t>(_edgeRows - e.rows);
                        e = _edgesPending[--_edgeCount];
                        continue; // the one moved here is looked at next
                }
                ++i;
        }
        return Error::Ok;
}

// Pixels [@p from, @p to) of the group at column @p x, @p rows from @p y, to be @p rgb. Pending
// groups of the same column are cut on its first and last rows; on the rows one already holds the
// colours are merged into it (the later fill's win), the others get new ones. Out of slots or of
// the line's rows, everything pending is resolved first.
Error Surface::addEdge(uint16_t x, uint16_t y, uint16_t rows, uint8_t from, uint8_t to, uint32_t rgb,
                       const Deadline &deadline) {
        constexpr size_t GroupBytes = Align * BytesPerPixel;
        const size_t     capacity = ((stride() + LineAlign - 1) / LineAlign * LineAlign) / GroupBytes; // rows
        uint8_t          mask = 0;
        for (uint8_t i = from; i < to; ++i) mask = static_cast<uint8_t>(mask | 1u << i);
        const uint32_t end = uint32_t(y) + rows;
        bool           cut = true;
        for (size_t i = 0; i < _edgeCount && cut; ++i)
                if (_edgesPending[i].x == x) cut = splitEdge(i, y) && splitEdge(i, static_cast<uint16_t>(end));
        if (!cut) {
                const Error err = resolveEdges(deadline);
                if (err.isError()) return err;
        }
        uint32_t row = y;
        while (row < end) {
                // A pending part starting here: merged. Else new up to the next part's start.
                uint32_t next = end;
                bool     merged = false;
                for (size_t i = 0; i < _edgeCount; ++i) {
                        PendingEdge &e = _edgesPending[i];
                        if (e.x != x || e.y + e.rows <= row || e.y >= end) continue;
                        if (e.y == row) {
                                for (uint8_t k = from; k < to; ++k) e.rgb[k] = rgb;
                                e.mask = static_cast<uint8_t>(e.mask | mask);
                                row += e.rows;
                                merged = true;
                                break;
                        }
                        if (e.y > row && e.y < next) next = e.y;
                }
                if (merged) continue;
                if (_edgeCount == MaxPendingEdges || _edgeRows == capacity) {
                        const Error err = resolveEdges(deadline); // everything pending written: none left
                        if (err.isError()) return err;
                        next = end;
                }
                const uint32_t room = capacity - _edgeRows;
                const auto     n = static_cast<uint16_t>(next - row < room ? next - row : room);
                PendingEdge   &e = _edgesPending[_edgeCount++];
                e.x = x;
                e.y = static_cast<uint16_t>(row);
                e.rows = n;
                e.mask = mask;
                for (uint8_t k = from; k < to; ++k) e.rgb[k] = rgb;
                _edgeRows = static_cast<uint16_t>(_edgeRows + n);
                row += n;
        }
        return Error::Ok;
}

// The pending edge groups: what the batch wrote run, the groups read into the fill line (one
// job), their pixels set, and their writes added to the batch (the line kept until it runs).
Error Surface::resolveEdges(const Deadline &deadline) {
        if (_edgeCount == 0) return Error::Ok;
        constexpr size_t         GroupBytes = Align * BytesPerPixel;
        const BlitChain::Picture surface = picture(back());
        const BlitChain::Picture buffer{_line, Align, _edgeRows, BytesPerPixel};
        Error                    err = runChain(deadline);
        size_t                   offset = 0;
        for (size_t i = 0; i < _edgeCount && err.isOk(); ++i) {
                const PendingEdge &e = _edgesPending[i];
                err = _chain.add(surface, e.x, e.y, buffer, 0, static_cast<uint16_t>(offset), Align, e.rows);
                offset += e.rows;
        }
        if (err.isOk()) err = runChain(deadline);
        const size_t bytes = (offset * GroupBytes + LineAlign - 1) / LineAlign * LineAlign;
        if (err.isOk()) (void)esp_cache_msync(_line, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        offset = 0;
        for (size_t i = 0; i < _edgeCount && err.isOk(); ++i) {
                const PendingEdge &e = _edgesPending[i];
                for (uint16_t r = 0; r < e.rows; ++r) {
                        uint8_t *p = _line + (offset + r) * GroupBytes;
                        for (uint8_t k = 0; k < Align; ++k, p += BytesPerPixel) {
                                if ((e.mask & (1u << k)) == 0) continue;
                                p[0] = static_cast<uint8_t>(e.rgb[k]);
                                p[1] = static_cast<uint8_t>(e.rgb[k] >> 8);
                                p[2] = static_cast<uint8_t>(e.rgb[k] >> 16);
                        }
                }
                offset += e.rows;
        }
        if (err.isOk()) (void)esp_cache_msync(_line, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        offset = 0;
        for (size_t i = 0; i < _edgeCount && err.isOk(); ++i) {
                const PendingEdge &e = _edgesPending[i];
                err = _chain.add(buffer, 0, static_cast<uint16_t>(offset), surface, e.x, e.y, Align, e.rows);
                offset += e.rows;
        }
        if (err.isError()) _chain.clear();
        _edgeCount = 0;
        _edgeRows = 0;
        return err;
}

Error Surface::endBatch(unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        const Error    err = flushBatch(deadline);
        _batching = false;
        return err;
}

// The job written run and waited for (none: the one under way waited for): every piece written,
// the fills' lines free again.
Error Surface::runChain(const Deadline &deadline) {
        const Error err = _chain.run(remainingMs(deadline));
        if (err.isOk()) {
                _linesUsed = 0;
                _fillLineCount = 0;
        }
        return err;
}

// Lines of @p width pixels of @p rgb for a batched fill: ones already made of the colour and at
// least as long, or new ones from the lines' memory, as many rows of it as fit in MaxFillLineBytes
// (up to MaxFillLineRows: a tall fill then takes that many times fewer descriptors); null when the
// memory is full.
const Surface::FillLine *Surface::fillLine(uint32_t rgb, uint16_t width) {
        for (size_t i = 0; i < _fillLineCount; ++i)
                if (_fillLines[i].rgb == rgb && _fillLines[i].width >= width) return &_fillLines[i];
        const size_t rowBytes = size_t(width) * BytesPerPixel;
        size_t       rows = MaxFillLineBytes / rowBytes;
        rows = rows < 1 ? 1 : rows > MaxFillLineRows ? MaxFillLineRows : rows;
        const size_t bytes = (rowBytes * rows + LineAlign - 1) / LineAlign * LineAlign;
        if (_lines == nullptr || _linesUsed + bytes > _linesBytes) return nullptr;
        uint8_t *line = _lines + _linesUsed;
        _linesUsed += bytes;
        const uint8_t b = static_cast<uint8_t>(rgb), g = static_cast<uint8_t>(rgb >> 8),
                      r = static_cast<uint8_t>(rgb >> 16);
        const uint32_t w0 = uint32_t(b) | uint32_t(g) << 8 | uint32_t(r) << 16 | uint32_t(b) << 24;
        const uint32_t w1 = uint32_t(g) | uint32_t(r) << 8 | uint32_t(b) << 16 | uint32_t(g) << 24;
        const uint32_t w2 = uint32_t(r) | uint32_t(b) << 8 | uint32_t(g) << 16 | uint32_t(r) << 24;
        auto          *words = reinterpret_cast<uint32_t *>(line); // 4 pixels: 3 words; rows back to back
        for (size_t i = 0; i < size_t(width) / Align * rows; ++i) {
                words[3 * i] = w0;
                words[3 * i + 1] = w1;
                words[3 * i + 2] = w2;
        }
        (void)esp_cache_msync(line, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        size_t slot = _fillLineCount;
        if (slot == MaxFillLines) { // all in use: one forgotten (its line stays until the job runs)
                slot = _fillLineNext;
                _fillLineNext = static_cast<uint8_t>((_fillLineNext + 1) % MaxFillLines);
        } else {
                ++_fillLineCount;
        }
        _fillLines[slot] = FillLine{rgb, width, static_cast<uint16_t>(rows), line};
        return &_fillLines[slot];
}

Error Surface::enableBatching(size_t maxPieces, size_t maxRows, size_t lineBytes) {
        if (!isStarted()) return Error::InvalidState;
        if (maxPieces == 0 || lineBytes < stride()) return Error::Invalid;
        if (_lines != nullptr) return Error::Ok;
        const Deadline deadline(WaitForever);
        Error          err = runChain(deadline);
        if (err.isError()) return err;
        const size_t bytes = (lineBytes + LineAlign - 1) / LineAlign * LineAlign;
        auto        *lines = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, bytes, LineAlign));
        if (lines == nullptr) return Error::NoMem;
        _chain.stop();
        err = _chain.start(BlitChain::Conversion::None, maxPieces, maxRows);
        if (err.isError()) {
                Memory::free(lines);
                const Error back = _chain.start(BlitChain::Conversion::None, MaxPieces, MaxFillRows);
                return back.isError() ? back : err;
        }
        _lines = lines;
        _linesBytes = bytes;
        _linesUsed = 0;
        _fillLineCount = 0;
        _fillLineNext = 0;
        return Error::Ok;
}

Error Surface::copy(Span<const Copy> copies, unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        for (const Copy &c : copies)
                if (!fits(c.from) || !fits(Rect{c.toX, c.toY, c.from.width, c.from.height})) return Error::Invalid;
        const Deadline deadline(timeoutMs);
        // Its sources read, its destinations written (more than kept: everything forwarded first).
        Rect  reads[MaxPrepareRects], writes[MaxPrepareRects];
        Error err;
        if (copies.size() <= MaxPrepareRects) {
                for (size_t i = 0; i < copies.size(); ++i) {
                        reads[i] = copies[i].from;
                        writes[i] = Rect{copies[i].toX, copies[i].toY, copies[i].from.width, copies[i].from.height};
                }
                err = prepareFor(deadline, Span<const Rect>(writes, copies.size()),
                                 Span<const Rect>(reads, copies.size()));
        } else {
                err = prepare(deadline);
        }
        if (err.isOk() && _batching) err = flushBatch(deadline); // it reads what the batch writes
        if (err.isError()) return err;
        const BlitChain::Picture pic = picture(back());
        for (const Copy &c : copies) {
                err = _chain.add(pic, c.from.x, c.from.y, pic, c.toX, c.toY, c.from.width, c.from.height);
                if (err == Error::Full) { // its pieces or rows: the job so far run first
                        err = runChain(deadline);
                        if (err.isOk())
                                err = _chain.add(pic, c.from.x, c.from.y, pic, c.toX, c.toY, c.from.width,
                                                 c.from.height);
                }
                if (err.isError()) {
                        _chain.clear();
                        return err;
                }
                if (_buffering == Buffering::Double) markDamage(Rect{c.toX, c.toY, c.from.width, c.from.height});
        }
        return runChain(deadline);
}

Result<uint32_t> Surface::present(unsigned int timeoutMs) {
        if (!isStarted()) return makeError<uint32_t>(Error::InvalidState);
        if (_buffering == Buffering::Single) {
                // Shown as it was drawn (a batch's pieces once they are).
                if (_batching) {
                        const Error err = flushBatch(Deadline(timeoutMs));
                        if (err.isError()) return makeError<uint32_t>(err);
                }
                _shown.store(++_presents, std::memory_order_release);
                return makeResult(_presents);
        }
        const Deadline deadline(timeoutMs);
        Error          err = prepare(deadline);
        if (err.isOk()) err = flushBatch(deadline); // a started copy or a batch, finished before the swap
        if (err.isError()) return makeError<uint32_t>(err);
        const size_t words = size_t(_tileWords) * _tileRows;
        std::memcpy(_forward, _damage, words * sizeof(uint32_t));
        std::memset(_damage, 0, words * sizeof(uint32_t));
        _forwardPending = true;
        ++_presents;
        ++_stats.presents;
        _asked.store(true, std::memory_order_release);
        if (!_scanning.load(std::memory_order_acquire)) swapNow();
        return makeResult(_presents);
}

Error Surface::waitFrame(uint32_t seen, unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        return waitUntil([this, seen] { return _frames.load(std::memory_order_acquire) != seen; }, deadline);
}

Error Surface::waitShown(uint32_t number, unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        return waitUntil(
                [this, number] { return static_cast<int32_t>(_shown.load(std::memory_order_acquire) - number) >= 0; },
                deadline);
}

// Waits until @p done() holds, woken by each swap.
template <typename Done> Error Surface::waitUntil(Done done, const Deadline &deadline) {
        while (!done()) {
                Semaphore       woken(1, 0);
                WaitList::Entry entry(_waiters, woken);
                if (!entry.isAdded()) return Error::Busy;
                if (done()) break;
                if (woken.acquire(deadline).isError()) return done() ? Error(Error::Ok) : Error(Error::Timeout);
        }
        return Error::Ok;
}

// The scanout has started a frame from the asked-for buffer.
BMDS_ISR_ATTR void Surface::swappedFromIsr(bool notify) {
        _front.store(_front.load(std::memory_order_relaxed) ^ 1, std::memory_order_release);
        _shown.store(_presents, std::memory_order_release);
        _asked.store(false, std::memory_order_release);
        if (notify) _waiters.notifyFromIsr();
        return;
}

void Surface::swapNow() {
        if (!_asked.load(std::memory_order_acquire)) return;
        _front.store(_front.load(std::memory_order_relaxed) ^ 1, std::memory_order_release);
        _shown.store(_presents, std::memory_order_release);
        _asked.store(false, std::memory_order_release);
        _waiters.notify();
        return;
}

void Surface::setScanning(bool scanning) {
        _scanning.store(scanning, std::memory_order_release);
        // No scan to make it: a pending swap now.
        if (!scanning) swapNow();
        return;
}

BMDS_NAMESPACE_END
