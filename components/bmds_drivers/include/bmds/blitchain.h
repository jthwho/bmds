/**
 * @file      blitchain.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Copies a list of rectangles between pictures as one 2D-DMA job
 *        (`devplan/windows.md` §4b): a chain of transmit descriptors (a
 *        block of a source picture each) and one of receive descriptors (a
 *        block of a destination picture each), so the job's fixed cost is
 *        paid once for all of them.
 *
 * Every piece of a chain goes through the same conversion: none, or RGB565
 * widened to RGB888 by the transmit channel's converter. Never through the
 * CPU's cache: sources are written back by their writers. A fill sends one
 * line of pixels again for every row of a block (a transmit descriptor a
 * row, one receive descriptor), so it needs no picture of its size.
 *
 * @par Thread Safety
 * One task at a time.
 */
class BlitChain {
        public:
                /** @brief What the pixels go through. */
                enum class Conversion : uint8_t {
                        None,           ///< Copied as they are (the same size of pixel both sides).
                        Rgb565ToRgb888, ///< 2 bytes a pixel in, 3 out.
                };

                /** @brief A picture: @p width pixels a line (its stride), @p height lines. */
                struct Picture {
                                const uint8_t *data = nullptr;
                                uint16_t       width = 0;
                                uint16_t       height = 0;
                                uint8_t        bytesPerPixel = 2; ///< 1 to 4.
                };

                BlitChain() = default;
                ~BlitChain() { stop(); }
                BlitChain(const BlitChain &) = delete;
                BlitChain &operator=(const BlitChain &) = delete;

                /**
                 * @brief Takes a 2D-DMA pool and allocates descriptors for
                 *        @p maxPieces pieces and @p maxRows rows of fills
                 *        (internal DMA RAM, 16 bytes each).
                 * @return `Ok`, `InvalidState` (started), `Invalid` (0 pieces),
                 *         `NoMem`, or the driver's error.
                 */
                Error start(Conversion conversion, size_t maxPieces, size_t maxRows = 0);

                /** @brief Waits for a job under way, frees everything. Idempotent. */
                void stop();

                /** @brief Returns true between `start()` and `stop()`. */
                bool isStarted() const { return _hw != nullptr; }

                /** @brief Returns the pieces added since the last run. */
                size_t count() const { return _rxCount; }

                /** @brief Forgets the pieces added. */
                void clear() {
                        _txCount = 0;
                        _rxCount = 0;
                        return;
                }

                /**
                 * @brief Adds a copy of @p width × @p height pixels from
                 *        (@p fromX, @p fromY) of @p from to (@p toX, @p toY) of @p to.
                 * @return `Ok`, `Full`, `InvalidState`, or `Invalid` (outside a
                 *         picture, or pixel sizes the conversion doesn't take).
                 * @nonblocking
                 */
                Error add(const Picture &from, uint16_t fromX, uint16_t fromY, const Picture &to, uint16_t toX,
                          uint16_t toY, uint16_t width, uint16_t height);

                /**
                 * @brief Adds a fill of @p width × @p height pixels at (@p toX,
                 *        @p toY) of @p to with the @p width pixels at @p line
                 *        (in the destination's format, DMA-readable), sent once a
                 *        row. Only without a conversion.
                 * @return As `add()`.
                 * @nonblocking
                 */
                Error addFill(const uint8_t *line, const Picture &to, uint16_t toX, uint16_t toY, uint16_t width,
                              uint16_t height);

                /**
                 * @brief Runs the pieces added as one job, waits until they are
                 *        written, and forgets them.
                 * @return `Ok` (also for no pieces), `InvalidState`, `Timeout`, or
                 *         the driver's error.
                 * @blocking
                 */
                Error run(unsigned int timeoutMs);

                /**
                 * @brief Starts the pieces added as one job and forgets them,
                 *        without waiting: the next `add()`, `addFill()` or `run()`
                 *        waits for it, or `wait()`. Sources stay untouched until then.
                 * @return `Ok` (also for no pieces), `InvalidState`, or the driver's error.
                 * @nonblocking
                 */
                Error submit();

                /**
                 * @brief Waits for the job under way, if any.
                 * @return `Ok` or `Timeout`.
                 * @blocking
                 */
                Error wait(unsigned int timeoutMs);

                /** @brief Returns true while a job is under way (or not yet seen done). */
                bool isBusy() const { return _busy; }

        private:
                struct Hardware;

                Hardware  *_hw = nullptr;
                Semaphore  _done{1, 0};
                bool       _busy = false; // a job enqueued and not yet seen done
                size_t     _txCount = 0;  // transmit descriptors written
                size_t     _rxCount = 0;  // receive descriptors (pieces) written
                size_t     _max = 0;      // pieces
                size_t     _maxTx = 0;    // transmit descriptors
                Conversion _conversion = Conversion::None;

                friend struct BlitChainIsr;
};

BMDS_NAMESPACE_END
