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
#include <bmds/glyphrenderer.h>
#include <bmds/icon.h>
#include <bmds/rect.h>
#include <bmds/region.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

class Surface;

/**
 * @brief Draws on a surface through a clip: a window's visible pieces
 *        (`devplan/windows.md` W8, §4a, §4d).
 *
 * Coordinates are the painter's own, from its origin (a window's client
 * area's top-left corner). Everything drawn is cut to the clip's pieces;
 * nothing outside them is touched. The damage is what must be redrawn (the
 * whole clip, or less when the window asked for an update and the clip is
 * all of it that shows).
 *
 * Fills take any rectangle and go out as one 2D-DMA job per 32 pieces
 * (sides off the 4-pixel columns cost a read and a write of their groups).
 * Text is expanded into one half of a strip in internal RAM while the
 * 2D-DMA copies the other half into the surface; it starts on a 4-pixel
 * column. Scrolling moves pixels inside the surface.
 *
 * The first error is kept (`error()`); drawing goes on regardless.
 *
 * @par Thread Safety
 * One task (the window manager's, in a redraw).
 */
class Painter {
        public:
                /**
                 * @brief What painters share: the strip (two halves of internal DMA
                 *        RAM, RGB888) and the glyph renderer, kept by the window manager.
                 */
                struct Tools {
                                uint8_t             *strip = nullptr; ///< Two halves, one after the other.
                                uint16_t             halfWidth = 0;   ///< Pixels a half's row (a multiple of 64).
                                uint16_t             height = 0;      ///< Rows: at least a font's height.
                                GlyphRenderer       *renderer = nullptr;
                                const WindowPalette *palette = nullptr; ///< The window manager's colors.
                                int8_t               busyHalf = -1;     ///< The half a started copy reads, or -1.
                };

                /**
                 * @brief A painter on @p surface clipped to @p clip and with
                 *        @p damage to redraw (surface coordinates, disjoint, on
                 *        4-pixel columns), its origin at (@p originX, @p originY).
                 */
                Painter(Surface &surface, Span<const Rect> clip, Span<const Rect> damage, uint16_t originX,
                        uint16_t originY, Tools &tools, unsigned int timeoutMs)
                    : _surface(surface), _clip(clip), _damage(damage), _tools(tools), _timeoutMs(timeoutMs),
                      _originX(originX), _originY(originY) {}

                /**
                 * @brief A painter for what of @p limit (@p outer's coordinates; its
                 *        x and width cut inward to 4-pixel columns) @p outer draws:
                 *        @p outer's clip and damage cut to it, into @p storage
                 *        (`clippedStorage()` rectangles of @p outer's suffice; past
                 *        them, pieces are left out and `Full` noted). The origin and
                 *        tools are @p outer's, and its calls, first error and
                 *        `defer()` go to @p outer when it is destroyed. Used while
                 *        @p outer isn't.
                 */
                Painter(Painter &outer, const Rect &limit, Span<Rect> storage);

                ~Painter();

                Painter(const Painter &) = delete;
                Painter &operator=(const Painter &) = delete;

                /** @brief Returns the storage a clipped painter of this one needs at most, in rectangles. */
                size_t clippedStorage() const { return _clip.size() + _damage.size(); }

                /** @brief Returns the smallest rectangle holding the clip, in the painter's coordinates. */
                Rect clipBounds() const { return local(bounds(_clip)); }

                /** @brief Returns the damage's rectangles. */
                size_t damageCount() const { return _damage.size(); }

                /** @brief Returns damage rectangle @p i, in the painter's coordinates. */
                Rect damageAt(size_t i) const { return local(_damage[i]); }

                /** @brief Returns the smallest rectangle holding the damage, in the painter's coordinates. */
                Rect damageBounds() const { return local(bounds(_damage)); }

                /** @brief Returns true if any of @p rect is in the clip. */
                bool isVisible(const Rect &rect) const;

                /** @brief Fills @p rect with @p rgb (0xRRGGBB). @return The first error so far. */
                Error fill(const Rect &rect, uint32_t rgb) { return fill(Span<const Rect>(&rect, 1), rgb); }

                /** @brief Fills every rectangle of @p rects with @p rgb, as one job where they fit. */
                Error fill(Span<const Rect> rects, uint32_t rgb);

