/**
 * @file      foldermodel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A folder's entries, read a batch at a time and kept sorted
 *        (`devplan/filebrowser.md` §4f).
 *
 * `open()` starts on a folder; `fillSome()` reads the next entries (a view
 * draws as the folder fills, never waiting for all of it). The entries shown
 * are in order at every step: folders first, then by the sort's key (names
 * without case, runs of digits as numbers). Hidden entries (a leading `.` or
 * FAT's Hidden attribute) are kept but shown only with `setShowHidden()`.
 *
 * A symbolic link is shown as what it leads to (a link to a folder is a
 * folder), marked as a link.
 *
 * Index `i` (0 to `count()`) is the `i`-th entry shown.
 *
 * @par Thread Safety
 * Not internally synchronized: one owner (a window's loop).
 */
class FolderModel {
        public:
                /** @brief What entries are ordered by (folders always first). */
                enum class Sort : uint8_t {
                        Name,
                        Size,
                        Type, ///< The extension, then the name.
                        Modified,
                };

                /** @brief An entry. */
                struct Entry {
                                uint64_t  size = 0;
                                Timestamp modified;
                                uint32_t  nameAt = 0; ///< Its name's offset in the model's names.
                                uint16_t  nameLength = 0;
                                FileType  type = FileType::File; ///< A link's: what it leads to (a broken one: `Link`).
                                uint8_t   attributes = 0;        ///< `FileInfo::Attribute` bits.
                                bool      link = false;          ///< A symbolic link (`/home`).

                                /** @brief Returns true for folders. */
                                bool isDirectory() const { return type == FileType::Directory; }
                };

                FolderModel() = default;
                FolderModel(const FolderModel &) = delete;
                FolderModel &operator=(const FolderModel &) = delete;

                /**
                 * @brief Starts on folder @p path (relative to @p context's cwd),
                 *        emptied of the last one's entries.
                 * @return `Ok`, `NotDir`, `NotExist`, `NoMem`, or the VFS's error.
                 */
                Error open(StringView path, const Vfs::Context &context);

                /**
                 * @brief Reads up to @p count more entries.
                 * @return The entries read (0 once complete), or the error
                 *         reading (the folder is then complete as it is).
                 */
                Result<size_t> fillSome(size_t count);

                /** @brief Returns true once every entry is read (or reading failed). */
                bool isComplete() const { return !_directory.isOpen(); }

                /** @brief Stops reading (the entries read are kept). */
                void close() { _directory.close(); }

                /** @brief Returns the folder's path (normalized). */
                StringView path() const { return _path.view(); }

                /** @brief Returns the entries shown. */
                size_t count() const { return _order.size(); }

                /** @brief Returns entry @p index (unchecked). */
                const Entry &entry(size_t index) const { return _entries[_order[index]]; }

                /** @brief Returns entry @p index's name. */
                StringView name(size_t index) const { return nameOf(_entries[_order[index]]); }

                /** @brief Returns the index of the entry named @p name (as the filesystem has it), or -1. */
                int indexOf(StringView name) const;

                /** @brief Sorts by @p sort, @p descending or not. */
                void setSort(Sort sort, bool descending);

                /** @brief Returns the sort. */
                Sort sort() const { return _sort; }

                /** @brief Returns true if sorted down. */
                bool isDescending() const { return _descending; }

                /** @brief Shows or leaves out hidden entries. */
                void setShowHidden(bool show);

                /** @brief Returns true if hidden entries are shown. */
                bool showsHidden() const { return _showHidden; }

                /** @brief Returns the bytes of the files shown. */
                uint64_t shownBytes() const;

                /**
                 * @brief Compares names as the model orders them: without case, runs of
                 *        digits as numbers. @return Negative, 0, or positive.
                 */
                static int compareNames(StringView a, StringView b);

                /** @brief Returns @p name's type for sorting: its extension, or empty. */
                static StringView extensionOf(StringView name);

        private:
                StringView nameOf(const Entry &e) const { return StringView(_names.data() + e.nameAt, e.nameLength); }
                bool       isHidden(const Entry &e) const;
                bool       before(uint32_t a, uint32_t b) const; // entry a shown before entry b
                Error      merge(size_t from);                   // entries [from, end) into the order
                void       reorder();                            // the order again from all entries

                Directory      _directory;
                Vfs::Context   _context; // open()'s (links are followed with it)
                String         _path{MemSpace::Psram};
                List<Entry>    _entries{MemSpace::Psram};
                List<char>     _names{MemSpace::Psram};
                List<uint32_t> _order{MemSpace::Psram}; // the entries shown, in order
                Sort           _sort = Sort::Name;
                bool           _descending = false;
                bool           _showHidden = false;
};

BMDS_NAMESPACE_END
