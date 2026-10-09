/**
 * @file      sampleformat.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A stream's sample format (`devplan/audio.md` §6.1, §17). The order
 *        and the names become ABI in step 3: append only.
 */
enum class SampleFormat : uint8_t {
        U8,      ///< Unsigned 8-bit, 128 the middle.
        S8,      ///< Signed 8-bit.
        S16Le,   ///< Signed 16-bit, little-endian.
        S16Be,   ///< Signed 16-bit, big-endian.
        S24Le,   ///< Signed 24-bit in the low bytes of 32, little-endian.
        S24_3Le, ///< Signed 24-bit in 3 bytes, little-endian.
        S32Le,   ///< Signed 32-bit, little-endian (24 bits kept: the mix is float).
        F32Le,   ///< IEEE float, little-endian, ±1.0 full scale.
        MuLaw,   ///< G.711 µ-law.
        ALaw,    ///< G.711 A-law.
};

/**
 * @brief Converts samples between the stream formats and the engine's
 *        interleaved stereo float (`devplan/audio.md` §17).
 *
 * Integer formats up to 24 bits, µ-law and A-law go to float and back
 * exactly. Mono becomes both channels; stereo stays as it is.
 *
 * @par Thread Safety
 * Stateless: callable from any task.
 */
namespace SampleFormats {

        /** @brief Formats in all. */
        inline constexpr size_t Count = 10;

        /** @brief Returns the bytes one sample takes. */
        size_t bytesPerSample(SampleFormat format);

        /** @brief Returns the format's name (`s16le`, `mu-law`, ...): the `audio` command's word. */
        const char *name(SampleFormat format);

        /** @brief Returns the format named @p text, or `Invalid`. */
        Result<SampleFormat> parse(StringView text);

        /**
         * @brief Converts @p frames frames of @p channels (1 or 2) channels from
         *        @p src into interleaved stereo float at @p stereo.
         * @hotpath
         */
        void toFloat(SampleFormat format, unsigned channels, const uint8_t *src, size_t frames, float *stereo);

        /**
         * @brief Converts @p frames stereo float frames into @p channels (1: the
         *        mean of the two; 2) channels of @p format at @p dst, clamped
         *        to full scale and rounded to nearest.
         */
        void fromFloat(SampleFormat format, unsigned channels, const float *stereo, size_t frames, uint8_t *dst);

        /** @brief Decodes a µ-law byte to 16-bit linear (G.711). */
        int16_t muLawToLinear(uint8_t code);

        /** @brief Decodes an A-law byte to 16-bit linear (G.711). */
        int16_t aLawToLinear(uint8_t code);

        /** @brief Encodes 16-bit linear to µ-law (G.711). */
        uint8_t linearToMuLaw(int16_t sample);

        /** @brief Encodes 16-bit linear to A-law (G.711). */
        uint8_t linearToALaw(int16_t sample);

} // namespace SampleFormats

BMDS_NAMESPACE_END
