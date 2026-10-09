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
#include <bmds/clock.h>
#include <bmds/fileformat.h>
#include <bmds/folderitemmodel.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Titles[] = {"Name", "Size", "Type", "Modified"};
        constexpr uint16_t   Widths[] = {200, 80, 144, 140};
        constexpr StringView Executable("application/x-executable");

} // namespace

FolderModel::Sort FolderItemModel::sortOf(size_t column) {
        switch (column) {
                case SizeColumn: return FolderModel::Sort::Size;
                case TypeColumn: return FolderModel::Sort::Type;
                case ModifiedColumn: return FolderModel::Sort::Modified;
                default: return FolderModel::Sort::Name;
        }
}

size_t FolderItemModel::columnOf(FolderModel::Sort sort) {
        switch (sort) {
                case FolderModel::Sort::Size: return SizeColumn;
                case FolderModel::Sort::Type: return TypeColumn;
                case FolderModel::Sort::Modified: return ModifiedColumn;
                default: return NameColumn;
        }
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
        if (e.isDirectory()) {
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
        return column < Columns ? Titles[column] : StringView();
}

ItemModel::Align FolderItemModel::columnAlign(size_t column) const {
        return column == SizeColumn ? Align::Right : Align::Left;
}

uint16_t FolderItemModel::columnWidth(size_t column) const {
        return column < Columns ? Widths[column] : 100;
}

StringView FolderItemModel::columnText(size_t row, size_t column, Span<char> buffer) const {
        const FolderModel::Entry &e = _folder.entry(row);
        int                       n = 0;
        switch (column) {
                case NameColumn: return _folder.name(row);
                case SizeColumn:
                        if (e.isDirectory() || e.builtin || e.type != FileType::File) return StringView();
                        n = std::snprintf(buffer.data(), buffer.size(), "%llu KB",
                                          static_cast<unsigned long long>((e.size + 1023) / 1024));
                        break;
                case TypeColumn: {
                        // As the Type sort orders them: by the extension.
                        if (e.isDirectory()) return "Folder";
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
                case ModifiedColumn: {
                        if (e.modified.ns == 0) return StringView();
                        const DateTime t = Clock::localTime(e.modified).local;
                        n = std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02u %02u:%02u", int(t.year),
                                          unsigned(t.month), unsigned(t.day), unsigned(t.hour), unsigned(t.minute));
                        break;
                }
                default: return StringView();
        }
        return StringView(buffer.data(), n > 0 && size_t(n) < buffer.size() ? size_t(n) : 0);
}

BMDS_NAMESPACE_END
