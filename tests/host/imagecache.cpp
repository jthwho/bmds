/**
 * @file      imagecache.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The image cache: programs kept for the next exec (programs.md §14.1).
 */

#include <doctest.h>

#include <cstring>
#include <string>
#include <bmds/imagecache.h>
#include <bmds/memory.h>

#include "hostmemory.h"

using bmds::ImageCache;
using bmds::MemSpace;
namespace ProgramImage = bmds::ProgramImage;

namespace {

        ImageCache::Key key(uint64_t id, uint64_t size = 1000, int64_t modified = 5, uint32_t device = 1) {
                ImageCache::Key k;
                k.device = device;
                k.id = id;
                k.size = size;
                k.modified = modified;
                return k;
        }

        ProgramImage::Info info(uint32_t fileSize, uint32_t relocations = 2) {
                ProgramImage::Info i;
                i.fileSize = fileSize;
                i.imageSize = fileSize + 100;
                i.relocations = relocations;
                return i;
        }

        // An entry for @p k, filled with @p fill, offered to the cache; true if kept.
        bool add(ImageCache &cache, const ImageCache::Key &k, uint32_t fileSize, uint8_t fill = 0x11) {
                ImageCache::Entry *e = cache.create(k, info(fileSize), cache.generation());
                REQUIRE(e != nullptr);
                std::memset(e->bytes(), fill, fileSize);
                e->patches()[0] = 4;
                e->patches()[1] = 8 | ProgramImage::PatchUpper;
                return cache.keep(e);
        }

        // Bytes an entry of @p fileSize takes (its header, two patches and its bytes).
        size_t entryBytes(uint32_t fileSize) {
                return sizeof(ImageCache::Entry) + 2 * sizeof(ProgramImage::Patch) + fileSize;
        }

        bool has(ImageCache &cache, const ImageCache::Key &k) {
                ImageCache::Entry *e = cache.acquire(k);
                if (e != nullptr) cache.release(e);
                return e != nullptr;
        }

} // namespace

TEST_CASE("Image cache: an entry is found by its file's key, and only by it") {
        const size_t live = HostMemory::liveAllocations();
        {
                ImageCache cache(1 << 20);
                CHECK(add(cache, key(7), 1000, 0x42));
                ImageCache::Entry *e = cache.acquire(key(7));
                REQUIRE(e != nullptr);
                CHECK(e->info().fileSize == 1000);
                CHECK(e->bytes()[999] == 0x42);
                CHECK(e->patches()[1] == (8 | ProgramImage::PatchUpper));
                cache.release(e);
                CHECK_FALSE(has(cache, key(8)));                // another file
                CHECK_FALSE(has(cache, key(7, 1001)));          // grown
                CHECK_FALSE(has(cache, key(7, 1000, 6)));       // rewritten
                CHECK_FALSE(has(cache, key(7, 1000, 5, 2)));    // another filesystem
                CHECK_FALSE(cache.acquire(key(7, 1000, 5, 0))); // outside the filesystems
                const bmds::Memory::Reclaimer::Usage s = cache.usage();
                CHECK(s.count == 1);
                CHECK(s.bytes == entryBytes(1000));
                CHECK(s.hits == 1);
                CHECK(s.misses == 4);
                // A file outside the filesystems, or a second entry for a key, isn't kept.
                CHECK_FALSE(add(cache, key(9, 1000, 5, 0), 100));
                CHECK_FALSE(add(cache, key(7), 1000));
                CHECK(cache.usage().count == 1);
        }
        CHECK(HostMemory::liveAllocations() == live);
}

TEST_CASE("Image cache: the budget, least recently used first") {
        const size_t live = HostMemory::liveAllocations();
        {
                ImageCache cache(3 * entryBytes(1000));
                CHECK(add(cache, key(1), 1000));
                CHECK(add(cache, key(2), 1000));
                CHECK(add(cache, key(3), 1000));
                CHECK(has(cache, key(1))); // now the most recently used: 2 goes first
                CHECK(add(cache, key(4), 1000));
                CHECK_FALSE(has(cache, key(2)));
                CHECK(has(cache, key(1)));
                CHECK(has(cache, key(3)));
                CHECK(has(cache, key(4)));
                // The least recently used, pinned, stays; the next one goes.
                ImageCache::Entry *pinned = cache.acquire(key(3));
                REQUIRE(pinned != nullptr);
                CHECK(has(cache, key(4)));
                CHECK(has(cache, key(1)));
                CHECK(add(cache, key(5), 1000));
                CHECK_FALSE(has(cache, key(4)));
                CHECK(has(cache, key(3)));
                cache.release(pinned);
                CHECK(cache.usage().count == 3);
                // Bigger than the whole budget: not kept, and nothing went for it.
                CHECK_FALSE(add(cache, key(9), 4000));
                CHECK(cache.usage().count == 3);
                // Nothing kept with no budget.
                ImageCache none(0);
                CHECK_FALSE(add(none, key(1), 10));
        }
        CHECK(HostMemory::liveAllocations() == live);
}