                /**
                 * @brief Writes @p utf8 at (@p x, @p y) in @p font, @p fg on @p bg,
                 *        at most @p maxWidth pixels of it. The position may be left of
                 *        or above the surface: what of the row is on it is drawn.
                 * @return The first error so far (`Invalid` if x, made the
                 *         surface's, isn't on a 4-pixel column).
                 * @hotpath
                 */
                Error text(int x, int y, StringView utf8, const Font &font, uint32_t fg, uint32_t bg,
                           uint16_t maxWidth);

                /**
                 * @brief Writes @p cells side by side from (@p x, @p y) in @p font,
                 *        each in its colors and style, at most @p maxWidth pixels.
                 * @return As `text()`.
                 * @hotpath
                 */
                Error cells(int x, int y, Span<const TextCell> cells, const Font &font, uint16_t maxWidth);

                /**
                 * @brief Draws @p icon with its top-left corner at (@p x, @p y) (x on a
                 *        4-pixel column), its alpha blended over @p bg (0xRRGGBB: the
                 *        face it sits on; nothing is read back from the surface).
                 *        Any height, in bands of the strip's; as `text()`, it may
                 *        start off the surface.
                 */
                Error image(int x, int y, const Icon &icon, uint32_t bg);

                /**
                 * @brief Copies a picture's @p width × @p height B, G, R pixels (the
                 *        surface's order; @p stride bytes a row, a multiple of 3; PSRAM
                 *        or internal RAM) to (@p x, @p y), what of it shows, by the
                 *        2D-DMA: no CPU a pixel (`devplan/images.md` §4b). @p x (made
                 *        the surface's) and @p width are on 4-pixel columns; it may lie
                 *        partly off the surface. Returns once copied: the picture may
                 *        change after.
                 * @return The first error so far (`Invalid`: off the columns, a bad stride).
                 * @blocking The copy.
                 */
                Error pixels(int x, int y, const uint8_t *bgr, size_t stride, uint16_t width, uint16_t height);

                /**
                 * @brief Says @p rect (the painter's coordinates) will be drawn whole, every pixel
                 *        that shows, by the calls that follow (`Surface::willDraw()`): a picture
                 *        drawn in strips over what changed since the last present costs no copies.
                 * @return The first error so far.
                 */
                Error cover(const Rect &rect);

                /**
                 * @brief As `pixels()`, from RGB565 pixels (@p stride bytes a row, even),
                 *        widened by the 2D-DMA on the way.
                 * @return As `pixels()`.
                 * @blocking The copy.
                 */
                Error pixels565(int x, int y, const uint8_t *rgb565, size_t stride, uint16_t width, uint16_t height);

                /**
                 * @brief Moves what shows of @p area by @p dy rows (down if
                 *        positive), inside the surface. A pixel is moved only if
                 *        its source shows and isn't damaged; @p area's other
                 *        pixels (newly uncovered rows among them) are returned, to
                 *        be redrawn with the damage. Columns are 4-pixel ones.
                 * @return The pixels to redraw, in the painter's coordinates.
                 */
                Region scroll(const Rect &area, int dy);

                /** @brief Returns the window manager's colors. */
                const WindowPalette &palette() const { return *_tools.palette; }

                /** @brief Returns the first error, or `Ok`. */
                Error error() const { return _error; }

                /** @brief Returns the 2D-DMA calls made (each a job or more). */
                uint32_t calls() const { return _calls; }

                /**
                 * @brief Says the window didn't draw (its owner is busy): what it
                 *        was asked to draw is asked again at the next round, no more.
                 * @nonblocking
                 */
                void defer() {
                        _deferred = true;
                        return;
                }

                /** @brief Returns true if `defer()` was called. */
                bool isDeferred() const { return _deferred; }

        private:
                static constexpr size_t BatchRects = 32;

                void note(Error err) {
                        if (_error.isOk()) _error = err;
                        return;
                }
                Error copyPixels(int x, int y, const uint8_t *data, size_t stride, uint16_t width, uint16_t height,
                                 bool rgb565);
                static Rect                    bounds(Span<const Rect> rects);
                Rect                           local(const Rect &r) const;
                void                           flush(uint32_t rgb);
                template <typename Draw> Error run(int x, int y, uint16_t maxWidth, uint16_t height, Draw draw);

                Surface         &_surface;
                Span<const Rect> _clip;
                Span<const Rect> _damage;
                Tools           &_tools;
                unsigned int     _timeoutMs;
                uint16_t         _originX;
                uint16_t         _originY;
                Rect             _batch[BatchRects];
                size_t           _batched = 0;
                uint32_t         _calls = 0;
                bool             _deferred = false;
                Error            _error;
                Painter         *_outer = nullptr; // a clipped painter's
};

BMDS_NAMESPACE_END
