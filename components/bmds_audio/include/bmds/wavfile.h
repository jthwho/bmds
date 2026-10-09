/**
 * @file      wavfile.h
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
#include <bmds/result.h>
#include <bmds/sampleformat.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads a WAV file's header: where its samples are and their format
 *        (`devplan/audio.md` §18).
 *
 * RIFF/WAVE, its chunks walked (with their padding to even lengths, unknown
 * ones skipped) to `fmt ` and `data`. PCM at 8, 16, 24 and 32 bits, IEEE float
 * at 32, A-law and µ-law, and `WAVE_FORMAT_EXTENSIBLE` naming one of those;
 * 1 or 2 channels. A `data` size of 0 or 0xFFFFFFFF (a file written as a
 * stream), or one past the file's end, means to the end.
 *
 * Malformed input is an error, never more than that: every size is checked
 * against the file's.
 *
 * @par Thread Safety
 * Stateless: callable from any task.
 */
namespace WavFile {

        /** @brief What `parse()` found. */
        struct Info {
                        SampleFormat format = SampleFormat::S16Le;
                        uint8_t      channels = 0;
                        uint32_t     rateHz = 0;
                        uint64_t     dataOffset = 0; ///< The first sample's byte in the file.
                        uint64_t     dataBytes = 0;  ///< Whole frames only.
        };

        /**
         * @brief Reads exactly `buf.size()` bytes at @p offset into @p buf.
         * @return `Ok`, `EndOfFile` (fewer were there), or the file's error.
         */
        using ReadAt = Error (*)(void *arg, uint64_t offset, WritableByteSpan buf);

        /**
         * @brief Finds the format and the samples of the WAV file @p fileSize
         *        bytes long that @p read reads.
         * @return The info; `Invalid` (not a WAV file, or a broken one),
         *         `NotSupported` (a WAV this can't play: another encoding, more
         *         channels, RIFX or RF64), or @p read's error.
         */
        Result<Info> parse(ReadAt read, void *arg, uint64_t fileSize);

} // namespace WavFile

BMDS_NAMESPACE_END
