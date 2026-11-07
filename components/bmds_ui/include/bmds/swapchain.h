/**
 * @file      swapchain.h
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
#include <bmds/memspace.h>
#include <bmds/mutex.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/scanout.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/stream.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A surface descriptor's buffers (`devplan/games.md` §11, `devplan/guiprograms.md` §11
 *        3c): what the loader's surface calls take. A program's own surface (`ProgramSurface`)
 *        and a GUI program's canvas both are one.
 *
 * The buffers are a swap chain: the program takes a free one (`acquire()`), draws into it with
 * the CPU or the 2D-DMA (`blit()`), and presents it. The newest presented one is shown and kept
 * (whatever shows it is redrawn from it); the one it replaced is free again once nothing reads
 * it (`beginRead()`, `release()`). What showing means is the subclass's: `present()`.
 *
 * @par Thread Safety
 * Thread-safe: the program's threads and whatever shows the buffers.
 */
class SwapChain : public Stream {
        public:
                /** @brief Pixel formats: the ABI's `BMDS_PIXEL_...` values. */
                enum class Format : uint8_t {
                        Bgr24 = 0,  ///< B, G, R.
                        Rgb565 = 3, ///< Little-endian, R in the top 5 bits.
                        L8 = 5,     ///< An index into the palette of 256.
                };

                /** @brief Pointer modes (`setPointerMode()`): the ABI's. */
                enum class PointerMode : uint8_t {
                        Normal = 0,
                        Hidden = 1,
                        Relative = 2,
                };

                /** @brief Its state (`info()`). */
                struct Info {
                                bool     fullScreen = false;
                                bool     active = false;
                                bool     shown = false; ///< Has a window (or the screen).
                                uint16_t scaleX16 = 16; ///< The scale across, in 16ths.
                                uint16_t scaleY16 = 16; ///< Down.
                                Rect     area;          ///< The picture on the screen.
                                uint32_t frames = 0;    ///< The display's frames so far.
                                uint32_t presents = 0;
                                uint32_t dropped = 0; ///< Presents replaced before shown.
                                uint16_t width = 0;   ///< The surface's own size (its buffers').
                                uint16_t height = 0;
                };

                /** @brief A picture of the program's that `blit()` may copy from (`BMDS_SURFACE_IMAGE`). */
                struct Image {
                                const uint8_t *pixels = nullptr; ///< Null: none.
                                uint16_t       width = 0;
                                uint16_t       height = 0;
                                uint32_t       stride = 0; ///< Bytes a row.
                };

                /** @brief Where a `blit()` takes its pixels. */
                enum class BlitSource : uint8_t {
                        Buffer, ///< Another of its buffers.
                        Image,  ///< An image slot.
                        Fill,   ///< One pixel value.
                };

                /** @brief A `blit()`'s rectangle: where in the buffer, and where in the source. */
                struct Piece {
                                int32_t  x = 0, y = 0;
                                uint32_t width = 0, height = 0;
                                int32_t  fromX = 0, fromY = 0;
                };

                static constexpr uint8_t MaxBuffers = 3;     ///< Buffers at most.
                static constexpr uint8_t MaxImages = 4;      ///< `BMDS_SURFACE_IMAGES`.
                static constexpr size_t  MaxBlitPieces = 64; ///< `BMDS_SURFACE_BLIT_MAX`.

                /** @brief Returns the bytes a pixel of @p format takes. */
                static size_t bytesPerPixel(Format format);

                uint16_t width() const { return _width; }
                uint16_t height() const { return _height; }
                Format   format() const { return _format; }
                uint8_t  bufferCount() const { return _bufferCount; }

                /** @brief Returns buffer @p index's first pixel, or null. */
                uint8_t *buffer(uint8_t index) const { return index < _bufferCount ? _buffers[index] : nullptr; }

                /** @brief Returns the bytes from a row to the next. */
                size_t stride() const { return _stride; }

                /**
                 * @brief Takes a free buffer for the program to draw.
                 * @return Its index, or `Timeout` (`WaitForever`: never), `Interrupted`.
                 * @blocking
                 */
                Result<uint8_t> acquire(unsigned int timeoutMs);

                /**
                 * @brief Takes a free buffer if there is one.
                 * @return Its index, or `WouldBlock`.
                 * @nonblocking
                 */
                Result<uint8_t> tryAcquire();

