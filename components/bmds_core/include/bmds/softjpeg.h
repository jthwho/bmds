/**
 * @file      softjpeg.h
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
#include <bmds/simd.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Baseline JPEGs decoded in software by TJpgDec (`devplan/images.md`
 *        §3b, `thirdparty/tjpgd`): a few KB of working memory whatever the
 *        picture's size, reduced by 1/2, 1/4 or 1/8 as it decodes (a reduction
 *        costs less than a whole decode). For pictures too big for the codec's
 *        memory, and those the codec refuses (sides not a multiple of 8).
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace SoftJpeg {

        /** @brief The working memory a decode takes (its tables, buffers and an MCU). */
        inline constexpr size_t WorkBytes = 16 * 1024;

        /** @brief The largest reduction: 1/2^3. */
        inline constexpr unsigned MaxShift = 3;

        /** @brief Returns a @p width × @p height picture's side reduced by 1/2^@p shift (each at least 1). */
        constexpr uint32_t reduced(uint32_t side, unsigned shift) {
                return side >> shift != 0 ? side >> shift : 1;
        }

        /**
         * @brief Decodes @p file reduced by 1/2^@p shift (0-3) into @p out's top-left
         *        corner (at least `reduced()`'s size), working memory from @p work.
         * @return `Ok`, `CorruptData`, `NotSupported` (progressive, 12-bit...),
         *         `BufferTooSmall`, `NoMem`, `Invalid` (@p shift).
         */
        Error decode(ByteSpan file, const ImageBuffer &out, unsigned shift, MemSpace work,
                     Simd::Kernel kernel = Simd::best());

} // namespace SoftJpeg

BMDS_NAMESPACE_END
