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

void WindowStack::take(size_t from, size_t to) {
        const Rect    rect = _rects[from];
        const uint8_t id = _ids[from];
        for (; from < to; ++from) {
                _rects[from] = _rects[from + 1];
                _ids[from] = _ids[from + 1];
        }
        for (; from > to; --from) {
                _rects[from] = _rects[from - 1];
                _ids[from] = _ids[from - 1];
        }
        _rects[to] = rect;
        _ids[to] = id;
        return;
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

Result<uint8_t> WindowStack::open(const Rect &rect) {
        if (!isPlaceable(rect)) return makeError<uint8_t>(Error::Invalid);
        const size_t total = size_t(_count) + _hidden;
        if (total == MaxWindows) return makeError<uint8_t>(Error::Full);
        uint8_t id = 1;
        while (find(id) >= 0) ++id;
        _rects[total] = rect;
        _ids[total] = id;
        take(total, 0);
        ++_count;
        _damage.add(rect);
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
        take(size_t(i), 0);
        ++_count;
        --_hidden;
        _damage.add(_rects[0]);
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
        if (z == 0) return Error::Ok;
        // What the windows above covered of it: the plan's pieces not its own.
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
                        if (pieces[i].layer != z) _damage.add(pieces[i].rect);
        }
        take(z, 0);
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

BMDS_NAMESPACE_END
