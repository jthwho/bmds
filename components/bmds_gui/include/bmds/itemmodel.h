/**
 * @file      itemmodel.h
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
#include <bmds/icon.h>
#include <bmds/list.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What an item view shows: rows with a name, an icon and columns
 *        (`devplan/filebrowser.md` §4g). The owner implements it.
 *
 * There are no change signals: the owner changes its model, then tells its
 * views (`ItemView::modelReset()`).
 *
 * @par Thread Safety
 * As its views: the application's lock.
 */
class ItemModel {
        public:
                /** @brief Where a column's text goes. */
                enum class Align : uint8_t {
                        Left,
                        Right,
                };

                virtual ~ItemModel() = default;

                /** @brief Returns the rows. */
                virtual size_t count() const = 0;

                /** @brief Returns row @p row's name. */
                virtual StringView text(size_t row) const = 0;

                /** @brief Returns row @p row's icon @p size pixels square (null: none yet). */
                virtual const Icon *icon(size_t row, uint16_t size) const = 0;

                /** @brief Returns the columns of a details view (the name is the first). */
                virtual size_t columns() const { return 1; }

                /** @brief Returns column @p column's title. */
                virtual StringView columnTitle(size_t column) const {
                        (void)column;
                        return "Name";
                }

                /** @brief Returns column @p column's text for @p row, written into @p buffer if need be. */
                virtual StringView columnText(size_t row, size_t column, Span<char> buffer) const {
                        (void)buffer;
                        return column == 0 ? text(row) : StringView();
                }

                /** @brief Returns where column @p column's text goes. */
                virtual Align columnAlign(size_t column) const {
                        (void)column;
                        return Align::Left;
                }

                /** @brief Returns column @p column's width at first, in pixels. */
                virtual uint16_t columnWidth(size_t column) const { return column == 0 ? 200 : 100; }

                /** @brief How a row stands out: a problem's colour (`devplan/sysmon.md` §3.5). */
                enum class Emphasis : uint8_t {
                        None,
                        Warning,  ///< Its text amber.
                        Critical, ///< Its text red.
                };

                /** @brief Returns how row @p row stands out (a details view's). */
                virtual Emphasis emphasis(size_t row) const {
                        (void)row;
                        return Emphasis::None;
                }
};

/**
 * @brief A plain model: names (copied) and icons (the caller's, one for both
 *        sizes or one each), and columns of text.
 *
 * @par Thread Safety
 * As `ItemModel`.
 */
class ListItemModel : public ItemModel {
        public:
                /** @brief Most columns. */
                static constexpr size_t MaxColumns = 6;

                /** @brief Removes every row. */
                void clear();

                /**
                 * @brief Adds a row named @p name with @p small (16-pixel) and @p large
                 *        (32-pixel) icons (either may be null) and the other
                 *        columns' texts.
                 * @return `Ok` or `NoMem`.
                 */
                Error add(StringView name, const Icon *small, const Icon *large,
                          Span<const StringView> columnTexts = Span<const StringView>());

                /** @brief Removes row @p row (its texts' bytes kept until the model is emptied). */
                void removeAt(size_t row);

                /** @brief Sets row @p row's icons. */
                void setIcons(size_t row, const Icon *small, const Icon *large);

                /** @brief Sets the columns' titles, widths and alignment (`MaxColumns` at most). */
                void setColumns(Span<const StringView> titles, Span<const uint16_t> widths, Span<const Align> aligns);

                size_t      count() const override { return _rows.size(); }
                StringView  text(size_t row) const override { return view(_rows[row].text[0]); }
                const Icon *icon(size_t row, uint16_t size) const override;
                size_t      columns() const override { return _columns > 0 ? _columns : 1; }
                StringView  columnTitle(size_t column) const override;
                StringView  columnText(size_t row, size_t column, Span<char> buffer) const override;
                Align       columnAlign(size_t column) const override;
                uint16_t    columnWidth(size_t column) const override;

        private:
                struct Text {
                                uint32_t at = 0;
                                uint16_t length = 0;
                };

                struct Row {
                                Text        text[MaxColumns];
                                const Icon *small = nullptr;
                                const Icon *large = nullptr;
                };

                StringView view(const Text &t) const { return StringView(_texts.data() + t.at, t.length); }
                Error      store(StringView s, Text &out);

                List<Row>  _rows{MemSpace::Psram};
                List<char> _texts{MemSpace::Psram};
                Text       _titles[MaxColumns];
                List<char> _titleTexts{MemSpace::Psram};
                uint16_t   _widths[MaxColumns] = {};
                Align      _aligns[MaxColumns] = {};
                size_t     _columns = 0;
};

BMDS_NAMESPACE_END
