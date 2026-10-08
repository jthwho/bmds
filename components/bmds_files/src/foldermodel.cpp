/**
 * @file      foldermodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A folder's entries, read a batch at a time and kept sorted (devplan/filebrowser.md §4f).
 */

#include <algorithm>
#include <bmds/foldermodel.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool isDigit(char c) {
                return c >= '0' && c <= '9';
        }

} // namespace

Error FolderModel::open(StringView path, const Vfs::Context &context) {
        _directory.close();
        _entries.clear();
        _names.clear();
        _order.clear();
        auto [normal, nerr] = Vfs::normalize(path, context.cwd);
        if (nerr.isError()) return nerr;
        Error err = _path.assign(normal.view());
        if (err.isError()) return err;
        auto [info, serr] = Vfs::stat(_path.view(), context);
        if (serr.isError()) return serr;
        if (!info.isDirectory()) return Error::NotDir;
        auto [dir, derr] = Vfs::openDirectory(_path.view(), context);
        if (derr.isError()) return derr;
        _directory = std::move(dir);
        _context = Vfs::Context{"/", nullptr}; // the path is absolute now
        return Error::Ok;
}

Result<size_t> FolderModel::fillSome(size_t count) {
        const size_t from = _entries.size();
        Error        failed;
        for (size_t i = 0; i < count && _directory.isOpen(); ++i) {
                auto [info, err] = _directory.next();
                if (err.isError()) {
                        _directory.close();
                        if (err != Error::EndOfFile) failed = err;
                        break;
                }
                const StringView name = info.name.view();
                if (name.size() > UINT16_MAX || _names.size() + name.size() > UINT32_MAX) continue;
                Entry e;
                e.size = info.size;
                e.modified = info.modified;
                e.nameAt = static_cast<uint32_t>(_names.size());
                e.nameLength = static_cast<uint16_t>(name.size());
                e.type = info.type;
                e.attributes = info.attributes;
                if (info.type == FileType::Link) {
                        // What it leads to (a broken link stays a link).
                        e.link = true;
                        String target(MemSpace::Psram);
                        if (target.assign(_path.view()).isOk() &&
                            (_path.view().endsWith('/') || target.append('/').isOk()) && target.append(name).isOk()) {
                                auto [to, terr] = Vfs::stat(target.view(), _context);
                                if (terr.isOk()) {
                                        e.type = to.type;
                                        e.size = to.size;
                                        e.modified = to.modified;
                                }
                        }
                }
                Error stored = _names.reserve(_names.size() + name.size());
                if (stored.isOk()) stored = _entries.append(e);
                if (stored.isError()) {
                        failed = stored;
                        _directory.close();
                        break;
                }
                for (const char c : name) (void)_names.append(c); // reserved
        }
        const size_t read = _entries.size() - from;
        const Error  merged = merge(from);
        if (failed.isOk()) failed = merged;
        return Result<size_t>(read, failed);
}

bool FolderModel::isHidden(const Entry &e) const {
        return (e.nameLength > 0 && _names[e.nameAt] == '.') || (e.attributes & FileInfo::Hidden) != 0;
}

StringView FolderModel::extensionOf(StringView name) {
        const size_t dot = name.lastIndexOf('.');
        return dot == StringView::NPos || dot == 0 ? StringView() : name.mid(dot + 1);
}

int FolderModel::compareNames(StringView a, StringView b) {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
                if (isDigit(a[i]) && isDigit(b[j])) {
                        // Runs of digits by value: leading zeros skipped, then the longer is larger.
                        size_t si = i, sj = j;
                        while (si < a.size() && a[si] == '0') ++si;
                        while (sj < b.size() && b[sj] == '0') ++sj;
                        size_t ei = si, ej = sj;
                        while (ei < a.size() && isDigit(a[ei])) ++ei;
                        while (ej < b.size() && isDigit(b[ej])) ++ej;
                        if (ei - si != ej - sj) return ei - si < ej - sj ? -1 : 1;
                        for (size_t k = 0; k < ei - si; ++k)
                                if (a[si + k] != b[sj + k]) return a[si + k] < b[sj + k] ? -1 : 1;
                        i = ei;
                        j = ej;
                        continue;
                }
                const char ca = StringView::toLower(a[i]), cb = StringView::toLower(b[j]);
                if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
                ++i;
                ++j;
        }
        if (i < a.size()) return 1;
        if (j < b.size()) return -1;
        // Equal without case: in a fixed order still (FAT can't hold both, others can).
        const int exact = a.toStdStringView().compare(b.toStdStringView());
        return exact < 0 ? -1 : exact > 0 ? 1 : 0;
}

