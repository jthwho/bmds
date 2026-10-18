/**
 * @file      folderitemmodel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/copiedfiles.h>
#include <bmds/fileicons.h>
#include <bmds/foldermodel.h>
#include <bmds/itemmodel.h>


BMDS_NAMESPACE_BEGIN

/**
 * @brief A folder's items for the views (`devplan/filebrowser.md` §4h): a
 *        `FolderModel` as an `ItemModel`, with each type's icon and the
 *        columns Name, Size, Type and Modified (in `/trash`: Name, Original
 *        Location, Date Deleted, Size and Type, §4k); a cut item's icon dimmed (§4j)
 *        when given the clipboard's files. The file browser's and
 *        `FileDialog`'s (`devplan/mediaplayer.md` §6d).
 *
 * @par Thread Safety
 * The application's loop (drawing asks for icons as `FileIcons` says).
 */
class FolderItemModel final : public ItemModel {
        public:
                /** @brief What a column shows. */
                enum class Field : uint8_t {
                        Name,
                        Size,
                        Type,
                        Modified,
                        Origin,  ///< `/trash`: the folder it was in.
                        Deleted, ///< `/trash`: when it was deleted.
                };

                /** @brief Items with @p icons. */
                explicit FolderItemModel(FileIcons &icons) : _icons(icons) {}

                /** @brief Dims the icons of @p copied's cut items (null: none), kept by the caller. */
                void setCopied(const CopiedFiles *copied) { _copied = copied; }

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

                /** @brief Returns what column @p column shows (the folder's columns). */
                Field fieldOf(size_t column) const;

                /** @brief Returns the sort of column @p column. */
                FolderModel::Sort sortOf(size_t column) const;

                /** @brief Returns the column of sort @p sort (Name's if none shows it). */
                size_t columnOf(FolderModel::Sort sort) const;

                size_t      count() const override { return _folder.count(); }
                StringView  text(size_t row) const override { return _folder.name(row); }
                const Icon *icon(size_t row, uint16_t size) const override;
                size_t      columns() const override;
                StringView  columnTitle(size_t column) const override;
                StringView  columnText(size_t row, size_t column, Span<char> buffer) const override;
                Align       columnAlign(size_t column) const override;
                uint16_t    columnWidth(size_t column) const override;

        private:
                FolderModel        _folder;
                FileIcons         &_icons;
                const CopiedFiles *_copied = nullptr;
};

BMDS_NAMESPACE_END
