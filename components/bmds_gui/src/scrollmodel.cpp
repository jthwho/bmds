/**
 * @file      scrollmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a scroll bar shows (devplan/filebrowser.md §4e).
 */

#include <bmds/scrollmodel.h>

BMDS_NAMESPACE_BEGIN

void ScrollModel::setRange(uint32_t total, uint32_t page) {
        _total = total;
        _page = page;
        if (_value > maximum()) _value = maximum();
        return;
}

bool ScrollModel::setValue(int64_t value) {
        const int64_t  most = maximum();
        const uint32_t v = static_cast<uint32_t>(value < 0 ? 0 : value > most ? most : value);
        if (v == _value) return false;
        _value = v;
        return true;
}

bool ScrollModel::act(Part part) {
        switch (part) {
                case Part::BackArrow: return stepLines(-1);
                case Part::ForwardArrow: return stepLines(1);
                case Part::BackPage: return stepPages(-1);
                case Part::ForwardPage: return stepPages(1);
                default: return false;
        }
}

ScrollModel::Geometry ScrollModel::geometry(uint16_t length, uint16_t arrow) const {
        Geometry g;
        g.arrow = length >= 2 * arrow ? arrow : static_cast<uint16_t>(length / 2);
        g.trackStart = g.arrow;
        g.trackLength = static_cast<uint16_t>(length - 2 * g.arrow);
        if (!isScrollable() || g.trackLength < MinThumb) return g;
        uint32_t thumb = uint32_t(uint64_t(g.trackLength) * _page / _total);
        if (thumb < MinThumb) thumb = MinThumb;
        if (thumb > g.trackLength) thumb = g.trackLength;
        const uint32_t room = g.trackLength - thumb;
        const uint32_t most = maximum();
        g.thumbLength = static_cast<uint16_t>(thumb);
        g.thumbStart = static_cast<uint16_t>(g.trackStart + (uint64_t(room) * _value + most / 2) / most);
        return g;
}

ScrollModel::Part ScrollModel::partAt(const Geometry &geometry, uint16_t length, int position) {
        if (position < 0 || position >= length) return Part::None;
        if (position < geometry.trackStart) return Part::BackArrow;
        if (position >= geometry.trackStart + geometry.trackLength) return Part::ForwardArrow;
        if (geometry.thumbLength == 0) return Part::None; // a track with nothing to scroll
        if (position < geometry.thumbStart) return Part::BackPage;
        if (position < geometry.thumbStart + geometry.thumbLength) return Part::Thumb;
        return Part::ForwardPage;
}

uint32_t ScrollModel::valueForThumb(const Geometry &geometry, int thumbStart) const {
        const int room = geometry.trackLength - geometry.thumbLength;
        if (room <= 0 || geometry.thumbLength == 0) return _value;
        int at = thumbStart - geometry.trackStart;
        if (at < 0) at = 0;
        if (at > room) at = room;
        return static_cast<uint32_t>((uint64_t(at) * maximum() + uint32_t(room) / 2) / uint32_t(room));
}

BMDS_NAMESPACE_END
