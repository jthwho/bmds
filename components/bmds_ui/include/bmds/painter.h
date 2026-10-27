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
#include <bmds/bitmapfont.h>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/icon.h>
#include <bmds/rect.h>
#include <bmds/region.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/textdraw.h>
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
 * Text is expanded into the next free columns of a strip in internal RAM
 * while the 2D-DMA copies what was made before; it starts on a 4-pixel
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
                 *        Pieces are made in the strip side by side, left to right, half
                 *        after half; a piece's columns are reused only after a wait for
                 *        the surface (a batch's pieces stay until it runs: windows.md §4n).
                 *        Whoever waits for the surface may set the position back to 0.
                 */
                struct Tools {
                                uint8_t             *strip = nullptr; ///< Two halves, one after the other.
                                uint16_t             halfWidth = 0;   ///< Pixels a half's row (a multiple of 64).
                                uint16_t             height = 0;      ///< Rows: at least a font's height.
                                GlyphRenderer       *renderer = nullptr;
                                const WindowPalette *palette = nullptr; ///< The window manager's colors.
                                uint8_t              stripHalf = 0;   ///< The half the next piece goes in (2: used up).
                                uint16_t             stripColumn = 0; ///< And its first free column there.
                                bool                 smooth = true;   ///< TrueType text smoothed (`font/smooth`).
                                TextDraw::Scratch   *textScratch = nullptr; ///< `TextDraw`'s memory (not the stack).
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

                /** @brief Returns the clip's rectangles (disjoint). */
                size_t clipCount() const { return _clip.size(); }

                /** @brief Returns clip rectangle @p i, in the painter's coordinates. */
                Rect clipAt(size_t i) const { return local(_clip[i]); }

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
                Error text(int x, int y, StringView utf8, const BitmapFont &font, uint32_t fg, uint32_t bg,
                           uint16_t maxWidth);

                /**
                 * @brief Writes @p utf8 at (@p x, @p y) (the line's top) in @p font, @p fg on a
                 *        box of @p bg (the advance of what fits in @p maxWidth pixels rounded up to
                 *        a 4-pixel column, the line height), smoothed as `Tools::smooth` says (`devplan/fonts.md` §4d,
                 *        Opaque). A bitmap font at its own size takes `GlyphRenderer`'s path;
                 *        the rest is blended into the strip a piece at a time, in bands of its
                 *        rows. Nothing is read back. @p x may be off a 4-pixel column: the box
                 *        then starts on the column left of it, @p bg in the pixels between.
                 * @return The first error so far.
                 * @hotpath
                 */
                Error text(int x, int y, StringView utf8, const Font &font, uint32_t fg, uint32_t bg,
                           uint16_t maxWidth);

                /** @brief A run of a line's text in its colors: bytes up to `end` (from the last run's). */
                struct TextRun {
                                uint32_t end = 0;
                                uint32_t fg = 0;
                                uint32_t bg = 0;
                };

                /** @brief Most runs a line takes. */
                static constexpr size_t MaxRuns = 32;

                /**
                 * @brief As `text()`, a line in @p runs of colors (a selection's three, syntax
                 *        colors): each run's columns filled with its bg and the line blended in its
                 *        fg, clipped to them, so a run may start at any pixel. The last run's bg
                 *        fills to the box's end. A bitmap font at its own size draws each run as
                 *        its own `text()` (8-pixel cells).
                 * @return As `text()`; `Invalid` for no runs or more than `MaxRuns`.
                 * @hotpath
                 */
                Error text(int x, int y, StringView utf8, const Font &font, Span<const TextRun> runs,
                           uint16_t maxWidth);

                /**
                 * @brief Writes @p cells side by side from (@p x, @p y) in @p font,
                 *        each in its colors and style, at most @p maxWidth pixels.
                 * @return As `text()`.
                 * @hotpath
                 */
                Error cells(int x, int y, Span<const TextCell> cells, const BitmapFont &font, uint16_t maxWidth);

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
                 *        2D-DMA: no CPU a pixel (`devplan/images.md` §4b) for its whole
                 *        4-pixel columns. A picture off the columns (a widget in a margin)
                 *        has its edges' few columns patched (`Surface::patch()`: two jobs
                 *        for each 320 rows of an edge). It may lie partly off the surface.
                 *        Returns once copied: the picture may change after.
                 * @return The first error so far (`Invalid`: a bad stride).
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
                 * @brief Makes the next piece of a row band in the strip: @p column
                 *        (from the band's left) onward, at most @p room pixels of each
                 *        of the band's rows, @p stride bytes apart, at @p strip.
                 * @return The pixels made across: a multiple of 4 unless the band's
                 *         last; 0 to stop.
                 */
                using RowMaker = uint16_t (*)(void *arg, uint8_t *strip, size_t stride, uint16_t column, uint16_t room);

                /**
                 * @brief Draws a band of @p width × @p height pixels (@p height at most
                 *        `stripRows()`) at (@p x, @p y), each row shown @p repeat times
                 *        down (the 2D-DMA reading it again), what of it shows, made by
                 *        @p make in the window manager's strip a piece at a time: one
                 *        piece made while the 2D-DMA copies the last: B, G, R pixels, or
                 *        RGB565 if @p rgb565 (widened by the 2D-DMA). @p x (made the
                 *        surface's) and @p width are on 4-pixel columns. Returns with
                 *        the last piece's copy started; whatever @p make read is free.
                 * @return The first error so far (`Invalid`: off the columns, too tall, no
                 *         repeat).
                 * @blocking A piece's copy.
                 */
                Error rows(int x, int y, uint16_t width, uint16_t height, uint16_t repeat, bool rgb565, RowMaker make,
                           void *arg);

                /**
                 * @brief Fills @p rect (x, made the surface's, and width on 4-pixel columns) with a
                 *        tile repeated across and down: @p width × @p height B, G, R pixels
                 *        (@p width a multiple of 4; @p stride bytes a row; PSRAM or internal RAM),
                 *        each row shown @p repeat times down, its top-left pixel at (@p tileX,
                 *        @p tileY) (the painter's coordinates; @p rect.x minus @p tileX a multiple
                 *        of 4). What of it shows is copied by the 2D-DMA straight from the tile
                 *        (`Surface::startTile()`): no CPU a pixel, no strip. Returns once copied:
                 *        the tile may change after.
                 * @return The first error so far (`Invalid`: off the columns, a bad tile).
                 * @blocking The copy.
                 */
                Error tiled(const Rect &rect, const uint8_t *bgr, size_t stride, uint16_t width, uint16_t height,
                            uint16_t repeat, int tileX, int tileY);

                /** @brief Returns the most rows `rows()` takes. */
                uint16_t stripRows() const { return _tools.height; }

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
                // Writes @p edge (surface coordinates, any x and width) of the picture at (@p sx,
                // @p sy) by Surface::patch(), in each clip piece.
                void patchPixels(const Rect &edge, int32_t sx, int32_t sy, const uint8_t *data, size_t stride,
                                 bool rgb565);
                static Rect                    bounds(Span<const Rect> rects);
                Rect                           local(const Rect &r) const;
                void                           flush(uint32_t rgb);
                template <typename Draw>
                Error run(int x, int y, uint16_t maxWidth, uint16_t height, Draw draw, bool rgb565 = false,
                          uint16_t repeat = 1);

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
