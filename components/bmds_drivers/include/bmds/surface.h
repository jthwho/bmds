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
 * RAM. Rows written into PSRAM start and end on 4 bytes, so copies take x
 * and widths in multiples of 4 pixels (`Align`); fills take any rectangle,
 * their edges' groups of 4 pixels read, patched and written back. Small
 * pictures with transparency (a pointer) are drawn the same way
 * (`patch()`).
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

                /**
                 * @brief Changes pixels in place (`patch()`): @p pixels is the
                 *        rectangle's first, B, G, R bytes, @p stride bytes a line.
                 */
                using Patcher = void (*)(void *arg, uint8_t *pixels, size_t stride);

                /** @brief A source picture's pixels (`startBlit()`). */
                enum class Source : uint8_t {
                        Bgr24,  ///< B, G, R: the surface's own.
                        Rgb565, ///< A little-endian 16-bit word, R in the top 5 bits: widened by the DMA.
                };

                /** @brief A copy inside the surface: @p from to (@p toX, @p toY). */
                struct Copy {
                                Rect     from;
                                uint16_t toX = 0;
                                uint16_t toY = 0;
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
                 * @brief Fills @p rect with @p rgb (0xRRGGBB). Sides off the 4-pixel
                 *        columns cost a read and a write of their groups (two
                 *        jobs for all of a call's).
                 * @return `Ok`, `InvalidState` (not started), `Invalid` (outside),
                 *         `Timeout`, or the driver's error.
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
                 * @brief Starts copying what of @p to lies in @p clip (rectangles
                 *        on 4-pixel columns) from @p src as in `blit()`, as one job,
                 *        and returns: @p src stays untouched until `wait()` or the
                 *        next drawing call returns.
                 * @return As `blit()`.
                 * @blocking Only while a swap is pending, or for an earlier job.
                 */
                Error startBlit(const Rect &to, const uint8_t *src, size_t srcStride, Span<const Rect> clip,
                                unsigned int timeoutMs) {
                        return startBlit(to, src, srcStride, clip, Source::Bgr24, timeoutMs);
                }

                /**
                 * @brief As `startBlit()`, from @p source pixels (@p srcStride bytes a
                 *        line, a multiple of the source's pixel). RGB565 is widened by
                 *        the 2D-DMA on the way (`devplan/games.md` §10): half the
                 *        source's bytes, no CPU a pixel.
                 * @return As `blit()`.
                 * @blocking Only while a swap is pending, or for an earlier job.
                 */
                Error startBlit(const Rect &to, const uint8_t *src, size_t srcStride, Span<const Rect> clip,
                                Source source, unsigned int timeoutMs) {
                        return startBlit(to, src, srcStride, clip, source, 1, 0, timeoutMs);
                }

                /**
                 * @brief As `startBlit()`, each of @p src's rows written @p repeat times
                 *        down: @p to's row i is @p src's row (@p first + i) / @p repeat
                 *        (@p first below @p repeat). The 2D-DMA reads a row again for
                 *        each: no CPU copies, a @p repeat'th of the source written back.
                 * @return As `blit()`.
                 * @blocking Only while a swap is pending, or for an earlier job.
                 */
                Error startBlit(const Rect &to, const uint8_t *src, size_t srcStride, Span<const Rect> clip,
                                Source source, uint16_t repeat, uint16_t first, unsigned int timeoutMs);

                /**
                 * @brief Says @p rects will be drawn whole (every pixel written) before the next
                 *        `present()`: what of the last present's changes they cover is not copied
                 *        forward (double buffering), only what they cover in part. A window redrawn
                 *        in strips calls it first, so no strip copies forward a tile the next
                 *        overwrites.
                 * @return As `fill()`.
                 * @blocking Only while a swap is pending, and for the copies.
                 */
                Error willDraw(Span<const Rect> rects, unsigned int timeoutMs);

                /**
                 * @brief Waits for a started copy.
                 * @return `Ok` or `Timeout`.
                 * @blocking
                 */
                Error wait(unsigned int timeoutMs);

                /**
                 * @brief Lets this surface batch its drawing (`beginBatch()`): its 2D-DMA chain
                 *        made for @p maxPieces pieces and @p maxRows fill rows a job, and
                 *        @p lineBytes of internal DMA RAM for fills' colour lines (at least a
                 *        row's bytes). For a surface drawn in many small pieces (the desktop's).
                 * @return `Ok`, `Invalid`, `NoMem` (the surface as it was), or the chain's error.
                 * @blocking A job under way.
                 */
                Error enableBatching(size_t maxPieces, size_t maxRows, size_t lineBytes);

                /**
                 * @brief From now on, fills and copies from pictures (`startBlit()`) are added to
                 *        one 2D-DMA job instead of each running its own (`devplan/windows.md`
                 *        §4n): the job runs when it is full, before anything that reads this
                 *        surface or a picture of the caller's, at `present()`, `wait()` and
                 *        `endBatch()`. A source given to `startBlit()` stays untouched until
                 *        `wait()`. Without `enableBatching()`, nothing changes.
                 */
                void beginBatch() {
                        if (_lines != nullptr) _batching = true;
                        return;
                }

                /**
                 * @brief Runs what the batch holds, waits for it, and ends batching.
                 * @return `Ok`, `Timeout`, or the chain's error.
                 * @blocking
                 */
                Error endBatch(unsigned int timeoutMs);

                /** @brief Returns the 2D-DMA jobs run since `start()` (both chains). */
                uint32_t jobs() const { return _chain.jobs() + _widen.jobs(); }

                /**
                 * @brief Reads @p rect (any x and width) into internal RAM, lets
                 *        @p patcher change it there, and writes it back: two 2D-DMA
                 *        jobs, for something small drawn over what is there (the
                 *        pointer). At most a surface line's bytes in all (its 4-pixel
                 *        groups, a row each).
                 * @return As `fill()`; `Invalid` too for a rectangle that is too big.
                 * @blocking As `fill()`.
                 */
                Error patch(const Rect &rect, Patcher patcher, void *arg, unsigned int timeoutMs);

                /**
                 * @brief Makes @p copies inside the surface, in order, a later one
                 *        reading what an earlier one wrote (up to 32 a job). All
                 *        on 4-pixel columns; a copy's source and destination may
                 *        overlap only moving up or left.
                 * @return As `fill()`.
                 * @blocking As `fill()`.
                 */
                Error copy(Span<const Copy> copies, unsigned int timeoutMs);

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

                /** @brief Returns true while a scanout shows it. @nonblocking */
                bool isScanning() const { return _scanning.load(std::memory_order_acquire); }

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
                // While the flash is written (the cache off), @p notify false: the waiters, whose
                // semaphores may be on stacks in PSRAM, are woken by notifyFromIsr() afterwards.
                const uint8_t *askedFromIsr() const { return _buffers[_front.load(std::memory_order_relaxed) ^ 1]; }
                void           swappedFromIsr(bool notify);
                void           frameFromIsr(bool notify) {
                        _frames.fetch_add(1, std::memory_order_release);
                        if (notify) notifyFromIsr();
                        return;
                }
                void notifyFromIsr() {
                        if (_waiters.hasWaiters()) _waiters.notifyFromIsr();
                        return;
                }
                void setScanning(bool scanning); // without a scan, a present swaps at once

                uint8_t           *back() const;
                BlitChain::Picture picture(const uint8_t *buffer) const;
                Error              prepare(const Deadline &deadline); // the swap done, the last changes copied
                Error prepareFor(const Deadline &deadline, Span<const Rect> writes, Span<const Rect> reads);
                void  swapNow();
                bool  fits(const Rect &rect) const;   // inside, on 4-pixel columns
                bool  inside(const Rect &rect) const; // inside
                Error fillEdges(Span<const Rect> rects, uint32_t rgb, const Deadline &deadline);
                void  markDamage(const Rect &rect);
                Error copyForward(const uint32_t *tiles, const Deadline &deadline);
                Error runChain(const Deadline &deadline); // the job written run, waited for; fill lines free
                struct FillLine;                          // below
                const FillLine *fillLine(uint32_t rgb, uint16_t width); // a batched fill's lines, or null (no room)
                Error           flushBatch(const Deadline &deadline);   // pending edges resolved, then runChain()
                Error resolveEdges(const Deadline &deadline); // pending edges read, patched, their writes added
                Error coverEdges(const Rect &rect, const Deadline &deadline); // pending edge rows under @p rect dropped
                bool  splitEdge(size_t i, uint16_t row); // pending edge @p i cut at @p row (false: no slot)
                Error addEdge(uint16_t x, uint16_t y, uint16_t rows, uint8_t from, uint8_t to, uint32_t rgb,
                              const Deadline &deadline); // a batched fill's edge group, pending
                template <typename Done> Error waitUntil(Done done, const Deadline &deadline);

                uint8_t              *_buffers[2] = {};
                uint8_t              *_line = nullptr;  // a fill's line, internal RAM
                uint8_t              *_edges = nullptr; // fills' edge groups being patched, internal RAM
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
                uint32_t             *_select = nullptr;  // prepareFor()'s tiles to copy now
                uint16_t              _tileWords = 0;     // words a row of tiles
                uint16_t              _tileRows = 0;
                bool                  _forwardPending = false;
                WaitListOf<4>         _waiters; // woken by each swap and each frame
                BlitChain             _chain;
                BlitChain             _widen; // RGB565 sources (started on first use)
                Stats                 _stats;
                // Batching (enableBatching()): fills' colour lines, kept until the job runs.
                struct FillLine {
                                uint32_t rgb;
                                uint16_t width;
                                uint16_t rows; // of the colour, one after the other
                                uint8_t *line;
                };
                static constexpr size_t MaxFillLines = 8;
                uint8_t                *_lines = nullptr; // internal DMA RAM
                size_t                  _linesBytes = 0;
                size_t                  _linesUsed = 0;
                FillLine                _fillLines[MaxFillLines] = {};
                uint8_t                 _fillLineCount = 0;
                uint8_t                 _fillLineNext = 0; // the slot replaced next when all are used
                bool                    _batching = false;
                // A batch's edge groups (fills' sides off the 4-pixel columns), read and patched
                // together when something meets them (resolveEdges()); a pixel's colour each.
                struct PendingEdge {
                                uint16_t x; // the group's first column
                                uint16_t y;
                                uint16_t rows;
                                uint8_t  mask; // the group's pixels set (bit i: pixel i)
                                uint32_t rgb[Align];
                };
                static constexpr size_t MaxPendingEdges = 64;
                PendingEdge             _edgesPending[MaxPendingEdges] = {};
                uint8_t                 _edgeCount = 0;
                uint16_t                _edgeRows = 0; // the rows they take in _line
};

BMDS_NAMESPACE_END
