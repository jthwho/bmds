/**
 * @file      colorexpander.h
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
 * @brief Widens RGB565 pixels to RGB888 in hardware: a memory-to-memory copy
 *        on the 2D-DMA through its transmit channel's color converter
 *        (`devplan/display.md` §5), so the pixels it writes never pass
 *        through the CPU's cache.
 *
 * @par Thread Safety
 * One task at a time (the scanout's refill task).
 */
class ColorExpander {
        public:
                ColorExpander() = default;
                ~ColorExpander() { stop(); }
                ColorExpander(const ColorExpander &) = delete;
                ColorExpander &operator=(const ColorExpander &) = delete;

                /**
                 * @brief Takes a 2D-DMA pool and allocates the descriptors
                 *        (internal DMA RAM).
                 * @return `Ok`, `NoMem`, or the driver's error.
                 */
                Error start();

                /** @brief Frees everything. Idempotent. */
                void stop();

                /**
                 * @brief Widens @p pixels RGB565 pixels at @p src (written back
                 *        from the cache by the caller) into RGB888 at @p dst, and
                 *        waits until they are written.
                 * @return `Ok`, `Invalid` (more than the DMA moves at once),
                 *         `Timeout`, or the driver's error.
                 * @blocking
                 */
                Error expand(const uint8_t *src, uint8_t *dst, size_t pixels, unsigned int timeoutMs);

        private:
                struct Hardware;

                Hardware *_hw = nullptr;
                Semaphore _done{1, 0};
                bool      _busy = false; // a job enqueued and not yet seen done

                Error settle(unsigned int timeoutMs); // waits for the job under way, if any

                friend struct ColorExpanderIsr;
};

BMDS_NAMESPACE_END
