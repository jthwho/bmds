/**
 * @file      surface.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/blitchain.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

class Deadline;
class Scanout;
struct ScanoutIsr;

/**
 * @brief A picture in PSRAM, RGB888 (B, G, R bytes), single or double
 *        buffered (`devplan/display.md` §5c, `devplan/windows.md` §0).
 *
 * Drawing is the same whatever the buffering and goes to the right buffer:
 * the only one, or with two the one not shown. A double-buffered surface
 * shows what was drawn when `present()` swaps it, at the start of the next
 * frame (the scanout makes the swap; without one it is made at once), and
 * the changes drawn since the last present are copied into the other buffer
 * before the next drawing, so both stay whole. The changes are kept as a
 * bitmap of tiles (`TileWidth` × `TileHeight`): marking is cheap whatever
 * is drawn, and the copy is of whole tiles. While a swap is pending,
 * drawing waits for it (a mailbox mode, which doesn't, is planned).
 *
 * The CPU never writes the buffers: drawing is by 2D-DMA, from internal
 * RAM. Rows written into PSRAM start and end on 4 bytes, so x and widths
 * are multiples of 4 pixels.
 *
 * @par Thread Safety
 * One drawing task at a time; `frames()` and `waitShown()` from any task.
 */
class Surface {
        public:
                /** @brief How many buffers. */
                enum class Buffering : uint8_t {
                        Single, ///< Drawn while shown (a drawing can tear): no memory beside the picture.
                        Double, ///< Drawn behind, shown by `present()` at a frame's start.
                };

                /** @brief Counts since `start()`. */
                struct Stats {
                                uint32_t presents = 0;      ///< Presents asked for.
                                uint32_t forwards = 0;      ///< Rectangles copied into the other buffer after swaps.
                                uint64_t forwardPixels = 0; ///< Their pixels.
                                uint64_t forwardUs = 0;     ///< Copying them.
                                uint32_t forwardUsMax = 0;
                                uint64_t waitUs = 0; ///< Drawing waiting for a pending swap.
                                uint32_t waitUsMax = 0;
                };

                /** @brief Bytes a pixel. */
                static constexpr size_t BytesPerPixel = 3;

                /** @brief The multiple of pixels x and widths are drawn on. */
                static constexpr uint16_t Align = 4;

                /** @brief A damage tile's size (double buffering). */
                static constexpr uint16_t TileWidth = 32;
                static constexpr uint16_t TileHeight = 16;

                Surface() = default;
                ~Surface() { stop(); }
                Surface(const Surface &) = delete;
                Surface &operator=(const Surface &) = delete;

                /**
                 * @brief Allocates the buffers (PSRAM, DMA-capable), black, and the
                 *        2D-DMA it draws with.
                 * @return `Ok`, `Invalid` (a zero size, a width not a multiple of
                 *         `Align`), `InvalidState` (started), `NoMem`, or the
                 *         driver's error.
                 * @blocking
                 */
                Error start(uint16_t width, uint16_t height, Buffering buffering);

                /** @brief Frees everything. Not while a scanout shows it. Idempotent. */
                void stop();

                /** @brief Returns true between `start()` and `stop()`. */
                bool isStarted() const { return _buffers[0] != nullptr; }

                uint16_t  width() const { return _width; }
                uint16_t  height() const { return _height; }
                Buffering buffering() const { return _buffering; }

                /** @brief Returns the bytes from one line to the next. */
                size_t stride() const { return size_t(_width) * BytesPerPixel; }

                /** @brief Returns the whole surface as a rectangle. */
                Rect bounds() const { return Rect{0, 0, _width, _height}; }

                /**
                 * @brief Fills @p rect with @p rgb (0xRRGGBB).
                 * @return `Ok`, `InvalidState` (not started), `Invalid` (outside,
                 *         or x or width not a multiple of `Align`), `Timeout`, or
                 *         the driver's error.
                 * @blocking Only while a swap is pending, and for the DMA.
                 */
                Error fill(const Rect &rect, uint32_t rgb, unsigned int timeoutMs);

