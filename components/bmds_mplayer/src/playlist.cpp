/**
 * @file      playlist.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "playlist.h"

#include <initializer_list>
#include <utility>

BMDS_NAMESPACE_BEGIN

namespace {

        // Texts left unused that make a compaction worth it.
        constexpr size_t CompactAfter = 4096;

} // namespace

Playlist::Playlist(MemSpace space) : _entries(space), _texts(space), _order(space) {}

Error Playlist::store(StringView s, Text &out) {
        if (s.size() > UINT16_MAX) return Error::Invalid;
        const size_t at = _texts.size();
        for (const char c : s) {
                if (_texts.append(c).isError()) {
                        _texts.truncate(at);
                        return Error::NoMem;
                }
        }
        out.at = static_cast<uint32_t>(at);
        out.length = static_cast<uint16_t>(s.size());
        return Error::Ok;
}

void Playlist::release(const Entry &e) {
        release(e.path);
        release(e.title);
        release(e.artist);
        release(e.album);
        release(e.format);
        return;
}

// The texts again without the unused ones (skipped if there's no memory for it).
void Playlist::compact() {
        List<char> texts(_texts.space());
        if (texts.reserve(_texts.size() - _unused).isError()) return;
        for (Entry &e : _entries) {
                for (Text *t : {&e.path, &e.title, &e.artist, &e.album, &e.format}) {
                        const uint32_t at = static_cast<uint32_t>(texts.size());
                        for (size_t i = 0; i < t->length; ++i) (void)texts.append(_texts[t->at + i]); // reserved
                        t->at = at;
                }
        }
        _texts = std::move(texts);
        _unused = 0;
        return;
}

Playlist::Info Playlist::info(size_t index) const {
        const Entry &e = _entries[index];
        Info         i;
        i.title = view(e.title);
        i.artist = view(e.artist);
        i.album = view(e.album);
        i.format = view(e.format);
        i.lengthMs = e.lengthMs;
        i.estimated = e.estimated;
        i.known = e.known;
        return i;
}

Error Playlist::insert(size_t at, StringView path) {
        if (_entries.size() >= MaxEntries) return Error::Full;
        if (path.isEmpty()) return Error::Invalid;
        if (at > _entries.size()) at = _entries.size();
        Entry e;
        Error err = _entries.reserve(_entries.size() + 1);
        if (err.isOk() && _shuffled) err = _order.reserve(_order.size() + 1);
        if (err.isOk()) err = store(path, e.path);
        if (err.isError()) return err;
        (void)_entries.insert(at, std::move(e)); // reserved
        if (_shuffled) {
                for (uint32_t &v : _order)
                        if (v >= at) ++v;
                // Somewhere in what is left of the round.
                const size_t first = _at == NoEntry ? 0 : _at + (_currentGone ? 0 : 1);
                const size_t place = first + random(static_cast<uint32_t>(_order.size() - first + 1));
                (void)_order.insert(place, static_cast<uint32_t>(at)); // reserved
        } else if (_at != NoEntry && at <= _at && !(_currentGone && at == _at)) {
                ++_at;
        }
        return Error::Ok;
}

Error Playlist::setInfo(size_t index, const Info &info) {
        Entry       &e = _entries[index];
        Entry        n = e;
        const size_t mark = _texts.size();
        Error        err = store(info.title, n.title);
        if (err.isOk()) err = store(info.artist, n.artist);
        if (err.isOk()) err = store(info.album, n.album);
        if (err.isOk()) err = store(info.format, n.format);
        if (err.isError()) {
                _texts.truncate(mark);
                return err;
        }
        release(e.title);
        release(e.artist);
        release(e.album);
        release(e.format);
        n.lengthMs = info.lengthMs;
        n.estimated = info.estimated;
        n.known = info.known;
        e = n;
        if (_unused > CompactAfter && _unused > _texts.size() / 2) compact();
        return Error::Ok;
}

void Playlist::remove(size_t index) {
        if (index >= _entries.size()) return;
        release(_entries[index]);
        _entries.removeAt(index);
        size_t position = index;
        if (_shuffled) {
                for (size_t p = 0; p < _order.size(); ++p)
                        if (_order[p] == index) position = p;
                _order.removeAt(position);
                for (uint32_t &v : _order)
                        if (v > index) --v;
        }
        if (_entries.isEmpty()) {
                _at = NoEntry;
                _currentGone = false;
        } else if (_at != NoEntry) {
                if (position < _at) {
                        --_at;
                } else if (position == _at) {
                        _currentGone = true; // _at is now the one that followed
                }
        }
        if (_entries.isEmpty()) {
                _texts.clear();
                _unused = 0;
        } else if (_unused > CompactAfter && _unused > _texts.size() / 2) {
                compact();
        }
        return;
}

void Playlist::move(size_t from, size_t to) {
        const size_t n = _entries.size();
        if (from >= n || to >= n || from == to) return;
        Entry e = _entries[from];
        _entries.removeAt(from);
        (void)_entries.insert(to, std::move(e)); // the room is there
        // Where an entry's index was, and where it is now.
        const auto moved = [from, to](size_t v) -> size_t {
                if (v == from) return to;
                if (from < to && v > from && v <= to) return v - 1;
                if (to < from && v >= to && v < from) return v + 1;
                return v;
        };
        if (_shuffled) {
                for (uint32_t &v : _order) v = static_cast<uint32_t>(moved(v));
        } else if (_at != NoEntry && !_currentGone) {
                _at = moved(_at);
        }
        return;
}

void Playlist::clear() {
        _entries.clear();
        _texts.clear();
        _order.clear();
        _unused = 0;
        _at = NoEntry;
        _currentGone = false;
        return;
}

size_t Playlist::orderEntry(size_t position) const {
        if (position == NoEntry || position >= orderSize()) return NoEntry;
        return _shuffled ? _order[position] : position;
}

uint32_t Playlist::random(uint32_t below) {
        // xorshift32: enough to shuffle a playlist.
        _random ^= _random << 13;
        _random ^= _random >> 17;
        _random ^= _random << 5;
        return static_cast<uint32_t>((static_cast<uint64_t>(_random) * below) >> 32);
}

Error Playlist::shuffleFrom(size_t first, size_t avoid) {
        const size_t n = _entries.size();
        _order.clear();
        if (_order.reserve(n).isError()) return Error::NoMem;
        for (size_t i = 0; i < n; ++i) (void)_order.append(static_cast<uint32_t>(i)); // reserved
        for (size_t i = n; i > 1; --i) {
                const size_t   j = random(static_cast<uint32_t>(i));
                const uint32_t v = _order[i - 1];
                _order[i - 1] = _order[j];
                _order[j] = v;
        }
        size_t swap = NoEntry;
        if (first != NoEntry) {
                for (size_t p = 0; p < n; ++p)
                        if (_order[p] == first) swap = p;
        } else if (n > 1 && _order[0] == avoid) {
                swap = 1 + random(static_cast<uint32_t>(n - 1));
        }
        if (swap != NoEntry) {
                const uint32_t v = _order[0];
                _order[0] = _order[swap];
                _order[swap] = v;
        }
        return Error::Ok;
}

Error Playlist::setShuffled(bool shuffled) {
        if (shuffled == _shuffled) return Error::Ok;
        if (shuffled) {
                const size_t c = current();
                const Error  err = shuffleFrom(c, NoEntry);
                if (err.isError()) return err;
                _shuffled = true;
                _at = c == NoEntry ? NoEntry : 0;
                _currentGone = false;
                return Error::Ok;
        }
        // Off: the same entry current, or the same one next.
        const size_t e = orderEntry(_at);
        _shuffled = false;
        _order.clear();
        _at = e;
        if (e == NoEntry) _currentGone = false;
        return Error::Ok;
}

void Playlist::setCurrent(size_t index) {
        if (index >= _entries.size()) return;
        _currentGone = false;
        if (_shuffled && shuffleFrom(index, NoEntry).isOk()) {
                _at = 0;
                return;
        }
        if (_shuffled) { // no memory for a new order: its place in this one
                for (size_t p = 0; p < _order.size(); ++p)
                        if (_order[p] == index) _at = p;
                return;
        }
        _at = index;
        return;
}

size_t Playlist::step(bool byUser) {
        const size_t n = orderSize();
        if (n == 0) {
                _at = NoEntry;
                _currentGone = false;
                return NoEntry;
        }
        if (_at == NoEntry) { // starting: a fresh round when shuffling
                if (_shuffled) (void)shuffleFrom(NoEntry, NoEntry);
                _at = 0;
                _currentGone = false;
                return current();
        }
        if (!byUser && !_currentGone && _repeat == Repeat::One) return current();
        const size_t position = _currentGone ? _at : _at + 1;
        const size_t last = current();
        _currentGone = false;
        if (position < n) {
                _at = position;
                return current();
        }
        if (_repeat == Repeat::Off) {
                _at = NoEntry;
                return NoEntry;
        }
        if (_shuffled) (void)shuffleFrom(NoEntry, last); // a new round, not starting with the last
        _at = 0;
        return current();
}

size_t Playlist::follow() {
        return step(false);
}

size_t Playlist::next() {
        return step(true);
}

size_t Playlist::previous() {
        if (_at == NoEntry) return NoEntry;
        if (_currentGone) {
                _currentGone = false;
                if (_at > 0) {
                        --_at;
                        return current();
                }
                return current(); // the first: the one now there
        }
        if (_at > 0) --_at;
        return current();
}

BMDS_NAMESPACE_END
