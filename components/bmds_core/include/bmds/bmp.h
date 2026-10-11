/**
 * @file      bmp.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/image.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads Windows bitmaps (`devplan/images.md` §3b).
 *
 * Supported: the OS/2 and Windows headers (12, 40, 52, 56, 108 and 124
 * bytes); 1, 4 and 8 bits with a palette, RLE8 and RLE4; 16 bits (5-5-5, or
 * bit fields); 24 bits; 32 bits (B, G, R and an unused byte, or bit fields,
 * alpha when its mask says); bottom-up and top-down rows. Refused
 * (`NotSupported`): JPEG or PNG inside a bitmap, sides over `MaxSide`.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Bmp {

        /** @brief The largest width or height decoded. */
        inline constexpr uint32_t MaxSide = 16384;

        /**
         * @brief Reads @p file's headers.
         * @return Its info (alpha when an alpha mask is given); `CorruptData` (not a bitmap) or
         *         `NotSupported`.
         */
        Result<ImageInfo> info(ByteSpan file);

        /**
         * @brief Decodes @p file into @p out's top-left corner (at least the image's size), in
         *        its format, alpha blended over @p background (0xRRGGBB) into a format without
         *        it. RLE's skipped pixels are palette entry 0. Needs no working memory.
         * @return `Ok`, `CorruptData`, `NotSupported`, `BufferTooSmall`.
         */
        Error decode(ByteSpan file, const ImageBuffer &out, uint32_t background);

} // namespace Bmp

BMDS_NAMESPACE_END
