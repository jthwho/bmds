/**
 * @file      canvas.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pixels the core draws slides into (devplan/preso.md §2): its own, not libbmdsgame's, whose
 * header is the BMDS SDK's. RGB565 on the board (memory and the 2D-DMA), XRGB8888 for a PC.
 * Text is drawn into a canvas by the platform (Platform::drawText).
 */

#ifndef PRESO_CANVAS_H
#define PRESO_CANVAS_H

#include "color.h"
#include "types.h"

namespace preso {

        enum class PixelFormat : uint8_t {
                Rgb565,   ///< 16 bits: 5 red (high), 6 green, 5 blue.
                Xrgb8888, ///< 32 bits: 0xXXRRGGBB in the machine's order.
        };

        /** @brief Bytes a pixel of @p format takes. */
        inline uint32_t bytesPerPixel(PixelFormat format) {
                return format == PixelFormat::Rgb565 ? 2 : 4;
        }

        struct Rect {
                        int32_t x = 0;
                        int32_t y = 0;
                        int32_t width = 0;
                        int32_t height = 0;

                        bool isEmpty() const { return width <= 0 || height <= 0; }
                        /** @brief The part of this inside @p other. */
                        Rect intersected(const Rect &other) const;
        };

        /** @brief The smallest rectangle holding @p a and @p b (an empty one adds nothing). */
        Rect uniteRects(const Rect &a, const Rect &b);

        /**
         * @brief A picture to draw on: pixels someone else owns, a format, a clip.
         *
         * @par Thread Safety
         * Not thread-safe: one drawer.
         */
        class Canvas {
                public:
                        Canvas() = default;

                        /** @brief Draws on @p width × @p height pixels at @p pixels, @p stride bytes a row. */
                        void attach(void *pixels, uint32_t stride, int32_t width, int32_t height, PixelFormat format);

                        uint8_t    *pixels() const { return _pixels; }
                        uint32_t    stride() const { return _stride; }
                        int32_t     width() const { return _width; }
                        int32_t     height() const { return _height; }
                        PixelFormat format() const { return _format; }
                        bool        isNull() const { return _pixels == nullptr; }

                        /** @brief Drawing stays inside @p clip (cut to the canvas). */
                        void setClip(const Rect &clip);
                        /** @brief Drawing anywhere on the canvas. */
                        void        clearClip() { setClip(Rect{0, 0, _width, _height}); }
                        const Rect &clip() const { return _clip; }

                        /** @brief The canvas's pixel value for @p color. */
                        uint32_t pixel(Color color) const;
                        /** @brief The colour of pixel value @p pixel. */
                        Color colorOf(uint32_t pixel) const;

                        void fill(const Rect &rect, Color color);
                        /** @brief @p color over what is there, @p alpha of it (0-255). */
                        void blend(const Rect &rect, Color color, uint8_t alpha);
                        /** @brief The pixels of @p from (the same size and format), the clip ignored. */
                        void copyFrom(const Canvas &from);
                        /** @brief The colour at (@p x, @p y); 0 outside. */
                        Color colorAt(int32_t x, int32_t y) const;

                private:
                        uint8_t    *_pixels = nullptr;
                        uint32_t    _stride = 0;
                        int32_t     _width = 0;
                        int32_t     _height = 0;
                        PixelFormat _format = PixelFormat::Rgb565;
                        Rect        _clip;
        };

} // namespace preso

#endif // PRESO_CANVAS_H
