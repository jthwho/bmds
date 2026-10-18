/**
 * @file      foldersettings.h
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
#include <bmds/foldermodel.h>
#include <bmds/list.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief How each folder was last shown: `folders.conf` (`devplan/filebrowser.md`
 *        §4h, FB16), pure.
 *
 * An INI file of a group per folder, its absolute path:
 * @code
 * [/sd0/p0/photos]
 * View=details
 * Sort=size
 * Descending=true
 * Tree=200
 * @endcode
 * A key missing or not understood keeps its default. Groups that aren't
 * absolute paths are dropped. The folder set last is written last; past
 * `MaxFolders`, the ones set longest ago go. Its keys are an interface
 * (frozen once shipped).
 *
 * @par Thread Safety
 * A value with one owner.
 */
class FolderSettings {
        public:
                /** @brief Folders kept at most. */
                static constexpr size_t MaxFolders = 256;

                /** @brief The view a folder is shown in. */
                enum class View : uint8_t {
                        Icons,
                        Details,
                };

                /** @brief A folder's settings. */
                struct Values {
                                View              view = View::Icons;
                                FolderModel::Sort sort = FolderModel::Sort::Name;
                                bool              descending = false;
                                uint16_t          tree = 0; ///< The folder tree's width (0: not set).
                };

                /** @brief Reads @p text (the folders before it forgotten). @return `Ok` or `NoMem`. */
                Error parse(StringView text);

                /** @brief Puts folder @p path's settings in @p out. @return False if it has none. */
                bool find(StringView path, Values &out) const;

                /**
                 * @brief Sets folder @p path's settings (an absolute path), making it the
                 *        newest; the oldest goes past `MaxFolders`.
                 * @return `Ok`, `Invalid` (not an absolute path) or `NoMem`.
                 */
                Error set(StringView path, const Values &values);

                /** @brief Writes the file's text into @p out (replaced). @return `Ok` or `NoMem`. */
                Error write(String &out) const;

                /** @brief Returns the folders kept. */
                size_t count() const { return _folders.size(); }

                /** @brief Returns @p view's name in the file (`icons`, `details`). */
                static StringView viewName(View view);

                /** @brief Returns @p sort's name in the file (`name`, `size`, `type`, `modified`, `origin`, `deleted`). */
                static StringView sortName(FolderModel::Sort sort);

                /** @brief Reads a view's name (any case). @return False if it isn't one. */
                static bool parseView(StringView text, View &out);

                /** @brief Reads a sort's name (any case). @return False if it isn't one. */
                static bool parseSort(StringView text, FolderModel::Sort &out);

        private:
                struct Folder {
                                String path{MemSpace::Psram};
                                Values values;
                };

                int indexOf(StringView path) const;

                List<Folder> _folders{MemSpace::Psram}; // the oldest first
};

BMDS_NAMESPACE_END
