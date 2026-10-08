/**
 * @file      itemselection.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Which rows of a view are selected (devplan/filebrowser.md §4g).
 */

#include <bmds/itemselection.h>

BMDS_NAMESPACE_BEGIN

Error ItemSelection::reset(size_t count) {
        _bits.clear();
        _count = 0;
        _selected = 0;
        _current = -1;
        _anchor = -1;
        const size_t words = (count + 31) / 32;
        const Error  err = _bits.reserve(words);
        if (err.isError()) return err;
        for (size_t i = 0; i < words; ++i) (void)_bits.append(0);
        _count = count;
        return Error::Ok;
}

int ItemSelection::nextSelected(size_t from) const {
        for (size_t row = from; row < _count;) {
                const uint32_t word = _bits[row / 32] >> (row % 32);
                if (word == 0) {
                        row = (row / 32 + 1) * 32; // the rest of this word is clear
                        continue;
                }
                if ((word & 1u) != 0) return int(row);
                ++row;
        }
        return -1;
}

void ItemSelection::set(size_t row, bool selected) {
        if (row >= _count || isSelected(row) == selected) return;
        uint32_t      &word = _bits[row / 32];
        const uint32_t bit = 1u << (row % 32);
        word = selected ? word | bit : word & ~bit;
        _selected = selected ? _selected + 1 : _selected - 1;
        return;
}

void ItemSelection::setCurrent(int row, bool moveAnchor) {
        _current = row >= 0 && size_t(row) < _count ? row : -1;
        if (moveAnchor) _anchor = _current;
        return;
}

void ItemSelection::clear() {
        for (uint32_t &w : _bits) w = 0;
        _selected = 0;
        return;
}

void ItemSelection::selectOnly(int row) {
        clear();
        setCurrent(row);
        if (_current >= 0) set(size_t(_current), true);
        return;
}

void ItemSelection::toggle(size_t row) {
        if (row >= _count) return;
        set(row, !isSelected(row));
        setCurrent(int(row));
        return;
}

void ItemSelection::selectRange(size_t row, bool add) {
        if (row >= _count) return;
        const size_t from = _anchor >= 0 ? size_t(_anchor) : row;
        if (!add) clear();
        const size_t lo = from < row ? from : row, hi = from < row ? row : from;
        for (size_t r = lo; r <= hi; ++r) set(r, true);
        setCurrent(int(row), false); // the anchor stays for the next Shift
        if (_anchor < 0) _anchor = int(row);
        return;
}

void ItemSelection::selectAll() {
        for (size_t r = 0; r < _count; ++r) set(r, true);
        return;
}

BMDS_NAMESPACE_END
