/**
 * @file      pngfilter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/simd.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief PNG's row filters for writing a 3-byte truecolor picture
 *        (`Png::write()`, `devplan/simd.md` step 5): None, Sub or Up, by the
 *        smallest sum of absolute values (each byte read as signed), a row
 *        equal to the last one Up's zeros at once.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace PngFilter {

        /**
         * @brief Filters @p cur (after @p prev, both @p bytes long) into @p out: the
         *        filter's byte, then the filtered row. The 3 bytes before @p cur must
         *        be readable and zero (Sub's left of the first pixel).
         * @hotpath
         */
        void row(const uint8_t *cur, const uint8_t *prev, size_t bytes, uint8_t *out,
                 Simd::Kernel kernel = Simd::best());

} // namespace PngFilter

BMDS_NAMESPACE_END
