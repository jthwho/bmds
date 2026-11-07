/**
 * @file      swapchain.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A surface descriptor's swap chain (devplan/games.md §11, devplan/guiprograms.md §11 3c): its
 * buffers, their states, and the 2D-DMA's blits into them, for a program's surface and a GUI
 * canvas's alike.
 */

#include <bmds/swapchain.h>

#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_memory_utils.h>
#include <bmds/blitchain.h>
#include <bmds/deadline.h>
#include <bmds/display.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/scanout.h>
#include <bmds/spinlock.h>
#include <bmds/surface.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t       BufferAlign = 128; // the L2 cache line; the 2D-DMA's PSRAM alignment
        constexpr size_t       WriteBackChunk = 64 * 1024;
        constexpr unsigned int WaitSliceMs = 20; // a frame wait looks again this often (the scanned surface may change)
        void                   unrefBlitter();   // blit()'s shared blitter (below)

        unsigned int remainingMs(const Deadline &deadline) {
                const int64_t us = deadline.remainingUs();
                if (us == Deadline::Forever) return WaitForever;
                return us <= 0 ? 1 : static_cast<unsigned int>((us + 999) / 1000);
        }

        Display *display0() {
                auto [d, err] = Display::display(0);
                return err.isOk() && d->isRunning() ? d : nullptr;
        }

} // namespace

size_t SwapChain::bytesPerPixel(Format format) {
        switch (format) {
                case Format::L8: return 1;
                case Format::Rgb565: return 2;
                default: return 3;
        }
}

uint32_t SwapChain::frames() {
        Display *d = display0();
        return d != nullptr ? d->scanout().stats().frames : 0;
}

SwapChain::FrameTime SwapChain::frameTime() {
        Display *d = display0();
        return d != nullptr ? d->scanout().frameTime() : FrameTime();
}

SwapChain::~SwapChain() {
        for (uint8_t *&b : _buffers) {
                Memory::free(b);
                b = nullptr;
        }
        if (_blitting.load(std::memory_order_acquire)) unrefBlitter();
}

