/**
 * @file      itemmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A plain item model (devplan/filebrowser.md §4g).
 */

#include <bmds/itemmodel.h>

BMDS_NAMESPACE_BEGIN

Error ListItemModel::store(StringView s, Text &out) {
        if (s.size() > UINT16_MAX) return Error::Invalid;
        const Error err = _texts.reserve(_texts.size() + s.size());
        if (err.isError()) return err;
        out.at = static_cast<uint32_t>(_texts.size());
        out.length = static_cast<uint16_t>(s.size());
        for (const char c : s) (void)_texts.append(c); // reserved
        return Error::Ok;
}

void ListItemModel::clear() {
        _rows.clear();
        _texts.clear();
        return;
}

Error ListItemModel::add(StringView name, const Icon *small, const Icon *large, Span<const StringView> columnTexts) {
        Row   r;
        Error err = _rows.reserve(_rows.size() + 1);
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
