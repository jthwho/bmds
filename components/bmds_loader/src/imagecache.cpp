/**
 * @file      imagecache.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <new>
#include <bmds/imagecache.h>

BMDS_NAMESPACE_BEGIN

ImageCache::~ImageCache() {
        (void)clear();
        return;
}

uint32_t ImageCache::generation() const {
        Mutex::Locker locker(_lock, WaitForever);
        return _generation;
}

ImageCache::Entry *ImageCache::acquire(const Key &key) {
        if (!key.isCacheable()) return nullptr;
        Mutex::Locker locker(_lock, WaitForever);
        for (Entry *e = _first; e != nullptr; e = e->_next) {
                if (e->_key != key) continue;
                // To the front: the most recently used.
                unlink(e);
                pushFront(e);
                ++e->_pins;
                ++_hits;
                return e;
        }
        ++_misses;
        return nullptr;
}

ImageCache::Entry *ImageCache::create(const Key &key, const ProgramImage::Info &info, uint32_t generation) {
        // Allocated outside the lock: a failed allocation may reclaim().
        const size_t bytes =
                sizeof(Entry) + size_t(info.relocations) * sizeof(ProgramImage::Patch) + size_t(info.fileSize);
        void *memory = Memory::alloc(MemSpace::Psram, bytes);
        if (memory == nullptr) return nullptr;
        auto *entry = ::new (memory) Entry();
        entry->_key = key;
        entry->_info = info;
        entry->_bytes = bytes;
        entry->_generation = generation;
        entry->_pins = 1;
        return entry;
}

bool ImageCache::keep(Entry *entry) {
        bool kept = false;
        {
                Mutex::Locker locker(_lock, WaitForever);
                bool may = entry->_key.isCacheable() && entry->_generation == _generation && entry->_bytes <= _budget;
                for (Entry *e = _first; may && e != nullptr; e = e->_next) may = e->_key != entry->_key;
                while (may && _held + entry->_bytes > _budget) may = evictOne();
                if (may) {
                        pushFront(entry);
                        kept = true;
                }
        }
        release(entry);
        return kept;
}

void ImageCache::release(Entry *entry) {
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (--entry->_pins != 0 || entry->_listed) return;
        }
        entry->~Entry();
        Memory::free(entry);
        return;
}

void ImageCache::changed(uint32_t device, uint64_t id) {
        Mutex::Locker locker(_lock, WaitForever);
        ++_generation;
        for (Entry *e = _first; e != nullptr;) {
                Entry *next = e->_next;
                if ((device == 0 && id == 0) || (e->_key.device == device && e->_key.id == id)) drop(e);
                e = next;
        }
        return;
}

size_t ImageCache::clear() {
        Mutex::Locker locker(_lock, WaitForever);
        const size_t  before = _held;
        while (evictOne()) {}
        return before - _held;
}

size_t ImageCache::reclaim(MemSpace space, size_t bytes) {
        if (space != MemSpace::Psram && space != MemSpace::PsramDma) return 0;
        Mutex::Locker locker(_lock, WaitForever);
        const size_t  before = _held;
        // Freeing @p bytes may not make a block that big (fragments); the
        // caller tries once more either way.
        while (before - _held < bytes && evictOne()) {}
        if (_held != before) ++_reclaims;
        return before - _held;
}

Memory::Reclaimer::Usage ImageCache::usage() const {
        Mutex::Locker locker(_lock, WaitForever);
        Usage         u;
        u.items = "programs";
        u.bytes = _held;
        u.count = _count;
        for (const Entry *e = _first; e != nullptr; e = e->_next)
                if (e->_pins == 0) u.reclaimable += e->_bytes;
        u.hits = _hits;
        u.misses = _misses;
        u.reclaims = _reclaims;
        return u;
}

void ImageCache::pushFront(Entry *entry) {
        entry->_listed = true;
        entry->_prev = nullptr;
        entry->_next = _first;
        if (_first != nullptr) _first->_prev = entry;
        _first = entry;
        if (_last == nullptr) _last = entry;
        ++_count;
        _held += entry->_bytes;
        return;
}

void ImageCache::unlink(Entry *entry) {
        if (!entry->_listed) return;
        if (entry->_prev != nullptr)
                entry->_prev->_next = entry->_next;
        else
                _first = entry->_next;
        if (entry->_next != nullptr)
                entry->_next->_prev = entry->_prev;
        else
                _last = entry->_prev;
        entry->_prev = nullptr;
        entry->_next = nullptr;
        entry->_listed = false;
        --_count;
        _held -= entry->_bytes;
        return;
}

void ImageCache::drop(Entry *entry) {
        unlink(entry);
        if (entry->_pins != 0) return;
        entry->~Entry();
        Memory::free(entry);
        return;
}

bool ImageCache::evictOne() {
        for (Entry *e = _last; e != nullptr; e = e->_prev) {
                if (e->_pins != 0) continue;
                drop(e);
                return true;
        }
        return false;
}

BMDS_NAMESPACE_END
