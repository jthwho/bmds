/**
 * @file      framebuffer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

class Deadline;
class Scanout;

/**
 * @brief A double-buffered RGB565 picture in PSRAM (`devplan/display.md`
 *        §5b): the front buffer is shown, the back buffer is drawn into,
 *        and `present()` swaps them at the start of a frame.
 *
 * A `Scanout` started on it reads the front buffer and makes the swap as
 * it begins a frame (there is no vertical-blank interrupt to do it from).
 * Without a running scanout, a present swaps at once.
 *
 * The buffers are written by DMA, not through the CPU's cache: whoever
 * writes one with the CPU writes it back from the cache before presenting.
 * While a swap is pending the back buffer is about to be shown: nothing
 * draws into it until the swap is done (`isSwapPending()`).
 *
 * @par Thread Safety
 * One drawing client at a time; `present()` and `tryPresent()` from that
 * client's task. The scanout's side is internal.
 */
class Framebuffer {
        public:
                /** @brief Pixel formats. */
                enum class Format : uint8_t {
                        Rgb565, ///< 16 bits: R5 G6 B5, little-endian.
                };

                Framebuffer() = default;
                ~Framebuffer() { stop(); }
                Framebuffer(const Framebuffer &) = delete;
                Framebuffer &operator=(const Framebuffer &) = delete;

                /**
                 * @brief Allocates both buffers (PSRAM, DMA-capable), cleared to
                 *        black.
                 * @return `Ok`, `Invalid` (a zero size), `InvalidState` (started),
                 *         or `NoMem`.
                 * @blocking
                 */
                Error start(uint16_t width, uint16_t height);

                /** @brief Frees the buffers. Not while a scanout reads them. Idempotent. */
                void stop();

                /** @brief Returns true between `start()` and `stop()`. */
                bool isStarted() const { return _buffers[0] != nullptr; }

                /** @brief Returns the width in pixels. */
                uint16_t width() const { return _width; }

                /** @brief Returns the height in lines. */
                uint16_t height() const { return _height; }

                /** @brief Returns the bytes from one line to the next. */
                size_t stride() const { return size_t(_width) * 2; }

                /** @brief Returns the pixel format. */
                Format format() const { return Format::Rgb565; }

                /** @brief Returns the buffer being shown. */
                uint8_t *front() const { return _buffers[_front.load(std::memory_order_acquire)]; }

                /** @brief Returns the buffer to draw into. */
                uint8_t *back() const { return _buffers[_front.load(std::memory_order_acquire) ^ 1]; }

                /** @brief Returns true from a present until its swap. @nonblocking */
                bool isSwapPending() const { return _take.load(std::memory_order_acquire) != Idle; }

                /** @brief Returns the swaps made since `start()`. */
                uint32_t swaps() const { return _swaps.load(std::memory_order_acquire); }

                /**
                 * @brief Shows the back buffer from the start of the next frame,
                 *        and waits until it does (after any swap already pending).
                 * @return `Ok`, `InvalidState` (not started), or `Timeout`.
                 * @blocking
                 */
                Error present(unsigned int timeoutMs);

                /**
                 * @brief Asks for the swap and returns at once.
                 * @return `Ok`, `InvalidState` (not started), or `WouldBlock` (a
                 *         swap is already pending).
                 * @nonblocking
                 */
                Error tryPresent();

        private:
                friend class Scanout;

                // For the scanout's refill task, as it begins a frame: makes a
                // pending swap and returns the buffer the frame shows.
                uint8_t *beginFrame();

                // For the scanout: whether a scan reads the buffers (without one,
                // a present swaps at once).
                void setScanning(bool scanning);

                void  swap(); // the swap asked for, if any
                Error waitForSwap(uint32_t target, const Deadline &deadline);

                uint8_t             *_buffers[2] = {};
                uint16_t             _width = 0;
                uint16_t             _height = 0;
                std::atomic<uint8_t> _front{0};
                // The swap request: Idle, Asked (by a present), Swapping (being made).
                static constexpr uint8_t Idle = 0, Asked = 1, Swapping = 2;
                std::atomic<uint8_t>     _take{Idle};
                std::atomic<bool>        _scanning{false};
                std::atomic<uint32_t>    _swaps{0};
                Semaphore                _swapped{1, 0};
};

BMDS_NAMESPACE_END
