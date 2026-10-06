/**
 * @file      png.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief PNG files from pictures in memory (`devplan/display.md` §5d).
 *
 * Truecolor, 8 bits a channel. Each row is filtered (None, Sub or Up, the
 * one with the smallest sum of absolute values) and compressed by the ROM's
 * deflate (miniz's `tdefl`), so no compressor is in flash. The compressor's
 * state (164 KB) is taken from internal RAM when there is room, else PSRAM,
 * for the call.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Png {

        /** @brief Takes the file's bytes, in order. */
        using Sink = Error (*)(void *arg, ByteSpan data);

        /** @brief How hard to compress: 0 stores, 1 is fastest, 9 smallest. */
        constexpr int DefaultLevel = 2;

        /** @brief What a write took. */
        struct Stats {
                        size_t   bytes = 0;             ///< The file's.
                        uint32_t filterUs = 0;          ///< Reading and filtering rows.
                        uint32_t readUs = 0;            ///< Of which reading them (B, G, R to R, G, B).
                        uint32_t compressUs = 0;        ///< Deflating (writing included).
                        bool     internalState = false; ///< The compressor's state was in internal RAM.
        };

        /**
         * @brief Writes a @p width × @p height picture of B, G, R bytes
         *        (the surface's order), @p stride bytes a line, as a PNG.
         * @return `Ok`, `Invalid`, `NoMem`, or @p sink's error. @blocking
         */
        Error write(const uint8_t *bgr, uint16_t width, uint16_t height, size_t stride, int level, Sink sink, void *arg,
                    Stats *stats);

} // namespace Png

BMDS_NAMESPACE_END
