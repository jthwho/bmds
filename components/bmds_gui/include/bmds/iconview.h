/**
 * @file      iconview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/icongrid.h>
#include <bmds/itemview.h>
#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Items as 32-pixel icons with their names under them in up to two
 *        lines (`devplan/filebrowser.md` §4g): flowing (rows left to right, as
 *        many columns as fit: a folder window), or in the cells of the
 *        desktop's grid, where they are dragged to other cells.
 *
 * A name longer than two lines ends in an ellipsis. The selection is drawn
 * filled while the view has the focus, outlined while not.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class IconView : public ItemView {
        public:
                /** @brief How the items are placed. */
                enum class Arrangement : uint8_t {
                        Flow, ///< Left to right, then down; scrolled down.
                        Grid, ///< In grid cells (`setCells()`), down then across: the desktop's.
                };

                /** @brief Told that rows were dragged to other cells (`cellOf()`). */
                using Moved = void (*)(void *arg, IconView &view);

                static constexpr uint16_t IconSide = 32;
                static constexpr uint16_t Pad = 4;        ///< Around the grid, and in a cell.
                static constexpr uint16_t LineWidth = 72; ///< A name's line at most (9 characters).
                static constexpr uint16_t LineHeight = 16;
                static constexpr size_t   LineBytes = 40; ///< A line kept (9 characters of up to 4 bytes).

                explicit IconView(Widget *parent);

                /** @brief Sets how the items are placed (laid out again). */
                void setArrangement(Arrangement arrangement);

                /** @brief Returns how the items are placed. */
                Arrangement arrangement() const { return _arrangement; }

                /** @brief Sets the colours: the background's and the names' (a folder window's light face; the desktop's). */
                void setColors(WindowPalette::Role background, WindowPalette::Role text);

                /**
                 * @brief On the grid: each row in the cell of @p wanted it wants if it can
                 *        (`IconGrid::place()`; @p wanted as many as the rows, or empty).
                 * @return `Ok` or `NoMem`.
                 */
                Error setCells(Span<const IconGrid::Cell> wanted);

                /** @brief Returns row @p row's grid cell. */
                IconGrid::Cell cellOf(size_t row) const { return row < _cells.size() ? _cells[row] : IconGrid::Cell(); }

                /**
                 * @brief Puts row @p row in the grid cell at (@p screenX, @p screenY) if it
                 *        is on the grid and free.
                 * @return True if it moved.
                 */
                bool placeRow(size_t row, uint16_t screenX, uint16_t screenY);

                void setMoved(Moved fn, void *arg) {
                        _moved = fn;
                        _movedArg = arg;
                        return;
                }

                Rect nameRect(size_t row) const override;

                /**
                 * @brief Draws what the view shows into @p pixels (B, G, R; a window's
                 *        reference picture for `win check`, the view's place in it).
                 */
                void drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                   const WindowPalette &palette) const;

        protected:
                int         rowAt(uint32_t x, uint32_t y) const override;
                bool        onName(size_t row, uint32_t x, uint32_t y) const override;
                ContentRect rowRect(size_t row) const override;
                size_t      neighbour(size_t row, int dx, int dy) const override;
                size_t      endRow(bool last) const override;
                size_t      typingOrder(size_t k) const override;
                bool        inRange(size_t row, size_t anchor, size_t to) const override;
                void        relayout() override;
                bool        dragged(int dx, int dy, bool done) override;
                void        dragCancelled() override;
                void        paintRows(Painter &painter, const Rect &viewport) override;

        public:
                /** @brief A name broken into its lines. */
                struct Lines {
                                char     text[2][LineBytes];
                                uint8_t  count;
                                uint16_t x[2]; ///< From the cell's left, on 4-pixel columns.
                };

                /** @brief Something the items are drawn on (the painter, or a reference picture). */
                class Canvas;

        private:
                void        lines(size_t row, Lines &out) const;
                ContentRect hitRect(size_t row, uint8_t lineCount) const;
                void        drawRow(Canvas &canvas, size_t row, int offsetX, int offsetY) const;
                void        drawAll(Canvas &canvas, const Rect &viewport) const;
                bool        isDragged(size_t row) const;
                void        invalidateRow(size_t row, int dx, int dy);

                Arrangement          _arrangement = Arrangement::Flow;
                WindowPalette::Role  _background = WindowPalette::Role::FaceHighlight;
                WindowPalette::Role  _text = WindowPalette::Role::FaceText;
                uint16_t             _columns = 1; // flowing: per line; on the grid: the grid's
                uint16_t             _gridRows = 1;
                List<IconGrid::Cell> _cells{MemSpace::Psram};
                List<uint32_t>       _order{MemSpace::Psram}; // on the grid: the rows down then across
                int                  _dragX = 0;              // a drag's offset, while under way
                int                  _dragY = 0;
                bool                 _draggingCells = false;
                Moved                _moved = nullptr;
                void                *_movedArg = nullptr;
};

BMDS_NAMESPACE_END
