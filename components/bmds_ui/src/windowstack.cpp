/**
 * @file      windowstack.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The windows front to back and the damage their changes leave
 * (devplan/windows.md §4a).
 */

#include <bmds/windowstack.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxPieces = 64; // a change's plan, on the stack (640 bytes)

} // namespace

void WindowStack::reset(uint16_t width, uint16_t height) {
        _screen = Rect{0, 0, width, height};
        _count = 0;
        _hidden = 0;
        _damage.clear();
        _damage.add(_screen);
        return;
}

Result<size_t> WindowStack::layerOf(uint8_t id) const {
        for (size_t z = 0; z < _count; ++z)
                if (_ids[z] == id) return makeResult(z);
        return makeError<size_t>(Error::NotFound);
}

int WindowStack::find(uint8_t id) const {
        for (size_t i = 0; i < size_t(_count) + _hidden; ++i)
                if (_ids[i] == id) return int(i);
        return -1;
}

Result<Rect> WindowStack::rectOf(uint8_t id) const {
        const int i = find(id);
        return i < 0 ? makeError<Rect>(Error::NotFound) : makeResult(_rects[i]);
}

bool WindowStack::isHidden(uint8_t id) const {
        return find(id) >= int(_count);
}

WindowStack::Level WindowStack::levelOf(uint8_t id) const {
        const int i = find(id);
        return i < 0 ? Level::Normal : _levels[i];
}

void WindowStack::take(size_t from, size_t to) {
        const Rect    rect = _rects[from];
        const uint8_t id = _ids[from];
        const Level   level = _levels[from];
        for (; from < to; ++from) {
                _rects[from] = _rects[from + 1];
                _ids[from] = _ids[from + 1];
                _levels[from] = _levels[from + 1];
        }
        for (; from > to; --from) {
                _rects[from] = _rects[from - 1];
                _ids[from] = _ids[from - 1];
                _levels[from] = _levels[from - 1];
        }
        _rects[to] = rect;
        _ids[to] = id;
        _levels[to] = level;
        return;
}

// The layer a window of @p level goes to on top of it: under every shown
// window of a higher level (entry @p skip, the one moving, not counted).
size_t WindowStack::topOf(Level level, size_t skip) const {
        size_t z = 0;
        for (size_t i = 0; i < _count; ++i)
                if (i != skip && _levels[i] > level) ++z;
        return z;
}

bool WindowStack::isPlaceable(const Rect &rect) const {
        return !rect.isEmpty() && _screen.contains(rect) && rect.x % Align == 0 && rect.width % Align == 0;
}

void WindowStack::layers(Rect (&out)[MaxWindows + 1]) const {
        for (size_t z = 0; z < _count; ++z) out[z] = _rects[z];
        out[_count] = _screen;
        return;
}

void WindowStack::addShown(size_t z, const Rect &rect) {
        const Rect r = rect.intersected(rectAt(z));
        if (r.isEmpty()) return;
        if (z == 0) {
                _damage.add(r);
                return;
        }
        Rect all[MaxWindows + 1];
        layers(all);
        Region damage;
        damage.add(r);
        ComposePiece pieces[MaxPieces];
        auto [n, err] = planCompose(Span<const Rect>(all, z + 1), damage, Span<ComposePiece>(pieces, MaxPieces));
        if (err.isError()) {
                _damage.add(r); // more than needed, never less
                return;
        }
        for (size_t i = 0; i < n; ++i)
                if (pieces[i].layer == z) _damage.add(pieces[i].rect);
        return;
}

Result<uint8_t> WindowStack::open(const Rect &rect, Level level) {
        if (!isPlaceable(rect)) return makeError<uint8_t>(Error::Invalid);
        const size_t total = size_t(_count) + _hidden;
        if (total == MaxWindows) return makeError<uint8_t>(Error::Full);
        uint8_t id = 1;
        while (find(id) >= 0) ++id;
        _rects[total] = rect;
        _ids[total] = id;
        _levels[total] = level;
        const size_t z = topOf(level, MaxWindows);
        take(total, z);
        ++_count;
        addShown(z, rect);
        return makeResult(id);
}

Error WindowStack::close(uint8_t id) {
        const int i = find(id);
        if (i < 0) return Error::NotFound;
        const size_t last = size_t(_count) + _hidden - 1;
        if (i >= int(_count)) {
                take(size_t(i), last);
                --_hidden;
                return Error::Ok;
        }
        // What it showed, before it goes: then the layers under show it.
        addShown(size_t(i), _rects[i]);
        take(size_t(i), last);
        --_count;
        return Error::Ok;
}

