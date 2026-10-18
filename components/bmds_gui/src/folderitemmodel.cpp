/**
 * @file      folderitemmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A folder's items for the views (devplan/filebrowser.md §4h).
 */

#include <cstdio>
#include <bmds/apps.h>
#include <bmds/clock.h>
#include <bmds/fileformat.h>
#include <bmds/folderitemmodel.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Field = FolderItemModel::Field;

        constexpr Field      Plain[] = {Field::Name, Field::Size, Field::Type, Field::Modified};
        constexpr Field      Trashed[] = {Field::Name, Field::Origin, Field::Deleted, Field::Size, Field::Type};
        constexpr StringView Titles[] = {"Name", "Size", "Type", "Modified", "Original Location", "Date Deleted"};
        constexpr uint16_t   Widths[] = {200, 80, 144, 140, 200, 140};
        constexpr StringView Executable("application/x-executable");

        constexpr FolderModel::Sort Sorts[] = {FolderModel::Sort::Name,   FolderModel::Sort::Size,
                                               FolderModel::Sort::Type,   FolderModel::Sort::Modified,
                                               FolderModel::Sort::Origin, FolderModel::Sort::Deleted};

} // namespace

size_t FolderItemModel::columns() const {
        return _folder.isTrash() ? sizeof(Trashed) / sizeof(Trashed[0]) : sizeof(Plain) / sizeof(Plain[0]);
}

FolderItemModel::Field FolderItemModel::fieldOf(size_t column) const {
        if (_folder.isTrash()) return column < sizeof(Trashed) / sizeof(Trashed[0]) ? Trashed[column] : Field::Name;
        return column < sizeof(Plain) / sizeof(Plain[0]) ? Plain[column] : Field::Name;
}

FolderModel::Sort FolderItemModel::sortOf(size_t column) const {
        return Sorts[size_t(fieldOf(column))];
}

size_t FolderItemModel::columnOf(FolderModel::Sort sort) const {
        for (size_t c = 0; c < columns(); ++c)
                if (Sorts[size_t(fieldOf(c))] == sort) return c;
        return 0;
}

bool FolderItemModel::identifySome(size_t count) {
        size_t done = 0;
        for (size_t row = 0; row < _folder.count(); ++row) {
                const FolderModel::Entry &e = _folder.entry(row);
                if (e.type != FileType::File || !e.format.isEmpty()) continue;
                if (done == count) return false;
                ++done;
                String     path(MemSpace::Psram);
                StringView format = FileFormat::Binary; // not read: as unknown bytes
                if (path.assign(_folder.path()).isOk() && (_folder.path().endsWith('/') || path.append('/').isOk()) &&
                    path.append(_folder.name(row)).isOk()) {
                        auto [type, err] = Vfs::identify(path.view(), Vfs::IdentifyContent, Vfs::Context{"/", nullptr});
                        if (err.isOk() && !type.isEmpty()) format = type;
                }
                _folder.setFormat(row, format);
        }
        return true;
}

// Drawn on the window manager's task: an icon not loaded yet is asked for, and loaded on the loop.
const Icon *FolderItemModel::icon(size_t row, uint16_t size) const {
        const FolderModel::Entry &e = _folder.entry(row);
        const uint16_t            side = size > 16 ? 32 : 16;
        const Icon               *icon = nullptr;
        if (e.isDirectory() && Apps::isFolderName(_folder.name(row))) {
                // An application (apps.md §7): its own icon (its Icon=, else its program's).
                char       path[FileIcons::MaxAppPath + 1];
                const auto dir = _folder.path();
                const int  n = std::snprintf(path, sizeof(path), "%.*s%s%.*s", int(dir.size()), dir.data(),
                                            dir.endsWith('/') ? "" : "/", int(_folder.name(row).size()),
                                             _folder.name(row).data());
                if (n > 0 && size_t(n) < sizeof(path)) icon = _icons.loadedForApp(StringView(path, size_t(n)), side);
                if (icon == nullptr) icon = _icons.loadedNamed("application-x-executable", side);
        } else if (e.trash) {
                // The trash, or a link to it: full or empty (devplan/filebrowser.md §4k, FB26).
                icon = _icons.loadedNamed(_icons.isTrashFull() ? "user-trash-full" : "user-trash", side);
                if (icon == nullptr) icon = _icons.loadedNamed("folder", side);
        } else if (e.isDirectory()) {
                icon = _icons.loadedNamed("folder", side);
        } else if (e.builtin) {
                icon = _icons.loadedNamed("application-x-executable", side);
        } else {
                icon = _icons.loadedForType(e.format.isEmpty() ? FileFormat::Binary : e.format, side);
        }
        // Cut: dimmed until pasted or the clipboard changes (devplan/filebrowser.md §4j).
        if (icon != nullptr && _copied != nullptr && _copied->isCut(_folder.path(), _folder.name(row)))
                return _icons.dimmed(icon);
        return icon;
}

StringView FolderItemModel::columnTitle(size_t column) const {
        return column < columns() ? Titles[size_t(fieldOf(column))] : StringView();
}

ItemModel::Align FolderItemModel::columnAlign(size_t column) const {
        return fieldOf(column) == Field::Size ? Align::Right : Align::Left;
}

uint16_t FolderItemModel::columnWidth(size_t column) const {
        return column < columns() ? Widths[size_t(fieldOf(column))] : 100;
}

StringView FolderItemModel::columnText(size_t row, size_t column, Span<char> buffer) const {
        const FolderModel::Entry &e = _folder.entry(row);
        int                       n = 0;
        switch (fieldOf(column)) {
                case Field::Name: return _folder.name(row);
                case Field::Size:
                        if (e.isDirectory() || e.builtin || e.type != FileType::File) return StringView();
                        n = std::snprintf(buffer.data(), buffer.size(), "%llu KB",
                                          static_cast<unsigned long long>((e.size + 1023) / 1024));
                        break;
                case Field::Type: {
                        // As the Type sort orders them: by the extension.
                        if (e.isDirectory()) return Apps::isFolderName(_folder.name(row)) ? "Application" : "Folder";
                        if (e.builtin || e.format == Executable) return e.builtin ? "Built-in program" : "Program";
                        const StringView ext = FolderModel::extensionOf(_folder.name(row));
                        if (ext.isEmpty()) return "File";
                        size_t k = 0;
                        for (size_t i = 0; i < ext.size() && k + 6 < buffer.size(); ++i)
                                buffer[k++] = ext[i] >= 'a' && ext[i] <= 'z' ? char(ext[i] - 'a' + 'A') : ext[i];
                        for (const char c : StringView(" File"))
                                if (k + 1 < buffer.size()) buffer[k++] = c;
                        return StringView(buffer.data(), k);
                }
                case Field::Modified: {
                        if (e.modified.ns == 0) return StringView();
                        const DateTime t = Clock::localTime(e.modified).local;
                        n = std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02u %02u:%02u", int(t.year),
                                          unsigned(t.month), unsigned(t.day), unsigned(t.hour), unsigned(t.minute));
                        break;
                }
                case Field::Origin: return _folder.origin(row);
                case Field::Deleted: {
                        // YYYY-MM-DDThh:mm:ss as the Modified column shows a time.
                        const StringView d = _folder.deleted(row);
                        if (d.size() < 16) return d;
                        n = std::snprintf(buffer.data(), buffer.size(), "%.10s %.5s", d.data(), d.data() + 11);
                        break;
                }
        }
        return StringView(buffer.data(), n > 0 && size_t(n) < buffer.size() ? size_t(n) : 0);
}

BMDS_NAMESPACE_END
