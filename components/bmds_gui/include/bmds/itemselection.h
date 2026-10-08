/**
 * @file      itemselection.h
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
#include <bmds/list.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Which rows of a view are selected, the current row and the anchor
 *        of a Shift range (`devplan/filebrowser.md` §4g), pure.
 *
 * A bit per row, in PSRAM. Rows run from 0 to `count()`; the current row and
 * the anchor are -1 when there are none.
 *
 * @par Thread Safety
 * A value with one owner.
 */
class ItemSelection {
        public:
                /** @brief Sets the rows (none selected; the current row and anchor gone). @return `Ok` or `NoMem`. */
                Error reset(size_t count);

                /** @brief Returns the rows. */
                size_t count() const { return _count; }

                /** @brief Returns true if @p row is selected. */
                bool isSelected(size_t row) const {
                        return row < _count && (_bits[row / 32] & (1u << (row % 32))) != 0;
                }

                /** @brief Returns the selection's bits for rows 32 @p i to 32 @p i + 31 (a view's redraw). */
                uint32_t word(size_t i) const { return i < _bits.size() ? _bits[i] : 0; }

                /** @brief Returns the rows selected. */
                size_t selectedCount() const { return _selected; }

                /** @brief Returns the first selected row at or after @p from, or -1. */
                int nextSelected(size_t from) const;

                /** @brief Returns the current row (the keyboard's), or -1. */
                int current() const { return _current; }

                /** @brief Returns the anchor of a Shift range, or -1. */
                int anchor() const { return _anchor; }

                /** @brief Sets the current row (and the anchor) without changing the selection. */
                void setCurrent(int row, bool moveAnchor = true);

                /** @brief Selects only @p row (-1: none), the current row and anchor too. */
                void selectOnly(int row);

                /** @brief Toggles @p row (Ctrl), making it current and the anchor. */
                void toggle(size_t row);

                /** @brief Selects or deselects @p row alone. */
                void set(size_t row, bool selected);

                /**
                 * @brief Selects the rows from the anchor to @p row (Shift), the others
                 *        cleared unless @p add (Ctrl+Shift); @p row becomes current.
                 */
                void selectRange(size_t row, bool add);

                /** @brief Tells whether row @p row is between the anchor @p anchor and @p to. */
                using InRange = bool (*)(const void *arg, size_t row, size_t anchor, size_t to);

                /**
                 * @brief `selectRange()` by @p inRange's rule instead of the rows' order (a grid's
                 *        rectangle of cells).
                 */
                void selectRange(size_t row, bool add, InRange inRange, const void *arg);

                /** @brief Selects every row. */
                void selectAll();

                /** @brief Deselects every row (the current row stays). */
                void clear();

        private:
                List<uint32_t> _bits{MemSpace::Psram};
                size_t         _count = 0;
                size_t         _selected = 0;
                int            _current = -1;
                int            _anchor = -1;
};

BMDS_NAMESPACE_END
