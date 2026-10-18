/**
 * @file      foldersettings.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * folders.conf: how each folder was last shown (devplan/filebrowser.md §4h, FB16).
 */

#include <utility>
#include <bmds/foldersettings.h>
#include <bmds/inireader.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView ViewNames[] = {"icons", "details"};
        constexpr StringView SortNames[] = {"name", "size", "type", "modified", "origin", "deleted"};

        // A width: digits only, 1 to 4000.
        bool parseWidth(StringView text, uint16_t &out) {
                uint32_t v = 0;
                if (text.isEmpty() || text.size() > 4) return false;
                for (const char c : text) {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + uint32_t(c - '0');
                }
                if (v == 0 || v > 4000) return false;
                out = uint16_t(v);
                return true;
        }

} // namespace

StringView FolderSettings::viewName(View view) {
        return ViewNames[size_t(view) < 2 ? size_t(view) : 0];
}

StringView FolderSettings::sortName(FolderModel::Sort sort) {
        return SortNames[size_t(sort) < sizeof(SortNames) / sizeof(SortNames[0]) ? size_t(sort) : 0];
}

bool FolderSettings::parseView(StringView text, View &out) {
        for (size_t i = 0; i < 2; ++i) {
                if (!text.equalsIgnoreCase(ViewNames[i])) continue;
                out = View(i);
                return true;
        }
        return false;
}

bool FolderSettings::parseSort(StringView text, FolderModel::Sort &out) {
        for (size_t i = 0; i < sizeof(SortNames) / sizeof(SortNames[0]); ++i) {
                if (!text.equalsIgnoreCase(SortNames[i])) continue;
                out = FolderModel::Sort(i);
                return true;
        }
        return false;
}

int FolderSettings::indexOf(StringView path) const {
        for (size_t i = 0; i < _folders.size(); ++i)
                if (_folders[i].path.view() == path) return int(i);
        return -1;
}

Error FolderSettings::parse(StringView text) {
        _folders.clear();
        IniReader reader(text);
        int       at = -1; // the group being read, or -1 (none, or dropped)
        for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                if (item.kind == IniReader::Kind::Group) {
                        at = -1;
                        if (!item.name.startsWith('/')) continue;
                        Values v;
                        if (set(item.name, v).isError()) return Error::NoMem;
                        at = indexOf(item.name);
                        continue;
                }
                if (item.kind != IniReader::Kind::Entry || at < 0) continue;
                Values &v = _folders[size_t(at)].values;
                if (item.key.equalsIgnoreCase("View")) {
                        (void)parseView(item.value, v.view);
                } else if (item.key.equalsIgnoreCase("Sort")) {
                        (void)parseSort(item.value, v.sort);
                } else if (item.key.equalsIgnoreCase("Descending")) {
                        bool b = false;
                        if (IniReader::toBool(item.value, b).isOk()) v.descending = b;
                } else if (item.key.equalsIgnoreCase("Tree")) {
                        (void)parseWidth(item.value, v.tree);
                }
        }
        return Error::Ok;
}

bool FolderSettings::find(StringView path, Values &out) const {
        const int i = indexOf(path);
        if (i < 0) return false;
        out = _folders[size_t(i)].values;
        return true;
}

Error FolderSettings::set(StringView path, const Values &values) {
        if (!path.startsWith('/')) return Error::Invalid;
        const int i = indexOf(path);
        if (i >= 0) {
                // Made the newest: moved to the end.
                Folder f = std::move(_folders[size_t(i)]);
                _folders.removeAt(size_t(i));
                f.values = values;
                (void)_folders.append(std::move(f)); // the room it left
                return Error::Ok;
        }
        Folder f;
        if (f.path.assign(path).isError() || _folders.reserve(_folders.size() + 1).isError()) return Error::NoMem;
        f.values = values;
        (void)_folders.append(std::move(f));
        if (_folders.size() > MaxFolders) _folders.removeAt(0);
        return Error::Ok;
}

Error FolderSettings::write(String &out) const {
        out.clear();
        Error err = out.append("# How each folder was last shown: written by the panel's folder windows.\n");
        for (const Folder &f : _folders) {
                const Values    &v = f.values;
                const StringView view = viewName(v.view), sort = sortName(v.sort);
                if (err.isOk())
                        err = out.appendf("\n[%.*s]\nView=%.*s\nSort=%.*s\nDescending=%s\n", int(f.path.size()),
                                          f.path.cStr(), int(view.size()), view.data(), int(sort.size()), sort.data(),
                                          v.descending ? "true" : "false");
                if (err.isOk() && v.tree != 0) err = out.appendf("Tree=%u\n", unsigned(v.tree));
        }
        return err.isError() ? Error(Error::NoMem) : Error(Error::Ok);
}

BMDS_NAMESPACE_END
