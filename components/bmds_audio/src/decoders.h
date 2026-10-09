/**
 * @file      decoders.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Between `Decoder` and its formats (devplan/audio.md §19): each format's
 * opener, and what more than one of them reads (MPEG audio headers, ID3v2's
 * size, little- and big-endian numbers).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <bmds/decoder.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace Decoders {

        /** @brief Opens a WAV file. */
        Result<Decoder *> openWav(const Decoder::Source &source);

        /** @brief Opens an MP3 whose frames start at or after @p start (past an ID3v2 tag). */
        Result<Decoder *> openMp3(const Decoder::Source &source, uint64_t start);

        /** @brief Opens an Ogg Vorbis file. */
        Result<Decoder *> openVorbis(const Decoder::Source &source);

        /** @brief Opens a FLAC file whose `fLaC` is at @p start (past an ID3v2 tag). */
        Result<Decoder *> openFlac(const Decoder::Source &source, uint64_t start);

        /** @brief Allocates and constructs a @p T in `Psram` (null: `NoMem`). */
        template <typename T> T *make(const Decoder::Source &source) {
                void *mem = Memory::allocAligned(MemSpace::Psram, sizeof(T), alignof(T));
                return mem != nullptr ? ::new (mem) T(source) : nullptr;
        }

        inline uint16_t le16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | p[1] << 8);
        }

        inline uint32_t le32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
        }

        inline uint32_t be32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
                       static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]);
        }

        /** @brief The bytes an ID3v2 tag at @p h (10 bytes) takes with its header and footer; 0 if none. */
        uint64_t id3v2Bytes(const uint8_t *h);

        /**
         * @brief An MPEG audio frame header's four bytes (ISO/IEC 11172-3, 13818-3,
         *        and MPEG 2.5).
         */
        namespace MpegHeader {

                /** @brief Bytes in a header. */
                inline constexpr size_t Bytes = 4;

                /** @brief Returns true if @p h is a header: sync, and no reserved field. */
                bool isValid(const uint8_t *h);

                /** @brief Returns true if @p a and @p b are frames of one stream (version, layer, rate). */
                bool isSameStream(const uint8_t *a, const uint8_t *b);

                /** @brief Returns the frame's bytes with its header, or 0 for free format. */
                size_t frameBytes(const uint8_t *h);

                /** @brief Returns the samples a frame holds per channel. */
                unsigned samples(const uint8_t *h);

                /** @brief Returns the sample rate in Hz. */
                unsigned rateHz(const uint8_t *h);

                /** @brief Returns 1 or 2. */
                unsigned channels(const uint8_t *h);

                /** @brief Returns the bitrate in kb/s, 0 for free format. */
                unsigned bitrateKbps(const uint8_t *h);

                /** @brief Returns the layer, 1 to 3. */
                unsigned layer(const uint8_t *h);

                /** @brief Returns the bytes of a layer III frame's side information, with its CRC. */
                size_t sideInfoBytes(const uint8_t *h);

        } // namespace MpegHeader

} // namespace Decoders

BMDS_NAMESPACE_END
