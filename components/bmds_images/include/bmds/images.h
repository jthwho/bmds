/**
 * @file      images.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/image.h>
#include <bmds/imageplan.h>
#include <bmds/jpegheader.h>
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system's pictures (`devplan/images.md` §3d): what a file is,
 *        and its pixels in the format and size the caller wants, by whichever
 *        decoder suits (PNG, BMP, the JPEG codec, TJpgDec); and pictures
 *        written as PNG or JPEG. The SDK's `image_*` calls are this.
 *
 * @par Thread Safety
 * Thread-safe (the JPEG codec is shared: one picture at a time on it).
 */
namespace Images {

        /** @brief The setting: the most megapixels decoded at once. */
        inline constexpr const char *DecodeMaxKey = "image/decode-max";

        /** @brief The largest file read whole. */
        inline constexpr size_t MaxFileBytes = 64u * 1024 * 1024;

        /** @brief How to decode. */
        struct Options {
                        PixelFormat format = PixelFormat::Bgr24;
                        uint32_t    background = 0; ///< 0xRRGGBB: alpha blended over it into formats without.
                        uint32_t    minWidth = 0;   ///< The smallest wanted (a JPEG may come back reduced; 0: whole).
                        uint32_t    minHeight = 0;
                        uint32_t    boxWidth = 0; ///< Shown fitted in this box: no smaller than that fit is wanted.
                        uint32_t    boxHeight = 0;
                        bool        orient = false;  ///< EXIF's orientation undone (the picture comes upright).
                        bool        hardware = true; ///< The JPEG codec may be used.
        };

        /** @brief What a decode did. */
        struct Report {
                        ImageInfo info;      ///< The file's.
                        ImagePlan plan;      ///< The decoder, the reduction, the size decoded.
                        uint32_t  width = 0; ///< As given back (turned when oriented).
                        uint32_t  height = 0;
                        uint32_t  decodeUs = 0;
                        uint32_t  orientUs = 0;
                        bool      zeroCopy = false; ///< The codec wrote straight into the picture.
        };

        /**
         * @brief A picture's pixels, owned (freed with it). Move-only.
         */
        class Picture {
                public:
                        Picture() = default;
                        ~Picture() { reset(); }
                        Picture(Picture &&other) noexcept
                            : _image(other._image), _memory(std::exchange(other._memory, nullptr)) {}
                        Picture &operator=(Picture &&other) noexcept {
                                if (this != &other) {
                                        reset();
                                        _image = other._image;
                                        _memory = std::exchange(other._memory, nullptr);
                                }
                                return *this;
                        }
                        Picture(const Picture &) = delete;
                        Picture &operator=(const Picture &) = delete;

                        /** @brief Allocates @p width × @p height of @p format in @p space (`stride`: 0 for packed rows). */
                        Error allocate(uint32_t width, uint32_t height, PixelFormat format, MemSpace space,
                                       size_t stride = 0, size_t bytes = 0);

                        /** @brief Frees the pixels. */
                        void reset();

                        /** @brief Returns the pixels. */
                        const ImageBuffer &image() const { return _image; }

                        /** @brief Returns true if it has pixels. */
                        bool isValid() const { return _memory != nullptr; }

                private:
                        ImageBuffer _image;
                        void       *_memory = nullptr;
        };

        /** @brief A file's bytes, owned. Move-only. */
        class FileBytes {
                public:
                        FileBytes() = default;
                        ~FileBytes();
                        FileBytes(FileBytes &&other) noexcept
                            : _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0)) {}
                        FileBytes &operator=(FileBytes &&other) noexcept;
                        FileBytes(const FileBytes &) = delete;
                        FileBytes &operator=(const FileBytes &) = delete;

                        /** @brief Reads @p path whole (at most `MaxFileBytes`) into PSRAM. */
                        Error read(StringView path, const Vfs::Context &context);

                        /** @brief Reads @p file from where it is to its end (at most `MaxFileBytes`) into PSRAM. */
                        Error read(FileHandle &file);

                        /** @brief Returns the bytes. */
                        ByteSpan bytes() const { return ByteSpan(_data, _size); }

                private:
                        uint8_t *_data = nullptr;
                        size_t   _size = 0;
        };

        /**
         * @brief Reads @p file's header; @p jpeg (if given) gets a JPEG's markers.
         * @return Its info; `NotSupported` (not a PNG, JPEG or BMP), or the decoder's error.
         */
        Result<ImageInfo> info(ByteSpan file, JpegHeader *jpeg = nullptr);

        /** @brief Returns the budget, in pixels (`DecodeMaxKey`). */
        uint64_t budgetPixels();

        /**
         * @brief Plans @p file's decode for @p options: its decoder and the size decoded
         *        (`report.width`/`height`: turned when @p options orient it).
         * @return `Ok`, `NotSupported`, `OutOfRange` (over the budget), or the header's error.
         */
        Error plan(ByteSpan file, const Options &options, Report &report);

        /**
         * @brief Decodes @p file into @p out (at least `plan()`'s size), as @p options say.
         *        Orienting takes a second picture's memory for the time.
         * @return `Ok`, or the decoder's error.
         * @blocking
         */
        Error decode(ByteSpan file, const Options &options, const ImageBuffer &out, Report *report);

        /**
         * @brief Decodes @p file into a picture of its own, laid out for the codec when it
         *        decodes it (no copy).
         * @blocking
         */
        Error load(ByteSpan file, const Options &options, Picture &out, Report *report);

        /** @brief How to encode. */
        struct Encode {
                        ImageCodec           codec = ImageCodec::Png;                 ///< `Png` or `Jpeg`.
                        uint8_t              quality = 90;                            ///< JPEG: 1-100.
                        JpegHeader::Sampling sampling = JpegHeader::Sampling::Yuv420; ///< JPEG.
                        int                  level = 2;                               ///< PNG: 0-9.
        };

        /** @brief Takes encoded bytes, in order. */
        using Sink = Error (*)(void *arg, ByteSpan data);

        /**
         * @brief Writes @p in (any format; alpha dropped) as @p how says to @p sink.
         * @return `Ok`, `Invalid`, `NotSupported` (BMP; JPEG without a codec), `NoMem`, or
         *         the encoder's or @p sink's error.
         * @blocking
         */
        Error encode(const ImageBuffer &in, const Encode &how, Sink sink, void *arg);

        /** @brief Returns the codec a file name's extension says (`.png`, `.jpg`, `.jpeg`), else `Unknown`. */
        ImageCodec codecForName(StringView name);

        /** @brief Returns @p decoder's name: "png", "bmp", "hardware", "software". */
        const char *decoderName(ImagePlan::Decoder decoder);

} // namespace Images

BMDS_NAMESPACE_END
