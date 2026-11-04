/**
 * @file      canvas.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Slides are drawn ahead of time, so these aren't the hot path: a step is the platform's copy.
 * Fills still go a word at a time.
 */

#include "canvas.h"

namespace preso {

        Rect Rect::intersected(const Rect &other) const {
                int32_t left = x > other.x ? x : other.x;
                int32_t top = y > other.y ? y : other.y;
                int32_t right = x + width < other.x + other.width ? x + width : other.x + other.width;
                int32_t bottom = y + height < other.y + other.height ? y + height : other.y + other.height;
                if (right <= left || bottom <= top) return Rect{left, top, 0, 0};
                return Rect{left, top, right - left, bottom - top};
        }

        Rect uniteRects(const Rect &a, const Rect &b) {
                if (a.isEmpty()) return b;
                if (b.isEmpty()) return a;
                int32_t x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
                int32_t x1 = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
                int32_t y1 = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
                return Rect{x0, y0, x1 - x0, y1 - y0};
        }

        void Canvas::attach(void *pixels, uint32_t stride, int32_t width, int32_t height, PixelFormat format) {
                _pixels = static_cast<uint8_t *>(pixels);
                _stride = stride;
                _width = width;
                _height = height;
                _format = format;
                clearClip();
                return;
        }

        void Canvas::setClip(const Rect &clip) {
                _clip = clip.intersected(Rect{0, 0, _width, _height});
                return;
        }

        uint32_t Canvas::pixel(Color color) const {
                if (_format == PixelFormat::Rgb565) {
                        return ((color >> 8) & 0xF800) | ((color >> 5) & 0x07E0) | ((color >> 3) & 0x001F);
                }
                return color & 0xFFFFFF;
        }

        Color Canvas::colorOf(uint32_t p) const {
                if (_format == PixelFormat::Rgb565) {
                        uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                        return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
                }
                return p & 0xFFFFFF;
        }

        void Canvas::fill(const Rect &rect, Color color) {
                Rect r = rect.intersected(_clip);
                if (r.isEmpty()) return;
                uint32_t p = pixel(color);
                for (int32_t y = r.y; y < r.y + r.height; y++) {
                        uint8_t *row = _pixels + size_t(y) * _stride;
                        if (_format == PixelFormat::Rgb565) {
                                auto   *q = reinterpret_cast<uint16_t *>(row) + r.x;
                                int32_t n = r.width;
                                // To a word boundary, then two pixels a word.
                                if ((reinterpret_cast<uintptr_t>(q) & 2) != 0 && n > 0) {
                                        *q++ = uint16_t(p);
                                        n--;
                                }
                                auto    *w = reinterpret_cast<uint32_t *>(q);
                                uint32_t pair = p | (p << 16);
                                for (int32_t i = 0; i < n / 2; i++) w[i] = pair;
                                if ((n & 1) != 0) q[n - 1] = uint16_t(p);
                        } else {
                                auto *q = reinterpret_cast<uint32_t *>(row) + r.x;
                                for (int32_t i = 0; i < r.width; i++) q[i] = p;
                        }
                }
                return;
        }

        void Canvas::blend(const Rect &rect, Color color, uint8_t alpha) {
                if (alpha == 255) {
                        fill(rect, color);
                        return;
                }
                Rect r = rect.intersected(_clip);
                if (r.isEmpty() || alpha == 0) return;
                for (int32_t y = r.y; y < r.y + r.height; y++) {
                        for (int32_t x = r.x; x < r.x + r.width; x++) {
                                Color    under = colorAt(x, y);
                                Color    mixed = mixColors(under, color, uint32_t(alpha) + (alpha >> 7));
                                uint8_t *row = _pixels + size_t(y) * _stride;
                                if (_format == PixelFormat::Rgb565) {
                                        reinterpret_cast<uint16_t *>(row)[x] = uint16_t(pixel(mixed));
                                } else {
                                        reinterpret_cast<uint32_t *>(row)[x] = pixel(mixed);
                                }
                        }
                }
                return;
        }

        void Canvas::copyFrom(const Canvas &from) {
                if (from._width != _width || from._height != _height || from._format != _format) return;
                uint32_t bytes = uint32_t(_width) * bytesPerPixel(_format);
                for (int32_t y = 0; y < _height; y++) {
                        memcpy(_pixels + size_t(y) * _stride, from._pixels + size_t(y) * from._stride, bytes);
                }
                return;
        }

        Color Canvas::colorAt(int32_t x, int32_t y) const {
                if (x < 0 || y < 0 || x >= _width || y >= _height) return 0;
                const uint8_t *row = _pixels + size_t(y) * _stride;
                if (_format == PixelFormat::Rgb565) return colorOf(reinterpret_cast<const uint16_t *>(row)[x]);
                return colorOf(reinterpret_cast<const uint32_t *>(row)[x]);
        }

} // namespace preso
