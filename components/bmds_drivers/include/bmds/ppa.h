/**
 * @file      ppa.h
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
#include <bmds/rect.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The pixel-processing accelerator (`devplan/games.md` §10): scaling
 *        (its SRM engine) and an 8-bit palette expanded through the blender's
 *        lookup table (CLUT), on the 2D-DMA, with BMDS's own register setup
 *        (IDF's driver leaves the palette out, and its fixed cost a job is
 *        high).
 *
 * A job reads a block of one picture and writes it at an offset of another,
 * by DMA: sources are written back from the cache by their writers, and a
 * CPU reading a destination invalidates it first. One job at a time:
 * starting one waits for the last. Jobs end by an interrupt; the timeouts
 * cover a job that never ends (`docs/platforms/esp32p4.md`).
 *
 * @par Thread Safety
 * One task at a time.
 */
class Ppa {
        public:
                /** @brief A picture's pixels. */
                enum class Format : uint8_t {
                        Rgb565,   ///< 2 bytes.
                        Rgb888,   ///< 3 bytes: B, G, R.
                        Argb8888, ///< 4 bytes: B, G, R, A.
                        Gray8,    ///< 1 byte (scaling only).
                        L8,       ///< 1 byte, an index into the palette (expanding only).
                };

                /** @brief A picture: @p width pixels a line (its stride), @p height lines. */
                struct Picture {
                                const uint8_t *data = nullptr;
                                uint16_t       width = 0;
                                uint16_t       height = 0;
                                Format         format = Format::Rgb888;
                };

                /** @brief Entries in the palette. */
                static constexpr size_t PaletteSize = 256;

                /** @brief Scale factors are in 16ths: 16 is 1×, 1 the least, `MaxScale16` the most. */
                static constexpr uint16_t MaxScale16 = 16 * 16 - 1;

                /** @brief Returns the bytes of a pixel of @p format. */
                static constexpr size_t bytesPerPixel(Format format) {
                        switch (format) {
                                case Format::Rgb565: return 2;
                                case Format::Rgb888: return 3;
                                case Format::Argb8888: return 4;
                                default: return 1;
                        }
                }

                /** @brief Returns the scaled size of @p size pixels at @p scale16 (as the engine makes it). */
                static constexpr uint16_t scaled(uint16_t size, uint16_t scale16) {
                        return static_cast<uint16_t>(uint32_t(size) * scale16 / 16);
                }

                Ppa() = default;
                ~Ppa() { stop(); }
                Ppa(const Ppa &) = delete;
                Ppa &operator=(const Ppa &) = delete;

                /**
                 * @brief Powers the PPA and takes a 2D-DMA pool; the palette is black.
                 * @return `Ok`, `InvalidState` (started), `NoMem`, or the driver's error.
                 */
                Error start();

                /** @brief Waits for a job under way, powers the PPA down. Idempotent. */
                void stop();

                /** @brief Returns true between `start()` and `stop()`. */
                bool isStarted() const { return _hw != nullptr; }

                /**
                 * @brief Sets @p count palette entries from @p first (0xRRGGBB each).
                 *        Not while an expansion runs: a running one would use both.
                 */
                void setPalette(const uint32_t *rgb, size_t first, size_t count);

                /**
                 * @brief Starts scaling @p block of @p from by @p scaleX16, @p scaleY16
                 *        (16ths) to (@p toX, @p toY) of @p to, converting its pixels
                 *        (`Rgb565`, `Rgb888`, `Argb8888`, `Gray8` either side).
                 * @return `Ok`, `InvalidState`, `Invalid` (outside a picture, a
                 *         format, a factor), `Timeout` (the last job), or the driver's.
                 * @nonblocking Except for the last job.
                 */
                Error scale(const Picture &from, const Rect &block, const Picture &to, uint16_t toX, uint16_t toY,
                            uint16_t scaleX16, uint16_t scaleY16, unsigned int timeoutMs);

                /**
                 * @brief Starts expanding @p block of @p from (`L8`) through the palette
                 *        to (@p toX, @p toY) of @p to (`Rgb888`), the same size.
                 *        @p bypass: the blender's bypass mode, one input channel
                 *        (measured against the blend with an invisible foreground).
                 * @return As `scale()`.
                 * @nonblocking Except for the last job.
                 */
                Error expand(const Picture &from, const Rect &block, const Picture &to, uint16_t toX, uint16_t toY,
                             bool bypass, unsigned int timeoutMs);

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
                enum class Job : uint8_t {
                        Scale,
                        Expand,
                        ExpandBypass
                };

                Hardware *_hw = nullptr;
                Semaphore _done{1, 0};
                bool      _busy = false;

                friend struct PpaIsr;
};

BMDS_NAMESPACE_END
