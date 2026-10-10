/**
 * @file      tablemodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The System Monitor's tables (devplan/sysmon.md §3.5).
 */

#include "tablemodel.h"

BMDS_NAMESPACE_BEGIN

void TableModel::setColumns(Span<const Column> columns) {
        _columnCount = columns.size() < MaxColumns ? columns.size() : MaxColumns;
        for (size_t c = 0; c < _columnCount; ++c) _columns[c] = columns[c];
        return;
}

void TableModel::clear() {
        _rows.clear();
        _texts.clear();
        return;
}

Error TableModel::add(Span<const StringView> texts, Emphasis emphasis, int64_t key) {
        Row row;
        row.key = key;
        row.emphasis = emphasis;
        for (size_t c = 0; c < texts.size() && c < MaxColumns; ++c) {
                const StringView t = texts[c].size() <= UINT16_MAX ? texts[c] : texts[c].left(UINT16_MAX);
                row.at[c] = uint32_t(_texts.size());
                row.length[c] = uint16_t(t.size());
                if (_texts.reserve(_texts.size() + t.size()).isError()) return Error::NoMem;
                for (const char ch : t) (void)_texts.append(ch); // reserved
        }
        return _rows.append(row);
}

int TableModel::find(int64_t key) const {
        for (size_t i = 0; i < _rows.size(); ++i)
                if (_rows[i].key == key) return int(i);
        return -1;
}

StringView TableModel::cell(size_t row, size_t column) const {
        if (row >= _rows.size() || column >= MaxColumns) return StringView();
        const Row &r = _rows[row];
        return r.length[column] == 0 ? StringView() : StringView(_texts.data() + r.at[column], r.length[column]);
}

StringView TableModel::columnTitle(size_t column) const {
        return column < _columnCount ? StringView(_columns[column].title) : StringView();
}

StringView TableModel::columnText(size_t row, size_t column, Span<char> buffer) const {
        (void)buffer;
        return cell(row, column);
}

ItemModel::Align TableModel::columnAlign(size_t column) const {
        return column < _columnCount ? _columns[column].align : Align::Left;
}

uint16_t TableModel::columnWidth(size_t column) const {
        return column < _columnCount ? _columns[column].width : 100;
}

BMDS_NAMESPACE_END
