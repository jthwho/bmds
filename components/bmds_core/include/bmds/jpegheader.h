/**
 * @file      jpegheader.h
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
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A JPEG's markers up to its first scan (`devplan/images.md` §3b): its
 *        size, coding, components and sampling, restart interval and EXIF
 *        orientation, read without decoding anything. What the code choosing
 *        a decoder asks first.
 *
 * @par Thread Safety
 * A plain value; `parse()` is reentrant.
 */
struct JpegHeader {
                /** @brief How the colour components are sampled. */
                enum class Sampling : uint8_t {
                        Gray,   ///< One component.
                        Yuv444, ///< Three, none subsampled.
                        Yuv422, ///< Three, chroma halved across.
                        Yuv420, ///< Three, chroma halved both ways.
                        Other,  ///< Anything else (4:4:0, 4:1:1, CMYK...).
                };

                /** @brief The frame's coding (its SOF marker). */
                enum class Coding : uint8_t {
                        Baseline,    ///< SOF0.
                        Extended,    ///< SOF1: sequential, maybe 12-bit.
                        Progressive, ///< SOF2.
                        Other,       ///< Lossless, hierarchical, arithmetic.
                };

                uint32_t width = 0;
                uint32_t height = 0;
                uint8_t  components = 0;
                uint8_t  precision = 8; ///< Bits a sample.
                Sampling sampling = Sampling::Other;
                Coding   coding = Coding::Other;
                uint16_t restartInterval = 0;
                uint8_t  mcuWidth = 8;    ///< The first component's MCU: 8 × its horizontal sampling.
                uint8_t  mcuHeight = 8;   ///< 8 × its vertical sampling.
                uint8_t  orientation = 1; ///< EXIF's, 1-8 (1: upright, or none given).
                bool     adobe = false;   ///< An Adobe APP14 marker (its colour transform).
                size_t   scanAt = 0;      ///< Where the first SOS marker is.

                /**
                 * @brief Reads @p file's markers up to the first scan.
                 * @return The header; `CorruptData` (not a JPEG, or cut short before the scan).
                 */
                static Result<JpegHeader> parse(ByteSpan file);

                /**
                 * @brief Returns true if the P4's codec decodes it: baseline, 8-bit, grey or
                 *        4:4:4/4:2:2/4:2:0, width × height a multiple of 8 (ESP-IDF's driver refuses others).
                 */
                bool isHardware() const {
                        return coding == Coding::Baseline && precision == 8 && sampling != Sampling::Other &&
                               (uint64_t(width) * height) % 8 == 0;
                }

                /** @brief Returns the width the codec writes: rounded up to the MCU's. */
                uint32_t paddedWidth() const { return (width + mcuWidth - 1) / mcuWidth * mcuWidth; }

                /** @brief Returns the height the codec writes: rounded up to the MCU's. */
                uint32_t paddedHeight() const { return (height + mcuHeight - 1) / mcuHeight * mcuHeight; }

                /** @brief Returns true if TJpgDec (software) decodes it: baseline, 8-bit, 1 or 3 components. */
                bool isSoftware() const {
                        return coding == Coding::Baseline && precision == 8 && (components == 1 || components == 3);
                }

                /** @brief Returns its info (`ImageInfo`). */
                ImageInfo info() const;
};

BMDS_NAMESPACE_END
