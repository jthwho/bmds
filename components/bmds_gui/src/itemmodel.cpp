/**
 * @file      itemmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A plain item model (devplan/filebrowser.md §4g).
 */

#include <utility>
#include <bmds/itemmodel.h>

BMDS_NAMESPACE_BEGIN

namespace {
        // Room for @p needed, grown as an append grows a list: reserving only what one more row
        // needs copied every row and every text kept, each add (2,000 rows: 11.7 s, gui.md §4v).
        template <typename L> Error roomFor(L &list, size_t needed) {
                if (needed <= list.capacity()) return Error::Ok;
                return list.reserve(Detail::listGrownCapacity(list.capacity(), needed));
        }
} // namespace

Error ListItemModel::store(StringView s, Text &out) {
        if (s.size() > UINT16_MAX) return Error::Invalid;
        const Error err = roomFor(_texts, _texts.size() + s.size());
        if (err.isError()) return err;
        out.at = static_cast<uint32_t>(_texts.size());
        out.length = static_cast<uint16_t>(s.size());
        for (const char c : s) (void)_texts.append(c); // reserved
        return Error::Ok;
}

void ListItemModel::clear() {
        _rows.clear();
        _texts.clear();
        _dead = 0;
        return;
}

void ListItemModel::removeAt(size_t row) {
        if (row >= _rows.size()) return;
        for (const Text &t : _rows[row].text) _dead += t.length;
        _rows.removeAt(row);
        if (_rows.isEmpty()) {
                _texts.clear();
                _dead = 0;
        } else {
                compact();
        }
        return;
}

Error ListItemModel::setRow(size_t row, StringView name, Span<const StringView> columnTexts) {
        if (row >= _rows.size()) return Error::Invalid;
        StringView want[MaxColumns];
        want[0] = name;
        for (size_t c = 0; c < columnTexts.size() && c + 1 < MaxColumns; ++c) want[c + 1] = columnTexts[c];
        for (const StringView &w : want)
                if (w.size() > UINT16_MAX) return Error::Invalid;
        // Room first for every text that doesn't fit where its old one is: all or nothing.
        size_t grow = 0;
        for (size_t c = 0; c < MaxColumns; ++c)
                if (want[c].size() > _rows[row].text[c].length) grow += want[c].size();
        if (grow > 0 && roomFor(_texts, _texts.size() + grow).isError()) return Error::NoMem;
        Row &r = _rows[row];
        for (size_t c = 0; c < MaxColumns; ++c) {
                Text            &t = r.text[c];
                const StringView w = want[c];
                if (view(t) == w) continue;
                if (w.size() <= t.length) {
                        // In its old place (no row shares a text's bytes).
                        for (size_t i = 0; i < w.size(); ++i) _texts[t.at + i] = w[i];
                        _dead += t.length - w.size();
                        t.length = static_cast<uint16_t>(w.size());
                } else {
                        _dead += t.length;
                        (void)store(w, t); // reserved
                }
        }
        compact();
        return Error::Ok;
}

// The texts again without the bytes no row uses, once those outweigh the rest: a row changed
// often (a monitor's) can't grow the store for good.
void ListItemModel::compact() {
        if (_dead < 1024 || _dead * 2 < _texts.size()) return;
        List<char> kept(MemSpace::Psram);
        if (kept.reserve(_texts.size() - _dead).isError()) return; // later, then
        for (Row &r : _rows) {
                for (Text &t : r.text) {
                        const uint32_t at = static_cast<uint32_t>(kept.size());
                        for (uint16_t i = 0; i < t.length; ++i) (void)kept.append(_texts[t.at + i]); // reserved
                        t.at = at;
                }
        }
        _texts = std::move(kept);
        _dead = 0;
        return;
}

Error ListItemModel::add(StringView name, const Icon *small, const Icon *large, Span<const StringView> columnTexts) {
        Row   r;
        Error err = roomFor(_rows, _rows.size() + 1);
        if (err.isOk()) err = store(name, r.text[0]);
        for (size_t c = 0; err.isOk() && c < columnTexts.size() && c + 1 < MaxColumns; ++c)
                err = store(columnTexts[c], r.text[c + 1]);
        if (err.isError()) return err;
        r.small = small;
        r.large = large;
        (void)_rows.append(r); // reserved
        return Error::Ok;
}

void ListItemModel::setIcons(size_t row, const Icon *small, const Icon *large) {
        if (row >= _rows.size()) return;
        _rows[row].small = small;
        _rows[row].large = large;
        return;
}

const Icon *ListItemModel::icon(size_t row, uint16_t size) const {
        const Row  &r = _rows[row];
        const Icon *wanted = size > 16 ? r.large : r.small;
        return wanted != nullptr ? wanted : (size > 16 ? r.small : r.large);
}

void ListItemModel::setColumns(Span<const StringView> titles, Span<const uint16_t> widths, Span<const Align> aligns) {
        _titleTexts.clear();
        _columns = titles.size() < MaxColumns ? titles.size() : MaxColumns;
        for (size_t c = 0; c < _columns; ++c) {
                const StringView t = titles[c];
                _titles[c] = Text();
                if (_titleTexts.reserve(_titleTexts.size() + t.size()).isOk()) {
                        _titles[c].at = static_cast<uint32_t>(_titleTexts.size());
                        _titles[c].length = static_cast<uint16_t>(t.size());
                        for (const char ch : t) (void)_titleTexts.append(ch);
                }
                _widths[c] = c < widths.size() ? widths[c] : ItemModel::columnWidth(c);
                _aligns[c] = c < aligns.size() ? aligns[c] : Align::Left;
        }
        return;
}

StringView ListItemModel::columnTitle(size_t column) const {
        if (column >= _columns) return ItemModel::columnTitle(column);
        return StringView(_titleTexts.data() + _titles[column].at, _titles[column].length);
}

StringView ListItemModel::columnText(size_t row, size_t column, Span<char> buffer) const {
        (void)buffer;
        return column < MaxColumns ? view(_rows[row].text[column]) : StringView();
}

ItemModel::Align ListItemModel::columnAlign(size_t column) const {
        return column < _columns ? _aligns[column] : Align::Left;
}

uint16_t ListItemModel::columnWidth(size_t column) const {
        return column < _columns ? _widths[column] : ItemModel::columnWidth(column);
}

BMDS_NAMESPACE_END
