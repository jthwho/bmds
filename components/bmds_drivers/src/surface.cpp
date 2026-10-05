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
#include <bmds/surface.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t BufferAlign = 128;          // the L2 cache line; the 2D-DMA's PSRAM alignment
        constexpr size_t LineAlign = 64;             // the L1 cache line (internal RAM)
        constexpr size_t WriteBackChunk = 64 * 1024; // a cache operation runs with interrupts off
        constexpr size_t MaxPieces = 32;             // a job's copies
        constexpr size_t MaxFillRows = 1024;         // rows a fill sends in one job

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
                _damage = static_cast<uint32_t *>(Memory::alloc(MemSpace::Internal, words * 2 * sizeof(uint32_t)));
                if (_damage == nullptr) {
                        stop();
                        return Error::NoMem;
                }
                _forward = _damage + words;
                std::memset(_damage, 0, words * 2 * sizeof(uint32_t));
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
        for (uint8_t *&buffer : _buffers) {
                Memory::free(buffer);
                buffer = nullptr;
        }
        Memory::free(_line);
        _line = nullptr;
        Memory::free(_damage);
        _damage = nullptr;
        _forward = nullptr;
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

bool Surface::fits(const Rect &rect) const {
        return !rect.isEmpty() && rect.right() <= _width && rect.bottom() <= _height && rect.x % Align == 0 &&
               rect.width % Align == 0;
}

// Before drawing into the back buffer: a pending swap made, and what the
// last present drew copied into it from the one now shown.
Error Surface::prepare(const Deadline &deadline) {
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
        return copyForward(deadline);
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
Error Surface::copyForward(const Deadline &deadline) {
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
                if (err.isOk() && _chain.count() == MaxPieces) err = _chain.run(remainingMs(deadline));
                if (err.isOk()) err = _chain.add(from, r.x, r.y, to, r.x, r.y, r.width, r.height);
                ++_stats.forwards;
                _stats.forwardPixels += r.area();
        };
        for (uint16_t row = 0; row <= _tileRows; ++row) {
                // This row's runs; past the last row none, so everything closes.
                Open   runs[MaxOpen];
                size_t count = 0;
                if (row < _tileRows) {
                        const uint32_t *bits = _forward + size_t(row) * _tileWords;
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
        _forwardPending = false;
        if (err.isOk()) err = _chain.run(remainingMs(deadline));
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
        uint16_t widest = 0;
        for (const Rect &rect : rects) {
                if (!fits(rect)) return Error::Invalid;
                if (rect.width > widest) widest = rect.width;
        }
        if (widest == 0) return Error::Ok;
        const Deadline deadline(timeoutMs);
        Error          err = prepare(deadline);
        if (err.isError()) return err;
        // One line of the color, the widest's length, sent once a row by each.
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
                for (uint32_t y = rect.y; y < rect.bottom(); y += MaxFillRows) {
                        const uint16_t rows = static_cast<uint16_t>(rect.bottom() - y < MaxFillRows ? rect.bottom() - y
                                                                                                    : MaxFillRows);
                        err = _chain.addFill(_line, to, rect.x, static_cast<uint16_t>(y), rect.width, rows);
                        if (err == Error::Full) {
                                err = _chain.run(remainingMs(deadline));
                                if (err.isOk())
                                        err = _chain.addFill(_line, to, rect.x, static_cast<uint16_t>(y), rect.width,
                                                             rows);
                        }
                        if (err.isError()) {
                                _chain.clear();
                                return err;
                        }
                }
                if (_buffering == Buffering::Double) markDamage(rect);
        }
        return _chain.run(remainingMs(deadline));
}

Error Surface::blit(const Rect &to, const uint8_t *src, size_t srcStride, unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        if (!fits(to) || src == nullptr || srcStride % BytesPerPixel != 0 ||
            srcStride < size_t(to.width) * BytesPerPixel || srcStride / BytesPerPixel > UINT16_MAX)
                return Error::Invalid;
        const Deadline deadline(timeoutMs);
        Error          err = prepare(deadline);
        if (err.isError()) return err;
        // The source out of the cache for the DMA, in pieces.
        const size_t bytes = srcStride * (to.height - 1) + size_t(to.width) * BytesPerPixel;
        for (size_t off = 0; off < bytes; off += WriteBackChunk)
                (void)esp_cache_msync(const_cast<uint8_t *>(src) + off,
                                      bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        const BlitChain::Picture from{src, static_cast<uint16_t>(srcStride / BytesPerPixel), to.height, BytesPerPixel};
        err = _chain.add(from, 0, 0, picture(back()), to.x, to.y, to.width, to.height);
        if (err.isOk()) err = _chain.run(remainingMs(deadline));
        if (err.isOk() && _buffering == Buffering::Double) markDamage(to);
        return err;
}

Result<uint32_t> Surface::present(unsigned int timeoutMs) {
        if (!isStarted()) return makeError<uint32_t>(Error::InvalidState);
        if (_buffering == Buffering::Single) {
                // Shown as it was drawn.
                _shown.store(++_presents, std::memory_order_release);
                return makeResult(_presents);
        }
        const Deadline deadline(timeoutMs);
        const Error    err = prepare(deadline);
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
void Surface::swappedFromIsr() {
        _front.store(_front.load(std::memory_order_relaxed) ^ 1, std::memory_order_release);
        _shown.store(_presents, std::memory_order_release);
        _asked.store(false, std::memory_order_release);
        _waiters.notifyFromIsr();
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