                /**
                 * @brief Shows buffer @p index (the program's, from `acquire()`), which differs
                 *        from the last present in @p damage only (empty: all of it). The buffer
                 *        is whole all the same: whatever shows it may be redrawn from it.
                 * @return The display's frame it will be shown in, or `Invalid` (not the program's).
                 * @nonblocking
                 */
                virtual Result<uint32_t> present(uint8_t index, Span<const Rect> damage) = 0;

                /** @brief Sets palette entries (0xRRGGBB) from @p first. @return `NotSupported` here. */
                virtual Error setPalette(size_t first, Span<const uint32_t> rgb);

                /** @brief Sets its window's title. @return `NotSupported` here. */
                virtual Error setTitle(StringView title);

                /** @brief Sets what the pointer does over it. @return `NotSupported` here. */
                virtual Error setPointerMode(PointerMode mode);

                /** @brief Returns its state. */
                virtual Info info() = 0;

                /**
                 * @brief Puts @p image in slot @p slot (null pixels: empties it), in the surface's
                 *        format, and writes it back from the CPU's cache. The caller has checked
                 *        that it is the program's memory (`devplan/games.md` §25).
                 * @return `Ok`, or `Invalid` (the slot, its size, its row, memory the 2D-DMA can't read).
                 */
                Error setImage(uint8_t slot, const Image &image);

                /**
                 * @brief Copies (or fills) @p pieces of buffer @p index, which the program holds,
                 *        by the 2D-DMA in as few jobs as fit, and returns when they are written.
                 *        The rows they cover are written back from the cache before and
                 *        invalidated after, so what the program drew beside them stays.
                 * @return `Ok`, `Invalid` (a buffer not held, the source the buffer itself, an empty
                 *         slot, too many pieces), `NoMem`, `Timeout`, or the 2D-DMA's error.
                 * @blocking
                 */
                Error blit(uint8_t index, BlitSource source, uint32_t from, Span<const Piece> pieces);

                /**
                 * @brief Waits until the display has begun frame @p frame (`Info::frames`).
                 * @return `Ok`, `Timeout`, `Interrupted`, `InvalidState` (no display).
                 * @blocking
                 */
                Error waitFrame(uint32_t frame, unsigned int timeoutMs);

                /** @brief Returns the display's frames so far (the scan's: every surface's). */
                static uint32_t frames();

                /** @brief When the display's last frame began, and a frame's length (`Scanout::frameTime()`). */
                using FrameTime = Scanout::FrameTime;
                static FrameTime frameTime();

                // Stream
                SwapChain *swapChain() override { return this; }

        protected:
                enum class State : uint8_t {
                        Free,
                        Program,
                        Latest
                };

                SwapChain() = default;
                ~SwapChain();

                /**
                 * @brief Makes @p count buffers of @p width × @p height in @p format, black, in
                 *        @p space (aligned and padded to the cache's lines, for the DMA).
                 * @return `Ok`, `Invalid`, `NoMem`.
                 */
                Error makeBuffers(uint16_t width, uint16_t height, Format format, uint8_t count, MemSpace space);

                /** @brief Writes buffer @p index back from the CPU's cache (a present's: for the DMA). */
                void writeBack(uint8_t index) const;

                /**
                 * @brief Makes buffer @p index the newest presented: the one it replaces free
                 *        (unless being read), a present replaced before it was read counted.
                 *        `_lock` held.
                 * @return True if a buffer was freed: release `_freed` once the lock is let go.
                 */
                bool takePresentLocked(uint8_t index);

                /** @brief Starts reading the newest presented buffer. `_lock` held. @return Its index, or -1. */
                int8_t beginReadLocked();

                /** @brief Done reading presented buffer @p index: free if it was replaced meanwhile. */
                void release(int8_t index);

                uint16_t  _width = 0;
                uint16_t  _height = 0;
                Format    _format = Format::Bgr24;
                uint8_t   _bufferCount = 0;
                uint8_t  *_buffers[MaxBuffers] = {};
                size_t    _stride = 0;
                size_t    _bufferBytes = 0;
                Mutex     _lock; // the buffers' states, the images, and the subclass's own
                State     _states[MaxBuffers] = {};
                int8_t    _latest = -1;  // the newest presented
                int8_t    _reading = -1; // being read by whatever shows it
                bool      _latestDrawn = false;
                Semaphore _freed{1, 0};
                uint32_t  _presents = 0;
                uint32_t  _dropped = 0;

        private:
                Image             _images[MaxImages]; // blit()'s sources (under _lock)
                std::atomic<bool> _blitting{false};   // holds a reference to the shared blitter (blit())
};

BMDS_NAMESPACE_END
