/**
 * @file      detailsview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/itemview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Items in rows under a header of columns (`devplan/filebrowser.md`
 *        §4g): a 16-pixel icon and the name, then the model's other columns.
 *
 * The header's buttons show the columns' titles and the sort (a small
 * triangle); a click on one asks for that sort (`SortAsked`), and dragging a
 * column's right edge resizes it. The header scrolls across with the rows.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class DetailsView : public ItemView {
        public:
                /** @brief Told that column @p column was clicked (the owner sorts, then shows it). */
                using SortAsked = void (*)(void *arg, DetailsView &view, size_t column);

                static constexpr uint16_t RowHeight = 18;
                static constexpr uint16_t HeaderHeight = 20;
                static constexpr size_t   MaxColumns = 8;

                explicit DetailsView(Widget *parent);

                /** @brief Shows or hides the header (shown by default; a list of one column without one: a sidebar). */
                void setHeaderVisible(bool visible);

                /** @brief Shows the sort on column @p column (-1: none), down or up. */
                void setSortIndicator(int column, bool descending);

                /** @brief Returns column @p column's width. */
                uint16_t columnWidth(size_t column) const { return column < _columns ? _widths[column] : 0; }

                /** @brief Sets column @p column's width (on 4-pixel columns, a least kept). */
                void setColumnWidth(size_t column, uint16_t width);

                void setSortAsked(SortAsked fn, void *arg) {
                        _sortAsked = fn;
                        _sortArg = arg;
                        return;
                }

                Rect                   nameRect(size_t row) const override;
                void                   paint(Painter &painter) override;
                void                   mousePressed(const WindowManager::MouseEvent &event) override;
                void                   mouseMoved(const WindowManager::MouseEvent &event) override;
                void                   mouseReleased(const WindowManager::MouseEvent &event) override;
                WindowManager::Pointer pointerAt(int x, int y) const override;

        protected:
                DragImage::Box dragBox(size_t row) const override;
                int         rowAt(uint32_t x, uint32_t y) const override;
                bool        onName(size_t row, uint32_t x, uint32_t y) const override;
                ContentRect rowRect(size_t row) const override;
                size_t      neighbour(size_t row, int dx, int dy) const override;
                void        relayout() override;
                void        scrolled() override;
                void        paintRows(Painter &painter, const Rect &viewport) override;

        private:
                Rect     header() const; // the header's strip (window coordinates)
                bool     inHeader(int x, int y) const;
                void     paintHeader(Painter &painter);
                uint32_t total() const;

                uint32_t _headerX = 0; // the scroll across the header was drawn at

                uint16_t  _widths[MaxColumns] = {};
                size_t    _columns = 0;
                int       _sortColumn = -1;
                bool      _descending = false;
                int       _pressColumn = -1; // its button pressed
                bool      _pressOver = false;
                int       _resizing = -1; // its edge dragged
                int       _resizeX = 0;
                uint16_t  _resizeStart = 0;
                SortAsked _sortAsked = nullptr;
                void     *_sortArg = nullptr;
};

BMDS_NAMESPACE_END
