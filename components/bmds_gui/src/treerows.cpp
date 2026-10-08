/**
 * @file      treerows.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A tree's visible rows as one flat list (devplan/filebrowser.md §4g).
 */

#include <bmds/treerows.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t CompactAbove = 4096; // garbage bytes before compacting is worth it

} // namespace

Error TreeRows::store(StringView name, Row &row) {
        if (name.size() > UINT16_MAX) return Error::Invalid;
        const Error err = _names.reserve(_names.size() + name.size());
        if (err.isError()) return err;
        row.nameAt = static_cast<uint32_t>(_names.size());
        row.nameLength = static_cast<uint16_t>(name.size());
        for (const char c : name) (void)_names.append(c); // reserved
        _live += name.size();
        return Error::Ok;
}

Error TreeRows::addRoot(StringView name, uintptr_t data) {
        Row   r;
        Error err = _rows.reserve(_rows.size() + 1);
        if (err.isOk()) err = store(name, r);
        if (err.isError()) return err;
        r.data = data;
        (void)_rows.append(r); // reserved
        return Error::Ok;
}

size_t TreeRows::endOf(size_t row) const {
        if (row >= _rows.size()) return _rows.size();
        const uint8_t depth = _rows[row].depth;
        size_t        end = row + 1;
        while (end < _rows.size() && _rows[end].depth > depth) ++end;
        return end;
}

void TreeRows::collapse(size_t row) {
        if (row >= _rows.size()) return;
        const size_t end = endOf(row);
        for (size_t i = row + 1; i < end; ++i) _live -= _rows[i].nameLength;
        // The run after it moved down over the descendants.
        const size_t gone = end - (row + 1);
        for (size_t i = end; i < _rows.size(); ++i) _rows[i - gone] = _rows[i];
        _rows.truncate(_rows.size() - gone);
        _rows[row].flags &= static_cast<uint8_t>(~Expanded);
        compact();
        return;
}

Error TreeRows::setChildren(size_t row, Span<const StringView> names) {
        if (row >= _rows.size()) return Error::OutOfRange;
        if (_rows[row].depth == UINT8_MAX) return Error::OutOfRange;
        const uint8_t depth = static_cast<uint8_t>(_rows[row].depth + 1);
        const size_t  end = endOf(row);
        // The children shown now, and for each name the one it was (-1: new).
        List<size_t>  old(MemSpace::Psram);
        List<uint8_t> kept(MemSpace::Psram);
        List<int>     from(MemSpace::Psram);
        Error         err = from.reserve(names.size());
        for (size_t i = row + 1; err.isOk() && i < end; ++i)
                if (_rows[i].depth == depth) err = old.append(i);
        if (err.isOk()) err = kept.reserve(old.size());
        if (err.isError()) return err;
        for (size_t i = 0; i < old.size(); ++i) (void)kept.append(0); // reserved
        // Most often in the same order: looked for from after the last one found.
        size_t cursor = 0;
        size_t rows = (row + 1) + (_rows.size() - end);
        size_t bytes = 0;
        for (const StringView n : names) {
                int found = -1;
                for (size_t k = 0; k < old.size() && found < 0; ++k) {
                        const size_t at = (cursor + k) % old.size();
                        if (kept[at] == 0 && name(old[at]) == n) found = int(at);
                }
                (void)from.append(found); // reserved
                if (found >= 0) {
                        kept[size_t(found)] = 1;
                        cursor = size_t(found) + 1;
                        rows += endOf(old[size_t(found)]) - old[size_t(found)];
                } else {
                        rows += 1;
                        bytes += n.size();
                }
        }
        // Room first for the rows and names: then nothing fails half way.
        List<Row> fresh(MemSpace::Psram);
        err = fresh.reserve(rows);
        if (err.isOk()) err = _names.reserve(_names.size() + bytes);
        if (err.isError()) return err;
        // The rows of children gone, and their descendants, no longer name anything.
        for (size_t k = 0; k < old.size(); ++k) {
                if (kept[k] != 0) continue;
                const size_t stop = endOf(old[k]);
                for (size_t i = old[k]; i < stop; ++i) _live -= _rows[i].nameLength;
        }
        for (size_t i = 0; i <= row; ++i) (void)fresh.append(_rows[i]);
        for (size_t i = 0; i < names.size(); ++i) {
                if (from[i] >= 0) {
                        // Still there: with what is shown below it.
                        const size_t at = old[size_t(from[i])];
                        const size_t stop = endOf(at);
                        for (size_t j = at; j < stop; ++j) (void)fresh.append(_rows[j]);
                        continue;
                }
                Row r;
                (void)store(names[i], r); // reserved
                r.depth = depth;
                (void)fresh.append(r);
        }
        for (size_t i = end; i < _rows.size(); ++i) (void)fresh.append(_rows[i]);
        _rows = std::move(fresh);
        const size_t k = names.size();
        _rows[row].flags = static_cast<uint8_t>(k > 0 ? (_rows[row].flags | Expanded | HasChildren)
                                                      : (_rows[row].flags & ~(Expanded | HasChildren)));
        compact();
        return Error::Ok;
}

void TreeRows::clear() {
        _rows.clear();
        _names.clear();
        _live = 0;
        return;
}

int TreeRows::parent(size_t row) const {
        if (row >= _rows.size() || _rows[row].depth == 0) return -1;
        const uint8_t depth = _rows[row].depth;
        for (size_t i = row; i-- > 0;)
                if (_rows[i].depth < depth) return int(i);
        return -1;
}

Error TreeRows::pathOf(size_t row, String &out) const {
        out.clear();
        if (row >= _rows.size()) return Error::OutOfRange;
        // The ancestors, root first: found walking up, written down.
        size_t chain[256];
        size_t n = 0;
        for (int r = int(row); r >= 0 && n < 256; r = parent(size_t(r))) chain[n++] = size_t(r);
        for (size_t i = n; i-- > 0;) {
                const StringView part = name(chain[i]);
                Error            err = Error::Ok;
                if (i + 1 < n && !out.view().endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(part);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

int TreeRows::find(StringView path) const {
        String p(MemSpace::Psram);
        for (size_t i = 0; i < _rows.size(); ++i) {
                // Only rows whose own name ends the path are worth building.
                const StringView n = name(i);
                if (!path.endsWith(n)) continue;
                if (pathOf(i, p).isOk() && p.view() == path) return int(i);
        }
        return -1;
}

void TreeRows::compact() {
        const size_t garbage = _names.size() - _live;
        if (garbage < CompactAbove || garbage < _live) return;
        List<char> fresh(MemSpace::Psram);
        if (fresh.reserve(_live).isError()) return; // kept as it is
        for (Row &r : _rows) {
                const uint32_t at = static_cast<uint32_t>(fresh.size());
                for (uint16_t i = 0; i < r.nameLength; ++i) (void)fresh.append(_names[r.nameAt + i]);
                r.nameAt = at;
        }
        _names = std::move(fresh);
        return;
}

BMDS_NAMESPACE_END
