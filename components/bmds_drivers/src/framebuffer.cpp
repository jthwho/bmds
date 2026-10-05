/**
 * @file      framebuffer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The double-buffered PSRAM picture (devplan/display.md §5b). The swap is a
 * flag the scanout's refill task acts on as it begins a frame; a counter of
 * swaps lets a waiter tell its own swap from an earlier one.
 */

#include <cstring>
#include <esp_cache.h>
#include <bmds/deadline.h>
#include <bmds/framebuffer.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t Align = 128;                // the L2 cache line; the 2D-DMA's PSRAM alignment
        constexpr size_t WriteBackChunk = 64 * 1024; // a cache operation runs with interrupts off

} // namespace

Error Framebuffer::start(uint16_t width, uint16_t height) {
        if (isStarted()) return Error::InvalidState;
        if (width == 0 || height == 0) return Error::Invalid;
        const size_t bytes = size_t(width) * 2 * height;
        for (uint8_t *&buffer : _buffers) {
                buffer = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, bytes, Align));
                if (buffer == nullptr) {
                        stop();
                        return Error::NoMem;
                }
                std::memset(buffer, 0, bytes);
                for (size_t off = 0; off < bytes; off += WriteBackChunk)
                        (void)esp_cache_msync(buffer + off, bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        _width = width;
        _height = height;
        _front.store(0, std::memory_order_release);
        _take.store(Idle, std::memory_order_release);
        _swaps.store(0, std::memory_order_release);
        while (_swapped.tryAcquire().isOk()) {}
        return Error::Ok;
}

void Framebuffer::stop() {
        for (uint8_t *&buffer : _buffers) {
                Memory::free(buffer);
                buffer = nullptr;
        }
        _width = 0;
        _height = 0;
        return;
}

Error Framebuffer::present(unsigned int timeoutMs) {
        if (!isStarted()) return Error::InvalidState;
        const Deadline deadline(timeoutMs);
        // A swap already pending (from `tryPresent()`) goes first.
        if (isSwapPending()) {
                const Error err = waitForSwap(_swaps.load(std::memory_order_acquire) + 1, deadline);
                if (err.isError()) return err;
        }
        const uint32_t target = _swaps.load(std::memory_order_acquire) + 1;
        _take.store(Asked, std::memory_order_release);
        // No scan (or it just stopped): nothing will make the swap but us.
        if (!_scanning.load(std::memory_order_acquire)) swap();
        return waitForSwap(target, deadline);
}

Error Framebuffer::tryPresent() {
        if (!isStarted()) return Error::InvalidState;
        uint8_t idle = Idle;
        if (!_take.compare_exchange_strong(idle, Asked, std::memory_order_acq_rel)) return Error::WouldBlock;
        if (!_scanning.load(std::memory_order_acquire)) swap();
        return Error::Ok;
}

uint8_t *Framebuffer::beginFrame() {
        swap();
        return _buffers[_front.load(std::memory_order_acquire)];
}

void Framebuffer::setScanning(bool scanning) {
        _scanning.store(scanning, std::memory_order_release);
        // No scan to wait for: a pending swap is made now.
        if (!scanning) swap();
        return;
}

// Makes the swap asked for, if one is: whoever clears the request makes it,
// so a scan stopping and a present racing it swap once.
void Framebuffer::swap() {
        uint8_t asked = Asked;
        if (!_take.compare_exchange_strong(asked, Swapping, std::memory_order_acq_rel)) return;
        _front.store(_front.load(std::memory_order_relaxed) ^ 1, std::memory_order_release);
        _swaps.fetch_add(1, std::memory_order_acq_rel);
        _take.store(Idle, std::memory_order_release); // pending until the front has moved
        _swapped.release();
        return;
}

// Waits until the swap count reaches @p target. A release left from an
// earlier swap just costs one more look at the count.
Error Framebuffer::waitForSwap(uint32_t target, const Deadline &deadline) {
        while (static_cast<int32_t>(_swaps.load(std::memory_order_acquire) - target) < 0) {
                if (_swapped.acquire(deadline).isError()) {
                        if (static_cast<int32_t>(_swaps.load(std::memory_order_acquire) - target) >= 0) break;
                        return Error::Timeout;
                }
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
