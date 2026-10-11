/**
 * @file      imageplan.h
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
#include <bmds/jpegheader.h>
#include <bmds/result.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief How a picture is decoded, pure (`devplan/images.md` §3d): which
 *        decoder, and how much a JPEG is reduced on the way.
 *
 * A JPEG goes to the codec when it can take it and its picture fits the
 * budget; else to TJpgDec, reduced as far as the caller's smallest wanted
 * size allows, and at least far enough to fit the budget. PNG and BMP decode
 * whole, within the budget.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct ImagePlan {
                /** @brief The decoder. */
                enum class Decoder : uint8_t {
                        Png,
                        Bmp,
                        Hardware, ///< The JPEG codec (`JpegCodec`).
                        Software, ///< TJpgDec (`SoftJpeg`).
                };

                /** @brief What the caller wants. */
                struct Want {
                                uint32_t minWidth = 0; ///< The smallest wanted (0: the whole picture).
                                uint32_t minHeight = 0;
                                uint64_t budgetPixels = 0; ///< The most pixels decoded at once.
                                bool     hardware = true;  ///< A JPEG codec is there.
                };

                Decoder  decoder = Decoder::Png;
                uint8_t  shift = 0; ///< A JPEG reduced by 1/2^shift.
                uint32_t width = 0; ///< The picture decoded.
                uint32_t height = 0;

                /**
                 * @brief Plans @p info's decode (a JPEG's @p jpeg header too).
                 * @return The plan; `NotSupported` (no decoder takes it: progressive JPEG, 12-bit...),
                 *         `OutOfRange` (over the budget, and no reduction makes it fit).
                 */
                static Result<ImagePlan> make(const ImageInfo &info, const JpegHeader *jpeg, const Want &want);
};

BMDS_NAMESPACE_END
