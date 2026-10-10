/**
 * @file      tablemodel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/itemmodel.h>
#include <bmds/list.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A table the System Monitor fills again each refresh: rows of texts,
 *        each with its emphasis (a problem's colour) and a key (a pid, a task
 *        number, a counter's id) to find the selection again.
 *
 * @par Thread Safety
 * Not thread-safe: the window's loop.
 */
class TableModel final : public ItemModel {
        public:
                /** @brief Most columns (a details view's). */
                static constexpr size_t MaxColumns = 8;

                /** @brief A column. */
                struct Column {
                                const char *title;
                                uint16_t    width;
                                Align       align;
                };

                /** @brief Sets the columns (`MaxColumns` at most; the titles kept by the caller). */
                void setColumns(Span<const Column> columns);

                /** @brief Removes every row. */
                void clear();

                /**
                 * @brief Adds a row of @p texts (a column each) with @p emphasis and @p key.
                 * @return `Ok` or `NoMem`.
                 */
                Error add(Span<const StringView> texts, Emphasis emphasis = Emphasis::None, int64_t key = 0);

                /** @brief Returns row @p row's key. */
                int64_t key(size_t row) const { return _rows[row].key; }

                /** @brief Returns the row whose key is @p key, or -1. */
                int find(int64_t key) const;

                size_t      count() const override { return _rows.size(); }
                StringView  text(size_t row) const override { return cell(row, 0); }
                const Icon *icon(size_t row, uint16_t size) const override {
                        (void)row;
                        (void)size;
                        return nullptr;
                }
                size_t     columns() const override { return _columnCount > 0 ? _columnCount : 1; }
                StringView columnTitle(size_t column) const override;
                StringView columnText(size_t row, size_t column, Span<char> buffer) const override;
                Align      columnAlign(size_t column) const override;
                uint16_t   columnWidth(size_t column) const override;
                Emphasis   emphasis(size_t row) const override { return _rows[row].emphasis; }

        private:
                struct Row {
                                uint32_t at[MaxColumns] = {};
                                uint16_t length[MaxColumns] = {};
                                int64_t  key = 0;
                                Emphasis emphasis = Emphasis::None;
                };

                StringView cell(size_t row, size_t column) const;

                List<Row>  _rows{MemSpace::Psram};
                List<char> _texts{MemSpace::Psram};
                Column     _columns[MaxColumns] = {};
                size_t     _columnCount = 0;
};

BMDS_NAMESPACE_END
