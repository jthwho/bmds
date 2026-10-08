/**
 * @file      treerows.h
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
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A tree's visible rows, as one flat list (`devplan/filebrowser.md`
 *        §4g), pure.
 *
 * A row's children follow it, one deeper; expanding inserts them, collapsing
 * removes its descendants (a run). Only what is visible is held: a collapsed
 * row's children are read again when it is expanded. A row may have children
 * (`HasChildren`, assumed until it is expanded and found to have none). The
 * names are kept in one PSRAM character list, compacted when mostly garbage.
 *
 * @par Thread Safety
 * A value with one owner.
 */
class TreeRows {
        public:
                /** @brief A row's flags. */
                enum Flag : uint8_t {
                        Expanded = 1u << 0,    ///< Its children are shown.
                        HasChildren = 1u << 1, ///< It has children, or may (not read yet).
                };

                /** @brief A row. */
                struct Row {
                                uint32_t  nameAt = 0;
                                uint16_t  nameLength = 0;
                                uint8_t   depth = 0;
                                uint8_t   flags = HasChildren;
                                uintptr_t data = 0; ///< The owner's.
                };

                /** @brief Adds a top-level row (a root: its name a path, `/sd0/p0`). @return `Ok` or `NoMem`. */
                Error addRoot(StringView name, uintptr_t data = 0);

                /**
                 * @brief Shows @p names as @p row's children, expanding it; none: it
                 *        has no children.
                 *
                 * A child shown before and named again keeps what is shown below it
                 * (a folder read again keeps its subfolders open); the others go.
                 * @return `Ok`, `OutOfRange`, or `NoMem` (as it was).
                 */
                Error setChildren(size_t row, Span<const StringView> names);

                /** @brief Hides @p row's descendants. */
                void collapse(size_t row);

                /** @brief Removes every row. */
                void clear();

                /** @brief Returns the rows. */
                size_t count() const { return _rows.size(); }

                /** @brief Returns row @p row (unchecked). */
                const Row &row(size_t row) const { return _rows[row]; }

                /** @brief Sets row @p row's data. */
                void setData(size_t row, uintptr_t data) {
                        if (row < _rows.size()) _rows[row].data = data;
                        return;
                }

                /** @brief Returns row @p row's name. */
                StringView name(size_t row) const {
                        const Row &r = _rows[row];
                        return StringView(_names.data() + r.nameAt, r.nameLength);
                }

                /** @brief Returns @p row's parent, or -1 (a root). */
                int parent(size_t row) const;

                /** @brief Returns the row after @p row's last descendant. */
                size_t endOf(size_t row) const;

                /** @brief Writes @p row's path into @p out: its root's name, then each name, joined by `/`. */
                Error pathOf(size_t row, String &out) const;

                /** @brief Returns the row whose path is @p path, or -1 (not shown). */
                int find(StringView path) const;

        private:
                Error store(StringView name, Row &row); // the name into _names
                void  compact();                        // _names without the removed rows' names

                List<Row>  _rows{MemSpace::Psram};
                List<char> _names{MemSpace::Psram};
                size_t     _live = 0; // bytes of _names still named by a row
};

BMDS_NAMESPACE_END
