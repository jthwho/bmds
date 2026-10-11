/**
 * @file      image.h
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
 * @brief How pixels are laid out in memory (`devplan/images.md` §3a), named by
 *        their bytes so a name never depends on endianness. Values are the
 *        SDK's `BMDS_PIXEL_*`: appended only.
 */
enum class PixelFormat : uint8_t {
        Bgr24 = 0,  ///< Bytes B, G, R: the screen's.
        Rgb24 = 1,  ///< Bytes R, G, B.
        Bgra32 = 2, ///< Bytes B, G, R, A: 0xAARRGGBB as a little-endian word (the icons').
        Rgb565 = 3, ///< A little-endian 16-bit word, red in the top 5 bits.
        Gray8 = 4,  ///< One byte of luma (BT.601).
};

/** @brief The number of pixel formats (one past the last). */
inline constexpr unsigned PixelFormatCount = 5;

/** @brief Returns @p format's bytes a pixel. */
constexpr unsigned bytesPerPixel(PixelFormat format) {
        switch (format) {
                case PixelFormat::Bgr24:
                case PixelFormat::Rgb24: return 3;
                case PixelFormat::Bgra32: return 4;
                case PixelFormat::Rgb565: return 2;
                default: return 1;
        }
}

/** @brief Returns true if @p format keeps alpha. */
constexpr bool hasAlpha(PixelFormat format) {
        return format == PixelFormat::Bgra32;
}

/** @brief A picture's file format. Values are the SDK's `BMDS_IMAGE_*`: appended only. */
enum class ImageCodec : uint8_t {
        Png = 0,
        Jpeg = 1,
        Bmp = 2,
        Unknown = 0xff,
};

/**
 * @brief Pixels somewhere: a picture's rows, top first, @c stride bytes apart.
 *        It doesn't own them.
 *
 * @par Thread Safety
 * A plain value.
 */
struct ImageBuffer {
                uint8_t    *pixels = nullptr;
                uint32_t    width = 0;
                uint32_t    height = 0;
                size_t      stride = 0; ///< Bytes from a row to the next.
                PixelFormat format = PixelFormat::Bgr24;
                size_t      capacity = 0; ///< Bytes at `pixels` (0: its rows only); a codec may write past them.

                /** @brief Returns row @p y. */
                uint8_t *row(uint32_t y) const { return pixels + size_t(y) * stride; }

                /** @brief Returns true if it has pixels and room for its rows. */
                bool isValid() const {
                        return pixels != nullptr && width != 0 && height != 0 &&
                               stride >= size_t(width) * bytesPerPixel(format);
                }

                /** @brief Returns the bytes at `pixels`: `capacity`, else what its rows span. */
                size_t room() const { return capacity != 0 ? capacity : bytes(); }

                /** @brief Returns the bytes its rows span. */
                size_t bytes() const { return height == 0 ? 0 : stride * (height - 1) + width * bytesPerPixel(format); }
};

/**
 * @brief What a picture's header says (`devplan/images.md` §3a).
 *
 * @par Thread Safety
 * A plain value.
 */
struct ImageInfo {
                /** @brief `flags`; the SDK's `BMDS_IMAGE_ALPHA`... */
                enum Flag : uint8_t {
                        HasAlpha = 1u << 0,    ///< Some pixels may be transparent.
                        Progressive = 1u << 1, ///< A progressive JPEG (or an interlaced PNG).
                        Hardware = 1u << 2,    ///< The JPEG codec can decode it.
                };

                ImageCodec codec = ImageCodec::Unknown;
                uint32_t   width = 0;
                uint32_t   height = 0;
                uint8_t    flags = 0;
                uint8_t    orientation = 1; ///< EXIF's, 1-8 (1: upright).
};

/**
 * @brief Converting rows of pixels (`devplan/images.md` §3a): every decoder
 *        makes 0xAARRGGBB and stores it in the format asked for.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Pixels {

        /**
         * @brief Stores @p count 0xAARRGGBB pixels @p in at @p out in @p format; into a
         *        format without alpha each is blended over @p background (0xRRGGBB):
         *        `(s × a + d × (255 − a) + 127) / 255` a channel. PIE (into B, G, R and R, G, B):
         *        16 pixels a step, the same bytes.
         * @hotpath
         */
        void fromArgb(const uint32_t *in, size_t count, uint8_t *out, PixelFormat format, uint32_t background,
                      Simd::Kernel kernel = Simd::best());

        /** @brief Reads @p count pixels of @p format at @p in as 0xAARRGGBB (opaque formats: alpha 0xff). @hotpath */
        void toArgb(const uint8_t *in, size_t count, uint32_t *out, PixelFormat format);

        /**
         * @brief Writes @p count 3-byte pixels @p in to @p out with their first and third
         *        bytes swapped (R, G, B to B, G, R and back); @p in and @p out may be the
         *        same. PIE: 16 pixels a step.
         * @hotpath
         */
        void swap24(const uint8_t *in, uint8_t *out, size_t count, Simd::Kernel kernel = Simd::best());

        /** @brief Returns @p argb's luma (BT.601), 0-255. */
        constexpr uint8_t luma(uint32_t argb) {
                return uint8_t((77u * (argb >> 16 & 0xff) + 150u * (argb >> 8 & 0xff) + 29u * (argb & 0xff) + 128u) >>
                               8);
        }

} // namespace Pixels

BMDS_NAMESPACE_END