Error SwapChain::makeBuffers(uint16_t width, uint16_t height, Format format, uint8_t count, MemSpace space) {
        if (width == 0 || height == 0 || count < 1 || count > MaxBuffers || _bufferCount != 0) return Error::Invalid;
        _width = width;
        _height = height;
        _format = format;
        _stride = size_t(width) * bytesPerPixel(format);
        _bufferBytes = (_stride * height + BufferAlign - 1) / BufferAlign * BufferAlign;
        for (uint8_t i = 0; i < count; ++i) {
                _buffers[i] = static_cast<uint8_t *>(Memory::allocAligned(space, _bufferBytes, BufferAlign));
                if (_buffers[i] == nullptr) return Error::NoMem;
                std::memset(_buffers[i], 0, _bufferBytes);
                (void)esp_cache_msync(_buffers[i], _bufferBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                _states[i] = State::Free;
        }
        _bufferCount = count;
        return Error::Ok;
}

// ---- The swap chain

Result<uint8_t> SwapChain::tryAcquire() {
        Mutex::Locker locker(_lock, WaitForever);
        for (uint8_t i = 0; i < _bufferCount; ++i) {
                if (_states[i] != State::Free) continue;
                _states[i] = State::Program;
                return makeResult(i);
        }
        return makeError<uint8_t>(Error::WouldBlock);
}

Result<uint8_t> SwapChain::acquire(unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        for (;;) {
                auto r = tryAcquire();
                if (r.error() != Error::WouldBlock) return r;
                const Error err = Interrupt::waitCurrent(_freed, deadline);
                if (err.isError()) return makeError<uint8_t>(err);
        }
}

void SwapChain::writeBack(uint8_t index) const {
        // In pieces: a cache operation runs with interrupts off.
        const size_t bytes = _stride * _height;
        for (size_t off = 0; off < bytes; off += WriteBackChunk)
                (void)esp_cache_msync(_buffers[index] + off,
                                      bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        return;
}

bool SwapChain::takePresentLocked(uint8_t index) {
        bool freed = false;
        if (_latest >= 0) {
                // The one it replaces is free, unless it is being read (then when that ends).
                if (_latest != _reading) {
                        _states[_latest] = State::Free;
                        freed = true;
                }
                if (!_latestDrawn) ++_dropped; // replaced before it was ever shown
        }
        _states[index] = State::Latest;
        _latest = int8_t(index);
        _latestDrawn = false;
        ++_presents;
        return freed;
}

int8_t SwapChain::beginReadLocked() {
        _reading = _latest;
        if (_latest >= 0) _latestDrawn = true; // shown, even if replaced while read
        return _latest;
}

// Done reading presented buffer @p index: free if it was replaced meanwhile.
void SwapChain::release(int8_t index) {
        bool freed = false;
        {
                Mutex::Locker locker(_lock, WaitForever);
                _reading = -1;
                if (index >= 0 && index != _latest && _states[index] == State::Latest) {
                        _states[index] = State::Free;
                        freed = true;
                }
        }
        if (freed) _freed.release();
        return;
}

Error SwapChain::setPalette(size_t, Span<const uint32_t>) {
        return Error::NotSupported;
}

Error SwapChain::setTitle(StringView) {
        return Error::NotSupported;
}

Error SwapChain::setPointerMode(PointerMode) {
        return Error::NotSupported;
}

// ---- The 2D-DMA for programs (devplan/games.md §25)

namespace {

        constexpr size_t       CacheLine = 128;    // the L2's: what an invalidate drops whole
        constexpr size_t       BlitFillRows = 256; // a job's fill rows (a transmit descriptor each)
        constexpr uint16_t     BlitLinePixels = 1280;
        constexpr size_t       BlitLineBytes = BlitLinePixels * 3; // a fill's line, the widest pixel
        constexpr size_t       MergeGap = 16 * 1024;               // rows' ranges this close: one cache operation
        constexpr unsigned int BlitTimeoutMs = 100;

        // Every program surface's: one chain (its descriptors in internal DMA RAM) and a fill's
        // line, made on the first blit and freed when the last surface that blitted closes (about
        // 19 KB of internal DMA RAM: kept, it took full screen's from the next game).
        struct Blitter {
                        Mutex     lock;
                        BlitChain chain;
                        uint8_t  *line = nullptr;
                        uint32_t  linePixel = 0; // what the line holds
                        uint8_t   lineBpp = 0;
        };

        constinit SpinLock sBlitterLock; // sBlitter and sBlitterUsers
        Blitter           *sBlitter = nullptr;
        uint32_t           sBlitterUsers = 0;

        void destroyBlitter(Blitter *b, void *memory) {
                b->chain.stop();
                Memory::free(b->line);
                b->~Blitter();
                Memory::free(memory);
                return;
        }

        // A reference to the blitter, made if there is none; nullptr without memory for it.
        Blitter *refBlitter() {
                {
                        SpinLock::Locker locker(sBlitterLock);
                        if (sBlitter != nullptr) {
                                ++sBlitterUsers;
                                return sBlitter;
                        }
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(Blitter));
                if (memory == nullptr) return nullptr;
                Blitter *b = new (memory) Blitter();
                b->line = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, BlitLineBytes, 64));
                if (b->line == nullptr ||
                    b->chain.start(BlitChain::Conversion::None, SwapChain::MaxBlitPieces, BlitFillRows).isError()) {
                        destroyBlitter(b, memory);
                        return nullptr;
                }
                Blitter *made = nullptr;
                {
                        SpinLock::Locker locker(sBlitterLock);
                        if (sBlitter == nullptr) sBlitter = b;
                        made = sBlitter;
                        ++sBlitterUsers;
                }
                if (made != b) destroyBlitter(b, memory); // another thread made one first: ours goes
                return made;
        }

        // A reference given back: the last frees it (no blit runs: each user's are over).
        void unrefBlitter() {
                Blitter *last = nullptr;
                {
                        SpinLock::Locker locker(sBlitterLock);
                        if (sBlitterUsers > 0 && --sBlitterUsers == 0) {
                                last = sBlitter;
                                sBlitter = nullptr;
                        }
                }
                if (last != nullptr) destroyBlitter(last, last);
                return;
        }

        // Bytes [start, end) of a picture: the cache operations' ranges.
        struct Span2 {
                        uintptr_t start, end;
        };

        // A buffer's ranges written back from the cache and invalidated (before a DMA writes them),
        // or invalidated (after), in pieces: a cache operation runs with interrupts off. Both on
        // the same whole L2 lines (buffers are aligned and padded to them). The L1's lines are half
        // as long: a write-back of the exact bytes left the line's other half dirty in the L1, and
        // the invalidate after, on whole lines, dropped what the CPU had drawn there (a 64-byte
        // stretch of a row lost, games.md §25g).
        void syncRanges(const Span2 *ranges, size_t count, bool after) {
                for (size_t i = 0; i < count; ++i) {
                        const uintptr_t a = ranges[i].start & ~uintptr_t(CacheLine - 1);
                        const uintptr_t b = (ranges[i].end + CacheLine - 1) & ~uintptr_t(CacheLine - 1);
                        for (uintptr_t at = a; at < b; at += WriteBackChunk) {
                                const size_t n = b - at < WriteBackChunk ? b - at : WriteBackChunk;
                                (void)esp_cache_msync(reinterpret_cast<void *>(at), n,
                                                      after ? ESP_CACHE_MSYNC_FLAG_DIR_M2C
                                                            : ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                                                      ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                        }
                }
                return;
        }

        // Adds [start, end) to @p ranges (sorted by start), merged with those it is near.
        void addRange(Span2 *ranges, size_t &count, Span2 r) {
                size_t i = count;
                while (i > 0 && ranges[i - 1].start > r.start) {
                        ranges[i] = ranges[i - 1];
                        --i;
                }
                ranges[i] = r;
                ++count;
                size_t out = 0;
                for (size_t k = 1; k < count; ++k) {
                        if (ranges[k].start <= ranges[out].end + MergeGap) {
                                if (ranges[k].end > ranges[out].end) ranges[out].end = ranges[k].end;
                        } else {
                                ranges[++out] = ranges[k];
                        }
                }
                count = out + 1;
                return;
        }

} // namespace

Error SwapChain::setImage(uint8_t slot, const Image &image) {
        if (slot >= MaxImages) return Error::Invalid;
        Image kept;
        if (image.pixels != nullptr) {
                const size_t bpp = bytesPerPixel(_format);
                if (image.width == 0 || image.height == 0 || image.stride % bpp != 0 ||
                    image.stride < image.width * bpp || image.stride / bpp > 0x3fff)
                        return Error::Invalid;
                const size_t   bytes = size_t(image.stride) * (image.height - 1u) + image.width * bpp;
                const uint8_t *last = image.pixels + bytes - 1;
                // PSRAM: the heap's, or the window's (the 2D-DMA reads both: devplan/psramwindow.md §2b).
                const bool     psram = Memory::isExternal(image.pixels, bytes);
                const bool     internal = esp_ptr_dma_capable(image.pixels) && esp_ptr_dma_capable(last);
                if (!psram && !internal) return Error::Invalid;
                // Written back only: nothing invalidated, so the program's memory beside it is untouched.
                for (size_t off = 0; off < bytes; off += WriteBackChunk)
                        (void)esp_cache_msync(const_cast<uint8_t *>(image.pixels) + off,
                                              bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                kept = image;
        }
        Mutex::Locker locker(_lock, WaitForever);
        _images[slot] = kept;
        return Error::Ok;
}

Error SwapChain::blit(uint8_t index, BlitSource source, uint32_t from, Span<const Piece> pieces) {
        if (index >= _bufferCount || pieces.size() == 0 || pieces.size() > MaxBlitPieces) return Error::Invalid;
        const size_t             bpp = bytesPerPixel(_format);
        const uint16_t           lineWidth = uint16_t(_stride / bpp);
        const BlitChain::Picture to{_buffers[index], lineWidth, _height, uint8_t(bpp)};
        BlitChain::Picture       src;
        int32_t                  srcWidth = 0, srcHeight = 0;
        bool                     srcHeld = false; // a buffer the program draws: its writes not yet written back
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (_states[index] != State::Program) return Error::Invalid;
                if (source == BlitSource::Buffer) {
                        if (from >= _bufferCount || from == index) return Error::Invalid;
                        src = BlitChain::Picture{_buffers[from], lineWidth, _height, uint8_t(bpp)};
                        srcWidth = _width;
                        srcHeight = _height;
                        srcHeld = _states[from] == State::Program;
                } else if (source == BlitSource::Image) {
                        if (from >= MaxImages || _images[from].pixels == nullptr) return Error::Invalid;
                        const Image &im = _images[from];
                        src = BlitChain::Picture{im.pixels, uint16_t(im.stride / bpp), im.height, uint8_t(bpp)};
                        srcWidth = im.width;
                        srcHeight = im.height;
                }
        }
        // The surface's reference, taken on its first blit and given back when it closes.
        if (!_blitting.load(std::memory_order_acquire)) {
                if (refBlitter() == nullptr) return Error::NoMem;
                if (_blitting.exchange(true, std::memory_order_acq_rel)) unrefBlitter(); // another thread's first
        }
        Blitter *b = sBlitter; // ours held: neither freed nor replaced
        // The pieces cut to the buffer and the source; the cache's ranges of both.
        Piece  cut[MaxBlitPieces];
        size_t n = 0;
        Span2  dst[MaxBlitPieces], srcRanges[MaxBlitPieces];
        size_t dstCount = 0, srcCount = 0;
        for (const Piece &p : pieces) {
                int64_t x0 = p.x, y0 = p.y, x1 = int64_t(p.x) + p.width, y1 = int64_t(p.y) + p.height;
                int64_t fx = p.fromX, fy = p.fromY;
                // Cut to the buffer, the source moving with it.
                if (x0 < 0) fx -= x0, x0 = 0;
                if (y0 < 0) fy -= y0, y0 = 0;
                if (x1 > _width) x1 = _width;
                if (y1 > _height) y1 = _height;
                if (source != BlitSource::Fill) { // and to the source
                        if (fx < 0) x0 -= fx, fx = 0;
                        if (fy < 0) y0 -= fy, fy = 0;
                        if (fx + (x1 - x0) > srcWidth) x1 = x0 + (srcWidth - fx);
                        if (fy + (y1 - y0) > srcHeight) y1 = y0 + (srcHeight - fy);
                }
                if (x1 <= x0 || y1 <= y0) continue;
                Piece &c = cut[n++];
                c.x = int32_t(x0);
                c.y = int32_t(y0);
                c.width = uint32_t(x1 - x0);
                c.height = uint32_t(y1 - y0);
                c.fromX = int32_t(fx);
                c.fromY = int32_t(fy);
                const uintptr_t base = reinterpret_cast<uintptr_t>(_buffers[index]);
                addRange(dst, dstCount,
                         Span2{base + size_t(c.y) * _stride + size_t(c.x) * bpp,
                               base + size_t(c.y + c.height - 1) * _stride + size_t(c.x + c.width) * bpp});
                if (srcHeld) {
                        const uintptr_t sb = reinterpret_cast<uintptr_t>(src.data);
                        addRange(
                                srcRanges, srcCount,
                                Span2{sb + size_t(c.fromY) * _stride + size_t(c.fromX) * bpp,
                                      sb + size_t(c.fromY + c.height - 1) * _stride + size_t(c.fromX + c.width) * bpp});
                }
        }
        if (n == 0) return Error::Ok;
        Mutex::Locker locker(b->lock, WaitForever);
        if (source == BlitSource::Fill && (b->linePixel != from || b->lineBpp != bpp)) {
                for (size_t i = 0; i < BlitLinePixels; ++i)
                        for (size_t k = 0; k < bpp; ++k) b->line[i * bpp + k] = uint8_t(from >> (8 * k));
                b->linePixel = from;
                b->lineBpp = uint8_t(bpp);
                // Internal RAM is cached too: the 2D-DMA reads memory, not the cache.
                (void)esp_cache_msync(b->line, BlitLineBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        syncRanges(srcRanges, srcCount, false);
        syncRanges(dst, dstCount, false);
        Error err = Error::Ok;
        for (size_t i = 0; i < n && err.isOk(); ++i) {
                const Piece &c = cut[i];
                // A fill as strips no wider than the line and no taller than a job's rows.
                for (uint32_t y = 0; y < c.height && err.isOk();) {
                        const uint32_t rows = source == BlitSource::Fill
                                                      ? (c.height - y < BlitFillRows ? c.height - y : BlitFillRows)
                                                      : c.height;
                        for (uint32_t x = 0; x < c.width && err.isOk();) {
                                const uint32_t cols =
                                        source == BlitSource::Fill
                                                ? (c.width - x < BlitLinePixels ? c.width - x : BlitLinePixels)
                                                : c.width;
                                for (int attempt = 0; attempt < 2; ++attempt) {
                                        err = source == BlitSource::Fill
                                                      ? b->chain.addFill(b->line, to, uint16_t(c.x + x),
                                                                         uint16_t(c.y + y), uint16_t(cols),
                                                                         uint16_t(rows))
                                                      : b->chain.add(src, uint16_t(c.fromX), uint16_t(c.fromY), to,
                                                                     uint16_t(c.x), uint16_t(c.y), uint16_t(c.width),
                                                                     uint16_t(c.height));
                                        if (err != Error::Full) break;
                                        err = b->chain.run(BlitTimeoutMs); // full: this job first
                                        if (err.isError()) break;
                                }
                                x += cols;
                        }
                        y += rows;
                }
        }
        if (err.isOk()) err = b->chain.run(BlitTimeoutMs);
        b->chain.clear();
        // Lines the CPU may have fetched meanwhile hold the old pixels: dropped.
        syncRanges(dst, dstCount, true);
        return err;
}


Error SwapChain::waitFrame(uint32_t frame, unsigned int timeoutMs) {
        Display *d = display0();
        if (d == nullptr) return Error::InvalidState;
        const Deadline deadline(timeoutMs);
        for (;;) {
                if (int32_t(frames() - frame) >= 0) return Error::Ok;
                // The surface scanned now (it changes with full screen): its frames, a slice at a time.
                Surface *scanned = d->scanout().surface();
                if (scanned == nullptr) return Error::InvalidState;
                const unsigned int left = remainingMs(deadline);
                const unsigned int slice = left == WaitForever || left > WaitSliceMs ? WaitSliceMs : left;
                const Error        err = scanned->waitFrame(scanned->frames(), slice);
                if (err.isError() && err != Error::Timeout) return err;
                if (err == Error::Timeout && !deadline.isForever() && deadline.remainingUs() <= 0)
                        return Error::Timeout;
        }
}


BMDS_NAMESPACE_END
