/**
 * @file      imagescaler.h
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
#include <bmds/memspace.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Pictures resized, turned and mirrored, pure (`devplan/images.md`
 *        §3e).
 *
 * Down, each output pixel is the average of the input it covers (no aliasing
 * however big the reduction); up, bilinear; each axis on its own (one may
 * shrink while the other grows). Fixed-point, 12-bit weights. Alpha is
 * averaged as it is (not premultiplied).
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace ImageScaler {

        /** @brief A width and height. */
        struct Size {
                        uint32_t width = 0;
                        uint32_t height = 0;
        };

        /**
         * @brief Returns the largest size of @p width × @p height's aspect inside
         *        @p boxWidth × @p boxHeight (each side at least 1); never larger
         *        than the picture unless @p enlarge.
         */
        Size fit(uint32_t width, uint32_t height, uint32_t boxWidth, uint32_t boxHeight, bool enlarge);

        /**
         * @brief Scales @p in to fill @p out (both one format: `Bgr24`, `Rgb24`,
         *        `Bgra32` or `Gray8`), working memory (a few rows) from @p space.
         * @return `Ok`, `Invalid` (formats differ; an empty buffer), `NotSupported`
         *         (`Rgb565`), `NoMem`.
         */
        Error scale(const ImageBuffer &in, const ImageBuffer &out, MemSpace space);

        /**
         * @brief Returns the size @p width × @p height has after EXIF orientation
         *        @p orientation is undone (5-8 turn it a quarter: sides swapped).
         */
        Size oriented(uint32_t width, uint32_t height, uint8_t orientation);

        /**
         * @brief Copies @p in into @p out upright: EXIF orientation @p orientation
         *        (1-8) undone. @p out is `oriented()`'s size, the same format.
         * @return `Ok`, `Invalid`.
         */
        Error orient(const ImageBuffer &in, const ImageBuffer &out, uint8_t orientation);

} // namespace ImageScaler

BMDS_NAMESPACE_END
