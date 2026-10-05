/**
 * @file      paletteexpander.h
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
 * @brief Expands 8-bit palette indices to RGB888 in hardware: the PPA's
 *        blender with its CLUT (`devplan/display.md` §5), fed and drained by
 *        the 2D-DMA, so the pixels it writes never pass through the CPU's
 *        cache.
 *
 * The blender reads two planes: the background is the indices through the
 * palette (L8), the foreground the same indices as alpha (A8) with a fixed
 * alpha of 0, so the output is the background. IDF's PPA driver doesn't
 * offer L8 (its CLUT support is a TODO), so this drives the PPA and the
 * 2D-DMA itself, as IDF's blend does.
 *
 * @par Thread Safety
 * One task at a time (the scanout's refill task).
 */
class PaletteExpander {
        public:
                /** @brief Palette entries. */
                static constexpr size_t PaletteSize = 256;

                PaletteExpander() = default;
                ~PaletteExpander() { stop(); }
                PaletteExpander(const PaletteExpander &) = delete;
                PaletteExpander &operator=(const PaletteExpander &) = delete;

                /**
                 * @brief Powers the PPA, takes a 2D-DMA pool, allocates the
                 *        descriptors (internal DMA RAM).
                 * @return `Ok`, `NoMem`, or the driver's error.
                 */
                Error start();

                /** @brief Frees everything. Idempotent. */
                void stop();

                /**
                 * @brief Loads @p count entries (0xRRGGBB) from entry @p first.
                 *        Not while an expansion runs.
                 */
                void setPalette(const uint32_t *rgb, size_t first, size_t count);

                /**
                 * @brief Expands @p width × @p lines indices at @p indices
                 *        (written back from the cache by the caller) into RGB888
                 *        at @p rgb, and waits until it is written.
                 * @return `Ok`, `Timeout`, or the driver's error.
                 * @blocking
                 */
                Error expand(const uint8_t *indices, uint8_t *rgb, uint16_t width, uint16_t lines,
                             unsigned int timeoutMs);

        private:
                struct Hardware;

                Hardware *_hw = nullptr;
                Semaphore _done{1, 0};
                bool      _busy = false; // a job enqueued and not yet seen done

                Error settle(unsigned int timeoutMs); // waits for the job under way, if any

                friend struct PaletteExpanderIsr;
};

BMDS_NAMESPACE_END