bool FolderModel::before(uint32_t a, uint32_t b) const {
        const Entry &x = _entries[a];
        const Entry &y = _entries[b];
        if (x.isDirectory() != y.isDirectory()) return x.isDirectory();
        const StringView nx = nameOf(x), ny = nameOf(y);
        int              key = 0;
        switch (_sort) {
                case Sort::Name: break;
                case Sort::Size:
                        if (!x.isDirectory() && x.size != y.size) key = x.size < y.size ? -1 : 1;
                        break;
                case Sort::Type: key = compareNames(extensionOf(nx), extensionOf(ny)); break;
                case Sort::Modified:
                        if (x.modified != y.modified) key = x.modified < y.modified ? -1 : 1;
                        break;
        }
        if (key == 0 && _sort == Sort::Name) key = compareNames(nx, ny);
        if (key != 0) return _descending ? key > 0 : key < 0;
        // Ties by name, always up; the same name twice in read order.
        key = compareNames(nx, ny);
        return key != 0 ? key < 0 : a < b;
}

// The entries from @p from on that show, sorted on their own, then merged into the order from its
// end (the batch is the only copy).
Error FolderModel::merge(size_t from) {
        List<uint32_t> batch(MemSpace::Psram);
        Error          err = batch.reserve(_entries.size() - from);
        if (err.isError()) return err;
        for (size_t i = from; i < _entries.size(); ++i)
                if (_showHidden || !isHidden(_entries[i])) (void)batch.append(static_cast<uint32_t>(i));
        if (batch.isEmpty()) return Error::Ok;
        std::sort(batch.begin(), batch.end(), [this](uint32_t a, uint32_t b) { return before(a, b); });
        const size_t old = _order.size();
        err = _order.reserve(old + batch.size());
        if (err.isError()) return err;
        for (size_t i = 0; i < batch.size(); ++i) (void)_order.append(0); // room, filled below
        size_t to = _order.size(), i = old, j = batch.size();
        while (j > 0) {
                if (i > 0 && before(batch[j - 1], _order[i - 1])) {
                        _order[--to] = _order[--i];
                } else {
                        _order[--to] = batch[--j];
                }
        }
        return Error::Ok;
}

void FolderModel::reorder() {
        _order.clear();
        if (_order.reserve(_entries.size()).isError()) return; // shown as far as memory allows
        for (size_t i = 0; i < _entries.size(); ++i)
                if (_showHidden || !isHidden(_entries[i])) (void)_order.append(static_cast<uint32_t>(i));
        std::sort(_order.begin(), _order.end(), [this](uint32_t a, uint32_t b) { return before(a, b); });
        return;
}

void FolderModel::setSort(Sort sort, bool descending) {
        if (sort == _sort && descending == _descending) return;
        _sort = sort;
        _descending = descending;
        std::sort(_order.begin(), _order.end(), [this](uint32_t a, uint32_t b) { return before(a, b); });
        return;
}

void FolderModel::setShowHidden(bool show) {
        if (show == _showHidden) return;
        _showHidden = show;
        reorder();
        return;
}

int FolderModel::indexOf(StringView name) const {
        for (size_t i = 0; i < _order.size(); ++i)
                if (nameOf(_entries[_order[i]]) == name) return int(i);
        return -1;
}

uint64_t FolderModel::shownBytes() const {
        uint64_t total = 0;
        for (const uint32_t i : _order)
                if (!_entries[i].isDirectory()) total += _entries[i].size;
        return total;
}

BMDS_NAMESPACE_END
