/**
 * @file      images.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A deck's pictures (devplan/preso.md §4, §5): their sizes, and each one scaled to where it is
 * drawn, filtered (Marp's grayscale and sepia), mapped to a theme's palette by ordered
 * dithering, and kept in the slides' pixel format. Decoding is the platform's; the rest is the
 * core's, the same everywhere.
 */

#ifndef PRESO_IMAGES_H
#define PRESO_IMAGES_H

#include "canvas.h"
#include "platform.h"

namespace preso {

        /** @brief A picture ready to draw: the canvas's format, its colours premultiplied by alpha. */
        struct ScaledImage {
                        int32_t     width = 0;
                        int32_t     height = 0;
                        uint32_t    stride = 0;
                        PixelFormat format = PixelFormat::Rgb565;
                        uint8_t    *pixels = nullptr;
                        uint8_t    *alpha = nullptr; ///< width × height opacities; nullptr: opaque.
        };

        /** @brief What scale() does to the colours (bits). */
        namespace ScaleFilter {
                constexpr uint8_t Grayscale = 0x1;
                constexpr uint8_t Sepia = 0x2;
        } // namespace ScaleFilter

        /**
         * @brief Scales @p picture to @p width × @p height (area-averaged down, bilinear up, alpha
         *        premultiplied), applies @p filters, maps it to @p palette (if any) or dithers it to
         *        RGB565, into @p out (its memory malloc'd: freeScaled()). Only @p window of the
         *        scaled picture is made (nullptr: all of it): a picture covering a slide may be far
         *        bigger than the part of it shown.
         * @return Error::NoMemory, Error::Invalid (a size, a window outside it), or Ok.
         */
        Error scalePicture(const Picture &picture, int32_t width, int32_t height, const Rect *window, uint8_t filters,
                           const Palette *palette, PixelFormat format, ScaledImage *out);
        void  freeScaled(ScaledImage &image);

        /** @brief The folder @p path is in (`.` for a bare name), into @p out. */
        void folderOf(const char *path, char *out, size_t size);

        /** @brief Draws @p image with its top left at (@p x, @p y), inside the canvas's clip. */
        void blitImage(Canvas &canvas, const ScaledImage &image, int32_t x, int32_t y);

        /**
         * @brief A deck's pictures: sizes as they are asked for, scaled pictures kept while they
         *        fit in MaxBytes (the least recently drawn dropped first).
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class ImageCache {
                public:
                        /** @brief Two full-slide pictures at RGB565 (the one shown and the next), a little over. */
                        static constexpr size_t MaxBytes = 4u << 20;

                        /** @brief Pictures named relative to the folder of the deck at @p deckPath. */
                        ImageCache(Platform &platform, const char *deckPath);
                        ~ImageCache();
                        ImageCache(const ImageCache &) = delete;
                        ImageCache &operator=(const ImageCache &) = delete;

                        /**
                         * @brief The picture's size: Error::NotFound (no file), Error::NotSupported (a
                         *        file, but the platform can't decode: size unknown), Error::Invalid
                         *        (not a picture it reads).
                         */
                        Error size(StringView path, int32_t *width, int32_t *height);

                        /**
                         * @brief @p window of the picture at @p path drawn @p width × @p height, as
                         *        scalePicture(); nullptr if it can't be (missing, not a picture, no memory).
                         */
                        const ScaledImage *get(StringView path, int32_t width, int32_t height, const Rect &window,
                                               uint8_t filters, const Palette *palette, PixelFormat format);

                        /**
                         * @brief A picture that couldn't be drawn since the last call, and why
                         *        (Error::Ok: none): each one once, for the presenter's messages.
                         */
                        Error takeFailure(char *path, size_t size);

                        /** @brief Forgets everything (the files may have changed). */
                        void clear();

                        /** @brief Bytes of scaled pictures kept. */
                        size_t bytesKept() const { return _bytes; }

                private:
                        struct Info;
                        struct Entry;

                        bool  resolve(StringView path, char *out, size_t size) const;
                        Info *info(StringView path);
                        void  dropOldest();

                        Platform &_platform;
                        char      _folder[512];
                        Info     *_infos = nullptr;
                        char      _failedPath[256] = {};
                        Error     _failed = Error::Ok;
                        Entry    *_entries = nullptr;
                        size_t    _bytes = 0;
                        uint32_t  _clock = 0;
        };

} // namespace preso

#endif // PRESO_IMAGES_H