                /**
                 * @brief Fills every rectangle of @p rects with @p rgb, as few
                 *        2D-DMA jobs as fit (up to 32 rectangles a job): the
                 *        cheap way to draw many.
                 * @return As `fill()` (nothing drawn if any rectangle is `Invalid`).
                 * @blocking As `fill()`.
                 */
                Error fill(Span<const Rect> rects, uint32_t rgb, unsigned int timeoutMs);

                /**
                 * @brief Copies @p to's size of RGB888 pixels from @p src (internal
                 *        RAM, @p srcStride bytes a line, a multiple of 3) into @p to.
                 *        Writes @p src back from the cache first.
                 * @return As `fill()`.
                 * @blocking As `fill()`.
                 */
                Error blit(const Rect &to, const uint8_t *src, size_t srcStride, unsigned int timeoutMs);

                /**
                 * @brief Shows what was drawn: with two buffers, swaps them at the
                 *        start of the next frame; with one, nothing (it is shown).
                 * @return The present's number, for `waitShown()`; `InvalidState`
                 *         (not started) or `Timeout` (a swap still pending, for now).
                 * @blocking Only while an earlier swap is pending.
                 */
                Result<uint32_t> present(unsigned int timeoutMs);

                /**
                 * @brief Waits until present @p number is on the screen.
                 * @return `Ok` or `Timeout`.
                 * @blocking
                 */
                Error waitShown(uint32_t number, unsigned int timeoutMs);

                /**
                 * @brief Waits until the scanout begins a frame after frame
                 *        @p seen (a value of `frames()`): a frame's pace.
                 * @return `Ok` or `Timeout` (no scan).
                 * @blocking
                 */
                Error waitFrame(uint32_t seen, unsigned int timeoutMs);

                /** @brief Returns the frames the scanout has begun with it. @nonblocking */
                uint32_t frames() const { return _frames.load(std::memory_order_acquire); }

                /** @brief Returns the presents shown so far. @nonblocking */
                uint32_t shownPresents() const { return _shown.load(std::memory_order_acquire); }

                /** @brief Returns true from a double-buffered present until its swap. @nonblocking */
                bool isSwapPending() const { return _asked.load(std::memory_order_acquire); }

                /** @brief Returns the counts. */
                Stats stats() const { return _stats; }

                /** @brief Returns the buffer being shown. */
                const uint8_t *shown() const { return _buffers[_front.load(std::memory_order_acquire)]; }

        private:
                friend class Scanout;
                friend struct ScanoutIsr;

                // For the scanout (its interrupt).
                const uint8_t *askedFromIsr() const { return _buffers[_front.load(std::memory_order_relaxed) ^ 1]; }
                void           swappedFromIsr();
                void           frameFromIsr() {
                        _frames.fetch_add(1, std::memory_order_release);
                        if (_waiters.hasWaiters()) _waiters.notifyFromIsr();
                        return;
                }
                void setScanning(bool scanning); // without a scan, a present swaps at once

                uint8_t           *back() const;
                BlitChain::Picture picture(const uint8_t *buffer) const;
                Error              prepare(const Deadline &deadline); // the swap done, the last changes copied
                void               swapNow();
                bool               fits(const Rect &rect) const;
                void               markDamage(const Rect &rect);
                Error              copyForward(const Deadline &deadline);
                template <typename Done> Error waitUntil(Done done, const Deadline &deadline);

                uint8_t              *_buffers[2] = {};
                uint8_t              *_line = nullptr; // a fill's line, internal RAM
                uint16_t              _width = 0;
                uint16_t              _height = 0;
                Buffering             _buffering = Buffering::Single;
                std::atomic<uint8_t>  _front{0};
                std::atomic<bool>     _asked{false}; // a swap pending
                std::atomic<bool>     _scanning{false};
                std::atomic<uint32_t> _frames{0};
                std::atomic<uint32_t> _shown{0};          // presents shown
                uint32_t              _presents = 0;      // presents asked for
                uint32_t             *_damage = nullptr;  // tiles drawn since the last present, a bit each
                uint32_t             *_forward = nullptr; // the last present's, still to copy into the other buffer
                uint16_t              _tileWords = 0;     // words a row of tiles
                uint16_t              _tileRows = 0;
                bool                  _forwardPending = false;
                WaitListOf<4>         _waiters; // woken by each swap and each frame
                BlitChain             _chain;
                Stats                 _stats;
};

BMDS_NAMESPACE_END