TEST_CASE("Image cache: a changed file's entries go; one being used goes when released") {
        const size_t live = HostMemory::liveAllocations();
        {
                ImageCache cache(1 << 20);
                CHECK(add(cache, key(1), 100));
                CHECK(add(cache, key(2), 100));
                CHECK(add(cache, key(3, 100, 5, 2), 100));
                cache.changed(1, 2);
                CHECK_FALSE(has(cache, key(2)));
                CHECK(has(cache, key(1)));
                // Dropped while in use: still readable, freed by its release.
                ImageCache::Entry *e = cache.acquire(key(1));
                REQUIRE(e != nullptr);
                cache.changed(1, 1);
                CHECK_FALSE(has(cache, key(1)));
                CHECK(e->bytes()[0] == 0x11);
                cache.release(e);
                // (0, 0): every file (an unmount).
                CHECK(has(cache, key(3, 100, 5, 2)));
                cache.changed(0, 0);
                CHECK(cache.usage().count == 0);
        }
        CHECK(HostMemory::liveAllocations() == live);
}

TEST_CASE("Image cache: a file that changed while it was read isn't kept") {
        const size_t live = HostMemory::liveAllocations();
        {
                ImageCache cache(1 << 20);
                // The loader takes the generation before it looks at the file.
                const uint32_t     generation = cache.generation();
                ImageCache::Entry *e = cache.create(key(1), info(100), generation);
                REQUIRE(e != nullptr);
                cache.changed(1, 99); // any file: the change might have been this one's
                CHECK_FALSE(cache.keep(e));
                CHECK_FALSE(has(cache, key(1)));
                CHECK(add(cache, key(1), 100)); // a later read is kept
        }
        CHECK(HostMemory::liveAllocations() == live);
}

namespace {

        // Installs a reclaimer for one test.
        struct ReclaimerGuard {
                        explicit ReclaimerGuard(bmds::Memory::Reclaimer &r) { bmds::Memory::setReclaimer(&r); }
                        ~ReclaimerGuard() { bmds::Memory::setReclaimer(nullptr); }
                        ReclaimerGuard(const ReclaimerGuard &) = delete;
                        ReclaimerGuard &operator=(const ReclaimerGuard &) = delete;
        };

} // namespace

TEST_CASE("Image cache: memory running short empties it") {
        const size_t live = HostMemory::liveAllocations();
        {
                ImageCache cache(1 << 20);
                CHECK(add(cache, key(1), 1000));
                CHECK(add(cache, key(2), 1000));
                CHECK(add(cache, key(3), 1000));
                ImageCache::Entry *pinned = cache.acquire(key(3));
                REQUIRE(pinned != nullptr);
                CHECK(cache.usage().reclaimable == 2 * entryBytes(1000));
                CHECK(cache.usage().bytes == 3 * entryBytes(1000));
                // Only the PSRAM it holds; at least what was asked, least recently used first.
                CHECK(cache.reclaim(MemSpace::Internal, 1) == 0);
                CHECK(cache.reclaim(MemSpace::Psram, 1) == entryBytes(1000));
                CHECK_FALSE(has(cache, key(1)));
                CHECK(has(cache, key(2)));
                {
                        // A failed allocation asks the installed reclaimer, then tries again.
                        ReclaimerGuard                 guard(cache);
                        HostMemory::FailGuard          fail(0);
                        bmds::Memory::Reclaimer::Usage u;
                        REQUIRE(bmds::Memory::reclaimerUsage(u));
                        CHECK(u.reclaimable == entryBytes(1000));
                        CHECK(u.reclaims == 1); // the direct reclaim() above
                        CHECK(bmds::Memory::alloc(MemSpace::Internal, 16) == nullptr);
                        CHECK(cache.usage().count == 2);
                        CHECK(bmds::Memory::alloc(MemSpace::Psram, 16) == nullptr); // injected failures stay failures
                        CHECK(cache.usage().count == 1);                            // ... but it was asked
                        CHECK(cache.usage().reclaims == 2);
                }
                cache.release(pinned);
                CHECK(cache.clear() == entryBytes(1000));
                CHECK(cache.usage().count == 0);
                CHECK(cache.usage().reclaimable == 0);
                CHECK(cache.usage().items == std::string("programs"));
                bmds::Memory::Reclaimer::Usage none;
                CHECK_FALSE(bmds::Memory::reclaimerUsage(none)); // uninstalled
        }
        CHECK(HostMemory::liveAllocations() == live);
}
