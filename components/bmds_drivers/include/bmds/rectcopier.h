/**
 * @file      rectcopier.h
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
 * @brief Copies rectangles of 16-bit pixels between pictures with the
 *        2D-DMA (`devplan/display.md` §5b): a block of one picture into a
 *        block of another, each with its own width, never through the CPU's
 *        cache.
 *
 * Into PSRAM, rows must start and end on 4 bytes (IDF's 2D-DMA note): even
 * x and width.
 *
 * @par Thread Safety
 * One task at a time.
 */
class RectCopier {
        public:
                /** @brief A picture: @p width pixels a line (its stride), @p height lines. */
                struct Picture {
                                const uint8_t *data = nullptr;
                                uint16_t       width = 0;
                                uint16_t       height = 0;
                };

                RectCopier() = default;
                ~RectCopier() { stop(); }
                RectCopier(const RectCopier &) = delete;
                RectCopier &operator=(const RectCopier &) = delete;

                /**
                 * @brief Takes a 2D-DMA pool, allocates the descriptors (internal
                 *        DMA RAM).
                 * @return `Ok`, `InvalidState` (started), `NoMem`, or the driver's
                 *         error.
                 */
                Error start();

                /** @brief Waits for a copy under way, frees everything. Idempotent. */
                void stop();

                /**
                 * @brief Copies @p width × @p height pixels from (@p fromX, @p fromY)
                 *        of @p from to (@p toX, @p toY) of @p to, and waits until
                 *        they're written. The source is written back from the
                 *        cache by the caller.
                 * @return `Ok`, `Invalid` (outside a picture, or an odd x or width),
                 *         `InvalidState`, `Timeout`, or the driver's error.
                 * @blocking
                 */
                Error copy(const Picture &from, uint16_t fromX, uint16_t fromY, const Picture &to, uint16_t toX,
                           uint16_t toY, uint16_t width, uint16_t height, unsigned int timeoutMs);

        private:
                struct Hardware;

                Error settle(unsigned int timeoutMs); // waits for the job under way, if any

                Hardware *_hw = nullptr;
                Semaphore _done{1, 0};
                bool      _busy = false; // a job enqueued and not yet seen done

                friend struct RectCopierIsr;
};

BMDS_NAMESPACE_END
