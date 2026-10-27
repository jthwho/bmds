/**
 * @file      glyphcache.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <new>
#include <bmds/glyphcache.h>

BMDS_NAMESPACE_BEGIN

struct GlyphCache::Entry {
                Entry                *next = nullptr;  // the bucket's chain
                Entry                *newer = nullptr; // the LRU list
                Entry                *older = nullptr;
                uint64_t              key = 0;
                uint32_t              bytes = 0;    // the allocation's
                std::atomic<uint32_t> pins{0};      // taken under the lock, given back without it
                bool                  used = false; // hit since the eviction sweep last passed it
                Glyph                 glyph;        // its rows follow the entry
};

namespace {

        size_t bucketOf(uint64_t key) {
                const uint32_t h = (uint32_t(key) ^ uint32_t(key >> 32) * 0x9e3779b1u) * 0x85ebca6bu;
                return size_t(h >> 20); // 12 bits: Buckets
        }

        // A pair's set: two ways, side by side.
        size_t pairSet(uint16_t faceId, uint16_t left, uint16_t right) {
                const uint32_t h =
                        (uint32_t(faceId) * 0x9e37u) ^ (uint32_t(left) * 0x85ebca6bu) ^ (uint32_t(right) * 0xc2b2ae35u);
                return ((h ^ (h >> 15)) & (GlyphCache::KernPairs / 2 - 1)) * 2;
        }

        // Smears @p rows (packed at @p depth, @p width wide) one pixel right into @p out, a pixel
        // wider: each pixel the most of itself and the one left of it.
        void embolden(const uint8_t *rows, size_t stride, uint16_t width, uint16_t height, FontFace::Depth depth,
                      uint8_t *out, size_t outStride) {
                for (uint32_t y = 0; y < height; ++y) {
                        const uint8_t *src = rows + y * stride;
                        uint8_t       *dst = out + y * outStride;
                        std::memset(dst, 0, outStride);
                        for (uint32_t x = 0; x <= width; ++x) {
                                const auto at = [&](uint32_t i) -> unsigned {
                                        if (i >= width) return 0;
                                        if (depth == FontFace::Depth::Mono) return (src[i / 8] >> (7 - i % 8)) & 1;
                                        return (i & 1) != 0 ? src[i / 2] & 0x0f : src[i / 2] >> 4;
                                };
                                const unsigned a = at(x), b = x > 0 ? at(x - 1) : 0;
                                const unsigned v = a > b ? a : b;
                                if (depth == FontFace::Depth::Mono)
                                        dst[x / 8] = static_cast<uint8_t>(dst[x / 8] | v << (7 - x % 8));
                                else
                                        dst[x / 2] = static_cast<uint8_t>(dst[x / 2] | ((x & 1) != 0 ? v : v << 4));
                        }
                }
                return;
        }

} // namespace

GlyphCache::GlyphCache(size_t budgetBytes) : _budget(budgetBytes) {}

GlyphCache::~GlyphCache() {
        for (Entry *e = _newest; e != nullptr;) {
                Entry *older = e->older;
                e->~Entry();
                Memory::free(e);
                e = older;
        }
        Memory::free(_buckets);
        Memory::free(_kern);
}

static_assert(FontFace::Phases <= 4, "a key holds two bits of phase");

uint64_t GlyphCache::keyOf(uint16_t faceId, uint16_t glyph, unsigned pixelSize, FontFace::Depth depth, bool bold,
                           unsigned phase) {
        return uint64_t(faceId) << 32 | uint64_t(glyph) << 16 | uint64_t(pixelSize & 0xff) << 8 |
               uint64_t(phase & 3u) << 2 | (depth == FontFace::Depth::Gray4 ? 1u : 0u) | (bold ? 2u : 0u);
}

bool GlyphCache::prepareSlow() {
        // Allocated outside the lock (a reclaim inside an allocation takes it), installed under it.
        auto *buckets = _buckets == nullptr
                                ? static_cast<Entry **>(Memory::alloc(MemSpace::Psram, Buckets * sizeof(Entry *)))
                                : nullptr;
        auto *kern = _kern == nullptr
                             ? static_cast<KernPair *>(Memory::alloc(MemSpace::Psram, KernPairs * sizeof(KernPair)))
                             : nullptr;
        if (buckets != nullptr) std::memset(buckets, 0, Buckets * sizeof(Entry *));
        if (kern != nullptr)
                for (size_t i = 0; i < KernPairs; ++i) new (&kern[i]) KernPair;
        Mutex::Locker lock(_mutex, WaitForever);
        if (buckets != nullptr) {
                if (_buckets == nullptr)
                        _buckets = buckets;
                else
                        Memory::free(buckets);
        }
        if (kern != nullptr) {
                if (_kern == nullptr)
                        _kern = kern;
                else
                        Memory::free(kern);
        }
        return _buckets != nullptr && _kern != nullptr;
}

GlyphCache::Entry *GlyphCache::find(uint64_t key) const {
        for (Entry *e = _buckets[bucketOf(key)]; e != nullptr; e = e->next)
                if (e->key == key) return e;
        return nullptr;
}

void GlyphCache::unlink(Entry *e) {
        if (e->newer != nullptr)
                e->newer->older = e->older;
        else
                _newest = e->older;
        if (e->older != nullptr)
                e->older->newer = e->newer;
        else
                _oldest = e->newer;
        e->newer = e->older = nullptr;
        return;
}

void GlyphCache::pushFront(Entry *e) {
        e->older = _newest;
        e->newer = nullptr;
        if (_newest != nullptr) _newest->newer = e;
        _newest = e;
        if (_oldest == nullptr) _oldest = e;
        return;
}

size_t GlyphCache::evict(size_t target) {
        // From the oldest: a pinned entry stays; one hit since the sweep last passed gets a second
        // chance (moved to the newest end); the rest go. Hits never move entries themselves.
        size_t freed = 0;
        size_t steps = 2 * _entries;
        for (Entry *e = _oldest; e != nullptr && _bytes > target && steps-- > 0;) {
                Entry *newer = e->newer;
                if (e->pins.load(std::memory_order_acquire) != 0) {
                        e = newer;
                        continue;
                }
                if (e->used) {
                        e->used = false;
                        if (newer != nullptr) {
                                unlink(e);
                                pushFront(e);
                        }
                        e = newer;
                        continue;
                }
                Entry **link = &_buckets[bucketOf(e->key)];
                while (*link != e) link = &(*link)->next;
                *link = e->next;
                unlink(e);
                _bytes -= e->bytes;
                freed += e->bytes;
                --_entries;
                ++_evictions;
                e->~Entry();
                Memory::free(e);
                e = newer;
        }
        return freed;
}

void GlyphCache::setCurve(const uint8_t (*curve)[256]) {
        Mutex::Locker lock(_mutex, WaitForever);
        if (curve == nullptr ? !_hasCurve : _hasCurve && std::memcmp(_curve, *curve, 256) == 0) return;
        _hasCurve = curve != nullptr;
        if (curve != nullptr) std::memcpy(_curve, *curve, 256);
        // The glyphs rendered with the old curve: every unpinned one.
        if (_buckets != nullptr) {
                const size_t budget = _budget;
                for (Entry *e = _oldest; e != nullptr; e = e->newer) e->used = false;
                _budget = 0;
                (void)evict(0);
                _budget = budget;
        }
        return;
}

void GlyphCache::setBudget(size_t budgetBytes) {
        Mutex::Locker lock(_mutex, WaitForever);
        _budget = budgetBytes;
        if (_buckets != nullptr) (void)evict(_budget);
        return;
}

Result<const GlyphCache::Glyph *> GlyphCache::acquire(uint16_t faceId, const FontFace &face, uint16_t glyph,
                                                      unsigned pixelSize, FontFace::Depth depth, bool bold,
                                                      unsigned phase) {
        if (!prepare()) return makeError<const Glyph *>(Error::NoMem);
        const uint64_t key = keyOf(faceId, glyph, pixelSize, depth, bold, phase);
        uint8_t        curve[256];
        bool           hasCurve = false;
        {
                Mutex::Locker lock(_mutex, WaitForever);
                if (Entry *e = find(key); e != nullptr) {
                        if (e->pins.fetch_add(1, std::memory_order_relaxed) == 0)
                                _pinned.fetch_add(1, std::memory_order_relaxed);
                        e->used = true;
                        ++_hits;
                        return makeResult(static_cast<const Glyph *>(&e->glyph));
                }
                ++_misses;
                hasCurve = _hasCurve;
                if (hasCurve) std::memcpy(curve, _curve, sizeof(curve));
        }

        // A miss: rendered outside the lock, into a byte a pixel in internal RAM, then packed.
        const auto fail = [&](Error err) {
                Mutex::Locker lock(_mutex, WaitForever);
                ++_failures;
                return makeError<const Glyph *>(err);
        };
        auto [box, berr] = face.box(glyph, pixelSize, phase);
        if (berr.isError()) return fail(berr);
        const size_t   stride = box.stride(depth);
        const uint16_t width = box.width == 0 ? 0 : static_cast<uint16_t>(box.width + (bold ? 1 : 0));
        const size_t   outStride = (size_t(width) * static_cast<uint8_t>(depth) + 7) / 8;
        const size_t   rowBytes = outStride * box.height;
        uint8_t       *work = nullptr;
        if (box.workBytes() != 0) {
                work = static_cast<uint8_t *>(Memory::alloc(MemSpace::Internal, box.workBytes()));
                if (work == nullptr) work = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, box.workBytes()));
                if (work == nullptr) return fail(Error::NoMem);
                if (Error rerr = face.render(glyph, pixelSize, depth, box, Span<uint8_t>(work, box.workBytes()),
                                             hasCurve ? curve : nullptr, phase);
                    rerr.isError()) {
                        Memory::free(work);
                        return fail(rerr);
                }
        }
        const size_t bytes = sizeof(Entry) + rowBytes;
        void        *memory = Memory::alloc(MemSpace::Psram, bytes);
        if (memory == nullptr) {
                Memory::free(work);
                return fail(Error::NoMem);
        }
        Entry *made = new (memory) Entry;
        auto  *rows = reinterpret_cast<uint8_t *>(made + 1);
        if (work != nullptr) {
                if (bold)
                        embolden(work, stride, box.width, box.height, depth, rows, outStride);
                else
                        std::memcpy(rows, work, rowBytes);
                Memory::free(work);
        }
        made->key = key;
        made->bytes = static_cast<uint32_t>(bytes);
        made->pins.store(1, std::memory_order_relaxed);
        made->glyph =
                Glyph{box.x, box.y, width, width == 0 ? uint16_t(0) : box.height, uint16_t(outStride), depth, rows};

        Mutex::Locker lock(_mutex, WaitForever);
        if (Entry *e = find(key); e != nullptr) { // another task rendered it meanwhile: theirs
                made->~Entry();
                Memory::free(made);
                if (e->pins.fetch_add(1, std::memory_order_relaxed) == 0)
                        _pinned.fetch_add(1, std::memory_order_relaxed);
                e->used = true;
                return makeResult(static_cast<const Glyph *>(&e->glyph));
        }
        Entry *&bucket = _buckets[bucketOf(key)];
        made->next = bucket;
        bucket = made;
        pushFront(made);
        _bytes += bytes;
        ++_entries;
        _pinned.fetch_add(1, std::memory_order_relaxed);
        (void)evict(_budget);
        return makeResult(static_cast<const Glyph *>(&made->glyph));
}

size_t GlyphCache::lookup(uint16_t faceId, Span<const uint16_t> glyphs, unsigned pixelSize, FontFace::Depth depth,
                          bool bold, Span<const Glyph *> out, Span<const uint8_t> phases) {
        size_t found = 0;
        for (size_t i = 0; i < out.size(); ++i) out[i] = nullptr;
        if (_buckets == nullptr) return 0;
        Mutex::Locker lock(_mutex, WaitForever);
        for (size_t i = 0; i < glyphs.size() && i < out.size(); ++i) {
                Entry *e = find(keyOf(faceId, glyphs[i], pixelSize, depth, bold, i < phases.size() ? phases[i] : 0));
                if (e == nullptr) continue;
                if (e->pins.fetch_add(1, std::memory_order_relaxed) == 0)
                        _pinned.fetch_add(1, std::memory_order_relaxed);
                e->used = true;
                out[i] = &e->glyph;
                ++found;
        }
        _hits += uint32_t(found);
        return found;
}

void GlyphCache::release(const Glyph *glyph) {
        if (glyph == nullptr) return;
        // Without the lock: an entry with pins is never freed, and pins only rise under the lock.
        auto *e = reinterpret_cast<Entry *>(reinterpret_cast<uintptr_t>(glyph) - offsetof(Entry, glyph));
        if (e->pins.fetch_sub(1, std::memory_order_release) == 1) _pinned.fetch_sub(1, std::memory_order_relaxed);
        return;
}

bool GlyphCache::kernRead(uint16_t faceId, uint16_t left, uint16_t right, int &value) const {
        const uint32_t  pair = uint32_t(left) << 16 | right;
        const KernPair *set = &_kern[pairSet(faceId, left, right)];
        for (size_t way = 0; way < 2; ++way) {
                const KernPair &p = set[way];
                const uint32_t  before = p.sequence.load(std::memory_order_acquire);
                if ((before & 1) != 0) continue; // being written: as a miss
                const uint32_t got = p.pair.load(std::memory_order_relaxed);
                const uint32_t faceValue = p.faceValue.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (p.sequence.load(std::memory_order_relaxed) != before) continue;
                if (got == pair && (faceValue >> 16) == faceId + 1u) {
                        value = int16_t(faceValue & 0xffff);
                        return true;
                }
        }
        return false;
}

namespace {

        void storePair(std::atomic<uint32_t> &sequence, std::atomic<uint32_t> &pair, std::atomic<uint32_t> &faceValue,
                       uint32_t newPair, uint32_t newFaceValue) {
                const uint32_t s0 = sequence.load(std::memory_order_relaxed);
                sequence.store(s0 + 1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                pair.store(newPair, std::memory_order_relaxed);
                faceValue.store(newFaceValue, std::memory_order_relaxed);
                sequence.store(s0 + 2, std::memory_order_release);
                return;
        }

} // namespace

void GlyphCache::kernWrite(uint16_t faceId, uint16_t left, uint16_t right, int value) {
        // The newest in way 0: what was there moves to way 1, whose pair goes.
        KernPair     *set = &_kern[pairSet(faceId, left, right)];
        const int16_t v = static_cast<int16_t>(value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : value);
        storePair(set[1].sequence, set[1].pair, set[1].faceValue, set[0].pair.load(std::memory_order_relaxed),
                  set[0].faceValue.load(std::memory_order_relaxed));
        storePair(set[0].sequence, set[0].pair, set[0].faceValue, uint32_t(left) << 16 | right,
                  (uint32_t(faceId) + 1u) << 16 | uint16_t(v));
        return;
}

int GlyphCache::kerning(uint16_t faceId, const FontFace &face, uint16_t left, uint16_t right) {
        if (!face.hasKerning()) return 0;
        if (!prepare()) return face.kerning(left, right);
        int value = 0;
        if (kernRead(faceId, left, right, value)) {
                _kernHits.fetch_add(1, std::memory_order_relaxed);
                return value;
        }
        _kernMisses.fetch_add(1, std::memory_order_relaxed);
        value = face.kerning(left, right);
        Mutex::Locker lock(_mutex, WaitForever); // writers one at a time
        kernWrite(faceId, left, right, value);
        return value;
}

void GlyphCache::kerning(uint16_t faceId, const FontFace &face, Span<const uint16_t> glyphs, Span<int16_t> out) {
        const size_t pairs = glyphs.size() > 0 ? glyphs.size() - 1 : 0;
        if (out.size() < pairs) return;
        for (size_t i = 0; i < pairs; ++i)
                out[i] = static_cast<int16_t>(kerning(faceId, face, glyphs[i], glyphs[i + 1]));
        return;
}

void GlyphCache::forget(uint16_t faceId) {
        Mutex::Locker lock(_mutex, WaitForever);
        if (_buckets != nullptr)
                for (Entry *e = _oldest; e != nullptr;) {
                        Entry *newer = e->newer;
                        if (uint16_t(e->key >> 32) == faceId && e->pins.load(std::memory_order_acquire) == 0) {
                                Entry **link = &_buckets[bucketOf(e->key)];
                                while (*link != e) link = &(*link)->next;
                                *link = e->next;
                                unlink(e);
                                _bytes -= e->bytes;
                                --_entries;
                                e->~Entry();
                                Memory::free(e);
                        }
                        e = newer;
                }
        if (_kern != nullptr)
                for (size_t i = 0; i < KernPairs; ++i) {
                        KernPair &p = _kern[i];
                        if ((p.faceValue.load(std::memory_order_relaxed) >> 16) == faceId + 1u)
                                storePair(p.sequence, p.pair, p.faceValue, 0, 0);
                }
        return;
}

GlyphCache::Stats GlyphCache::stats() const {
        Mutex::Locker lock(_mutex, WaitForever);
        Stats         s;
        s.entries = _entries;
        s.bytes = _bytes;
        s.budget = _budget;
        s.pinned = _pinned.load(std::memory_order_relaxed);
        s.hits = _hits;
        s.misses = _misses;
        s.evictions = _evictions;
        s.failures = _failures;
        s.kernHits = _kernHits.load(std::memory_order_relaxed);
        s.kernMisses = _kernMisses.load(std::memory_order_relaxed);
        return s;
}

size_t GlyphCache::reclaim(MemSpace space, size_t bytes) {
        if (space != MemSpace::Psram && space != MemSpace::PsramDma) return 0;
        Mutex::Locker lock(_mutex, WaitForever);
        if (_buckets == nullptr) return 0;
        const size_t freed = evict(_bytes > bytes ? _bytes - bytes : 0);
        if (freed != 0) ++_reclaims;
        return freed;
}

Memory::Reclaimer::Usage GlyphCache::usage() const {
        Mutex::Locker lock(_mutex, WaitForever);
        Usage         u;
        u.items = "glyphs";
        u.bytes = _bytes;
        u.count = _entries;
        size_t free = 0;
        for (const Entry *e = _oldest; e != nullptr; e = e->newer)
                if (e->pins.load(std::memory_order_acquire) == 0) free += e->bytes;
        u.reclaimable = free;
        u.hits = _hits;
        u.misses = _misses;
        u.reclaims = _reclaims;
        return u;
}

BMDS_NAMESPACE_END
