/**
 * @file      painter.h
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
#include <bmds/font.h>
#include <bmds/rect.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class Surface;

/**
 * @brief Draws on a surface through a clip: a window's visible pieces
 *        (`devplan/windows.md` W8, §4a).
 *
 * Coordinates are the painter's own, from its origin (a window's client
 * area's top-left corner). Everything drawn is cut to the clip's pieces;
 * nothing outside them is touched. Fills of many rectangles of one color
 * go out as one 2D-DMA job. Text is drawn into a strip in internal RAM and
 * copied piece by piece.
 *
 * Surfaces take x and widths on 4-pixel columns: fills snap their left and
 * right edges outward to them, and text starts on one.
 *
 * The first error is kept (`error()`); drawing goes on regardless.
 *
 * @par Thread Safety
 * One task (the window manager's, in a redraw).
 */
class Painter {
        public:
                /** @brief A strip of internal DMA RAM, RGB888, for text. */
                struct Strip {
                                uint8_t *data = nullptr;
                                uint16_t width = 0;  ///< Pixels a row (its stride).
                                uint16_t height = 0; ///< Rows: at least a font's height.
                };

                /**
                 * @brief A painter on @p surface clipped to @p clip (surface
                 *        coordinates, disjoint, on 4-pixel columns), its origin
                 *        at (@p originX, @p originY) of the surface.
                 */
                Painter(Surface &surface, Span<const Rect> clip, uint16_t originX, uint16_t originY, const Strip &strip,
                        unsigned int timeoutMs)
                    : _surface(surface), _clip(clip), _strip(strip), _timeoutMs(timeoutMs), _originX(originX),
                      _originY(originY) {}

                Painter(const Painter &) = delete;
                Painter &operator=(const Painter &) = delete;

                /** @brief Returns the smallest rectangle holding the clip, in the painter's coordinates. */
                Rect clipBounds() const;

                /** @brief Returns true if any of @p rect is in the clip. */
                bool isVisible(const Rect &rect) const;

                /** @brief Fills @p rect with @p rgb (0xRRGGBB). @return The first error so far. */
                Error fill(const Rect &rect, uint32_t rgb) { return fill(Span<const Rect>(&rect, 1), rgb); }

                /** @brief Fills every rectangle of @p rects with @p rgb, as one job where they fit. */
                Error fill(Span<const Rect> rects, uint32_t rgb);

                /**
                 * @brief Writes @p utf8 at (@p x, @p y) in @p font, @p fg on @p bg,
                 *        at most @p maxWidth pixels of it (and the strip's width).
                 * @return The first error so far (`Invalid` if x, made the
                 *         surface's, isn't on a 4-pixel column).
                 */
                Error text(uint16_t x, uint16_t y, StringView utf8, const Font &font, uint32_t fg, uint32_t bg,
                           uint16_t maxWidth);

                /** @brief Returns the first error, or `Ok`. */
                Error error() const { return _error; }

                /** @brief Returns the 2D-DMA calls made (each a job or more). */
                uint32_t calls() const { return _calls; }

        private:
                static constexpr size_t BatchRects = 32;

                void note(Error err) {
                        if (_error.isOk()) _error = err;
                        return;
                }
                void flush(uint32_t rgb);

                Surface         &_surface;
                Span<const Rect> _clip;
                Strip            _strip;
                unsigned int     _timeoutMs;
                uint16_t         _originX;
                uint16_t         _originY;
                Rect             _batch[BatchRects];
                size_t           _batched = 0;
                uint32_t         _calls = 0;
                Error            _error;
};

BMDS_NAMESPACE_END
