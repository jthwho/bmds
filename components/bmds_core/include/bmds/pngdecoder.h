/**
 * @file      pngdecoder.h
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
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads PNG images into 0xAARRGGBB pixels (`devplan/gui.md` §4h): the
 *        icons, and anything small of the same kinds.
 *
 * Supported: 8-bit greyscale, RGB, greyscale with alpha and RGBA; palettes of
 * 1, 2, 4 or 8 bits (with `tRNS` alpha); every filter. Refused
 * (`NotSupported`): 16-bit samples, greyscale under 8 bits, interlacing. Chunk
 * CRCs and the zlib checksum are checked; ancillary chunks are skipped.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Png {

        /** @brief An image's size. */
        struct Size {
                        uint16_t width = 0;
                        uint16_t height = 0;
        };

        /**
         * @brief Reads @p file's header.
         * @return Its size; `CorruptData` (not a PNG) or `NotSupported` (a kind
         *         `decode()` refuses).
         */
        Result<Size> size(ByteSpan file);

        /**
         * @brief Decodes @p file into @p pixels (width × height, rows top first),
         *        with working memory (the image's compressed and raw bytes) from
         *        @p space, freed before it returns.
         * @return Its size; `CorruptData`, `NotSupported`, `BufferTooSmall`
         *         (@p pixels), or `NoMem`.
         */
        Result<Size> decode(ByteSpan file, Span<uint32_t> pixels, MemSpace space);

        /** @brief Returns the CRC-32 (PNG's, zlib's) of @p data continuing from @p crc (0 to start). */
        uint32_t crc32(ByteSpan data, uint32_t crc = 0);

} // namespace Png

BMDS_NAMESPACE_END
