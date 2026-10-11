/**
 * @file      jpegcodec.h
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
#include <bmds/jpegheader.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stats.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The SoC's JPEG codec (`devplan/images.md` §3c): baseline JPEGs
 *        decoded and encoded by the hardware, for the system and programs.
 *
 * Numbered (`jpeg0`, as the board says). One picture at a time: callers wait
 * for the codec (`decode()`, `encode()`) or don't (`tryDecode()`). The work
 * itself waits on the block's interrupt (ESP-IDF's driver), never polls.
 *
 * **Buffers.** The codec writes a whole picture, each side rounded up to the
 * MCU (`JpegHeader::paddedWidth()`), rows `padded width × bytes a pixel` apart,
 * into memory aligned to `Align` in address and size. A buffer laid out so
 * (`layoutFor()`: its stride, and room for the padded rows in its `capacity`)
 * is decoded into without a copy; any other is filled from a bounce buffer,
 * row by row, which costs a copy (counted: `jpegN.bounces`). Its own formats
 * are `Bgr24`, `Rgb24`, `Rgb565` (and `Gray8` of a grey JPEG).
 *
 * **Colour.** JFIF's YCbCr is full range; ESP-IDF sets the 2D-DMA's YUV-to-RGB
 * to video's limited range, which stretches a JPEG's colours. The codec's
 * decodes load full-range coefficients instead (ESP-IDF's set-up is wrapped by
 * the link), unless `Decode::limitedRange` asks for video's (ESP-IDF's own). The PPA can't stand in: its full-range conversion takes neither
 * 4:2:2 (this chip's revision) nor 4:2:0 (it never ends).
 *
 * @par Thread Safety
 * Thread-safe.
 */
class JpegCodec {
        public:
                /** @brief Codecs a board can have. */
                static constexpr size_t MaxCodecs = 1;

                /** @brief The alignment of a zero-copy buffer's address and size (a cache line). */
                static constexpr size_t Align = 64;

                /** @brief A sink for encoded bytes (as `Png::write()`'s). */
                using Sink = Error (*)(void *arg, ByteSpan data);

                /** @brief How to decode. */
                struct Decode {
                                PixelFormat format = PixelFormat::
                                        Bgr24; ///< Any; `Bgr24`, `Rgb24`, `Rgb565` (and `Gray8` of a grey JPEG) are the codec's own.
                                bool bt709 = false; ///< BT.709's YCbCr; else BT.601 (JFIF's).
                                /** Video's limited range (Y 16-235, Cb and Cr 16-240: MJPEG from a camera, video);
                                    else JFIF's full range (JPEG files). */
                                bool limitedRange = false;
                };

                /** @brief How to encode. */
                struct Encode {
                                uint8_t              quality = 90;                            ///< 1-100.
                                JpegHeader::Sampling sampling = JpegHeader::Sampling::Yuv420; ///< `Gray` for grey.
                };

                /** @brief A zero-copy buffer's layout for a picture. */
                struct Layout {
                                uint32_t width = 0;  ///< Padded.
                                uint32_t height = 0; ///< Padded.
                                size_t   stride = 0; ///< Bytes a row.
                                size_t   bytes = 0;  ///< In all, a multiple of `Align`.
                };

                /** @brief What it has done since boot. */
                struct Counts {
                                uint32_t decodes = 0;
                                uint32_t encodes = 0;
                                uint32_t bounces = 0; ///< Of them, through a bounce buffer.
                                uint32_t errors = 0;
                                uint64_t pixels = 0;
                                uint64_t busyUs = 0;
                                uint32_t lastUs = 0;
                                uint32_t maxUs = 0;
                };

                /**
                 * @brief Returns codec @p number (the board's `jpegCodecs`).
                 * @return The codec, or `NotSupported` (none on this board, or no such number).
                 */
                static Result<JpegCodec *> codec(size_t number);

                explicit JpegCodec(uint8_t number);
                ~JpegCodec() = delete;
                JpegCodec(const JpegCodec &) = delete;
                JpegCodec &operator=(const JpegCodec &) = delete;

                /** @brief Returns the number. */
                uint8_t number() const { return _number; }

                /** @brief Returns true if the codec's own output is @p format for @p header (no conversion). */
                static bool isNative(const JpegHeader &header, PixelFormat format);

                /** @brief Returns the layout of a zero-copy buffer for @p header in @p format. */
                static Layout layoutFor(const JpegHeader &header, PixelFormat format);

                /** @brief Returns true if @p out is decoded into without a copy. */
                static bool isZeroCopy(const JpegHeader &header, const ImageBuffer &out);

                /**
                 * @brief Decodes @p jpeg into @p out's top-left corner (at least the picture's
                 *        size; zero-copy: `layoutFor()`'s), as @p how says. Waits up to
                 *        @p timeoutMs for the codec (`WaitForever`).
                 * @return `Ok`, `CorruptData` (not a JPEG; the codec failed on it),
                 *         `NotSupported` (not one the codec takes: `JpegHeader::isHardware()`),
                 *         `BufferTooSmall`, `NoMem` (the bounce buffer), `Timeout`.
                 * @blocking
                 */
                Error decode(ByteSpan jpeg, const ImageBuffer &out, const Decode &how, unsigned int timeoutMs);

                /** @brief As `decode()`, `WouldBlock` if the codec is busy. @nonblocking (to start) */
                Error tryDecode(ByteSpan jpeg, const ImageBuffer &out, const Decode &how);

                /**
                 * @brief Encodes @p in (any format: `Bgr24`, or `Gray8` for grey, without a copy;
                 *        the others converted, alpha dropped) as a baseline JPEG, given to @p sink.
                 * @return `Ok`, `Invalid` (quality, sampling, an empty picture), `NoMem`,
                 *         `IoError` (the codec failed), `Timeout`, or @p sink's error.
                 * @blocking
                 */
                Error encode(const ImageBuffer &in, const Encode &how, Sink sink, void *arg, unsigned int timeoutMs);

                /** @brief Returns what it has done. */
                Counts counts() const;

        private:
                Error       decodeLocked(ByteSpan jpeg, const JpegHeader &h, const ImageBuffer &out, const Decode &how);
                Error       encodeLocked(const ImageBuffer &in, const Encode &how, Sink sink, void *arg);
                Error       makeDecoder();
                Error       makeEncoder();
                void        count(bool decoding, bool bounced, Error err, uint64_t pixels, uint64_t startUs);
                static void readStats(void *arg, Span<int64_t> values);

                Mutex        _lock;
                void        *_decoder = nullptr; // ESP-IDF's engines, made on first use
                void        *_encoder = nullptr;
                Counts       _counts;
                uint8_t      _number;
                Stats::Group _stats;
};

BMDS_NAMESPACE_END
