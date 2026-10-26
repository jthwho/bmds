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
#include <bmds/image.h>
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads PNG images into 0xAARRGGBB pixels (`devplan/gui.md` §4h): the
 *        icons, and anything small of the same kinds.
 *
 * Supported (`devplan/images.md` §3b): every kind PNG has. Greyscale of 1, 2,
 * 4, 8 or 16 bits; RGB, greyscale with alpha and RGBA of 8 or 16 bits (16 taken
 * to 8); palettes of 1, 2, 4 or 8 bits (with `tRNS` alpha); every filter; Adam7
 * interlacing. Refused (`NotSupported`): sides over `MaxSide`, an unknown
 * critical chunk. Chunk CRCs and the zlib checksum are checked; ancillary
 * chunks are skipped. Decoding holds the compressed and the raw bytes at once.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Png {

        /** @brief The largest width or height decoded. */
        inline constexpr uint32_t MaxSide = 16384;

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

        /**
         * @brief Reads @p file's header: its size, alpha (an alpha channel or `tRNS`), interlacing
         *        (`ImageInfo::Progressive`).
         * @return Its info; `CorruptData` or `NotSupported`.
         */
        Result<ImageInfo> info(ByteSpan file);

        /**
         * @brief Decodes @p file into @p out's top-left corner (at least the image's size), in
         *        its format; into a format without alpha each pixel is blended over
         *        @p background (0xRRGGBB). Working memory from @p space. A palette image's
         *        first entries are replaced by @p colors (0xRRGGBB, each keeping its `tRNS`
         *        alpha; a recolored desktop pattern, `devplan/wallpaper.md` §4b); other kinds
         *        ignore them.
         * @return `Ok`, `CorruptData`, `NotSupported`, `BufferTooSmall`, `NoMem`.
         */
        Error decode(ByteSpan file, const ImageBuffer &out, uint32_t background, MemSpace space,
                     Span<const uint32_t> colors = Span<const uint32_t>());

        /**
         * @brief Reads a palette image's colors (0xRRGGBB) into @p out, as many as fit.
         * @return How many it has (0: not a palette image); `CorruptData`.
         */
        Result<size_t> palette(ByteSpan file, Span<uint32_t> out);

        /**
         * @brief Reads the next `tEXt` chunk at or after @p at (8 to start: past the signature;
         *        moved past it): its keyword and its text, as written (Latin-1), views into
         *        @p file.
         * @return True if there was one; false after the last (or a damaged chunk).
         */
        bool nextText(ByteSpan file, size_t &at, StringView &keyword, StringView &text);

        /** @brief Returns the CRC-32 (PNG's, zlib's) of @p data continuing from @p crc (0 to start). */
        uint32_t crc32(ByteSpan data, uint32_t crc = 0);

} // namespace Png

BMDS_NAMESPACE_END
