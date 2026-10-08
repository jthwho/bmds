/**
 * @file      folderitems.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/foldermodel.h>
#include <bmds/itemmodel.h>

#include "fileicons.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief A folder window's items (`devplan/filebrowser.md` §4h): a
 *        `FolderModel` as the views' `ItemModel`, with each type's icon and the
 *        columns Name, Size, Type and Modified.
 *
 * @par Thread Safety
 * The panel's loop.
 */
class FolderItems final : public ItemModel {
        public:
                /** @brief The columns. */
                enum Column : size_t {
                        NameColumn,
                        SizeColumn,
                        TypeColumn,
                        ModifiedColumn,
                        Columns,
                };

                explicit FolderItems(FileIcons &icons) : _icons(icons) {}

                /** @brief Returns the folder. */
                FolderModel &folder() { return _folder; }

                /** @brief Returns the folder. */
                const FolderModel &folder() const { return _folder; }

                /**
                 * @brief Identifies by their contents up to @p count files whose names
                 *        say nothing (`Vfs::identify()`: 512 bytes each).
                 * @return True if none is left.
                 */
                bool identifySome(size_t count);

                /** @brief Returns the sort of column @p column. */
                static FolderModel::Sort sortOf(size_t column);

                /** @brief Returns the column of sort @p sort. */
                static size_t columnOf(FolderModel::Sort sort);

                size_t      count() const override { return _folder.count(); }
                StringView  text(size_t row) const override { return _folder.name(row); }
                const Icon *icon(size_t row, uint16_t size) const override;
                size_t      columns() const override { return Columns; }
                StringView  columnTitle(size_t column) const override;
                StringView  columnText(size_t row, size_t column, Span<char> buffer) const override;
                Align       columnAlign(size_t column) const override;
                uint16_t    columnWidth(size_t column) const override;

        private:
                FolderModel _folder;
                FileIcons  &_icons;
};

BMDS_NAMESPACE_END