Error WindowStack::setLevel(uint8_t id, Level level) {
        const int i = find(id);
        if (i < 0) return Error::NotFound;
        if (_levels[i] == level) return Error::Ok;
        _levels[i] = level;
        if (i >= int(_count)) return Error::Ok; // hidden: shown on top of its new level later
        // Up or down past other windows: all of it may change hands (rare: a whole redraw).
        const Rect rect = _rects[i];
        take(size_t(i), topOf(level, size_t(i)));
        invalidateScreen(rect);
        return Error::Ok;
}

Error WindowStack::hide(uint8_t id) {
        auto [z, err] = layerOf(id);
        if (err.isError()) return err;
        addShown(z, _rects[z]);
        take(z, size_t(_count) - 1); // the first hidden
        --_count;
        ++_hidden;
        return Error::Ok;
}

Error WindowStack::show(uint8_t id) {
        const int i = find(id);
        if (i < int(_count)) return Error::NotFound;
        const size_t z = topOf(_levels[i], MaxWindows);
        take(size_t(i), z);
        ++_count;
        --_hidden;
        addShown(z, _rects[z]);
        return Error::Ok;
}

Error WindowStack::move(uint8_t id, const Rect &rect) {
        const int i = find(id);
        if (i < 0) return Error::NotFound;
        if (!isPlaceable(rect)) return Error::Invalid;
        if (i >= int(_count)) {
                _rects[i] = rect;
                return Error::Ok;
        }
        const auto z = size_t(i);
        if (rect == _rects[z]) return Error::Ok;
        addShown(z, _rects[z]);
        _rects[z] = rect;
        addShown(z, rect);
        return Error::Ok;
}

Error WindowStack::raise(uint8_t id) {
        auto [z, err] = layerOf(id);
        if (err.isError()) return err;
        const size_t to = topOf(_levels[z], z);
        if (to >= z) return Error::Ok;
        // What the windows it goes above covered of it: the plan's pieces
        // theirs (what the windows still above it cover stays covered).
        const Rect rect = _rects[z];
        Rect       all[MaxWindows + 1];
        layers(all);
        Region damage;
        damage.add(rect);
        ComposePiece pieces[MaxPieces];
        auto [n, perr] = planCompose(Span<const Rect>(all, z + 1), damage, Span<ComposePiece>(pieces, MaxPieces));
        if (perr.isError()) {
                _damage.add(rect);
        } else {
                for (size_t i = 0; i < n; ++i)
                        if (pieces[i].layer >= to && pieces[i].layer < z) _damage.add(pieces[i].rect);
        }
        take(z, to);
        return Error::Ok;
}

Error WindowStack::invalidate(uint8_t id, const Rect &rect) {
        auto [z, err] = layerOf(id);
        if (err.isError()) return err;
        addShown(z, rect);
        return Error::Ok;
}

void WindowStack::invalidateDesktop(const Rect &rect) {
        addShown(_count, rect);
        return;
}

void WindowStack::invalidateScreen(const Rect &rect) {
        const Rect r = rect.intersected(_screen);
        if (!r.isEmpty()) _damage.add(r);
        return;
}

Result<size_t> WindowStack::plan(const Region &damage, Span<ComposePiece> out) const {
        Rect all[MaxWindows + 1];
        layers(all);
        return planCompose(Span<const Rect>(all, size_t(_count) + 1), damage, out);
}

Rect cascadeRect(uint16_t width, uint16_t height, const Rect &area, Span<const Rect> taken) {
        constexpr uint32_t Margin = 16; // the first place, from the area's corner
        const uint32_t     left = (area.x + Margin + 3u) & ~3u;
        const uint32_t     top = area.y + Margin;
        Rect               first{static_cast<uint16_t>(left), static_cast<uint16_t>(top), width, height};
        if (left + width > area.right() || top + height > area.bottom()) {
                // Too big for the steps: as far up and left as it fits.
                first.x = static_cast<uint16_t>(width < area.width ? (area.x + 3u) & ~3u : area.x);
                first.y = area.y;
                return first;
        }
        const uint32_t down = (area.bottom() - top - height) / CascadeStep + 1; // places on a diagonal
        for (uint32_t k = 0; k < 256; ++k) {
                const uint32_t i = k % down;
                const uint32_t x = left + (i + 2 * (k / down)) * CascadeStep;
                const uint32_t y = top + i * CascadeStep;
                if (x + width > area.right()) break; // no diagonal left
                bool used = false;
                for (const Rect &t : taken) used = used || (t.x == x && t.y == y);
                if (!used) return Rect{static_cast<uint16_t>(x), static_cast<uint16_t>(y), width, height};
        }
        return first;
}

BMDS_NAMESPACE_END
