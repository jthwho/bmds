/**
 * @file      usermemory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A U-mode program's memory (devplan/protection.md §15): window ranges of its own, mapped page by
 * page from the PSRAM heap, its blocks' records here. The caches are tagged by address
 * (devplan/psramwindow.md §2): pages are taken with their heap addresses' lines dropped, a range
 * is invalidated after mapping and before unmapping.
 */

#include "usermemory.h"

#include <cstring>
#include <new>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/psramwindow.h>
#include <bmds/sys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Page = uint32_t(PsramWindow::PageBytes);

        // Window pages a range keeps above its top for the heap to grow into in place.
        constexpr uint32_t HeadroomPages = 4;

        constexpr uint8_t Rx = PmpSet::Read | PmpSet::Exec;
        constexpr uint8_t Rw = PmpSet::Read | PmpSet::Write;
        constexpr uint8_t Rwx = Rw | PmpSet::Exec;

        uint32_t address(const void *p) {
                return uint32_t(reinterpret_cast<uintptr_t>(p));
        }

        uint32_t pagesFor(uint32_t bytes) {
                return uint32_t((uint64_t(bytes) + Page - 1) / Page);
        }

        uint32_t roundUp(uint32_t value, uint32_t to) {
                return (value + to - 1) / to * to;
        }

} // namespace

UserMemory *UserMemory::create(uint32_t imageBytes, uint32_t codeBytes) {
        if (!PsramWindow::isReady() || imageBytes == 0) return nullptr;
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(UserMemory));
        if (memory == nullptr) return nullptr;
        auto          *m = ::new (memory) UserMemory();
        const uint32_t imagePages = pagesFor(imageBytes);
        m->_imageBytes = imageBytes;
        m->_codeBytes = codeBytes < imageBytes ? codeBytes : imageBytes;
        m->_space = UserSpace::create();
        uint32_t reserve = imagePages + HeadroomPages;
        uint8_t *at = m->_space != nullptr ? PsramWindow::takeRange(reserve, false) : nullptr;
        if (at == nullptr && m->_space != nullptr) {
                reserve = imagePages;
                at = PsramWindow::takeRange(reserve, false);
        }
        if (at == nullptr) {
                m->unref();
                return nullptr;
        }
        Range &r = m->_ranges[0];
        r.base = at;
        r.reserved = reserve;
        r.imagePages = imagePages;
        m->_rangeCount = 1;
        return m;
}

UserMemory::~UserMemory() {
        for (Internal &b : _internal) Memory::free(b.block);
        for (Large &l : _large) release(l.range);
        for (unsigned i = 0; i < _rangeCount; ++i) release(_ranges[i]);
        UserSpace::destroy(_space);
        return;
}

void UserMemory::ref() {
        _refs.fetch_add(1, std::memory_order_relaxed);
        return;
}

void UserMemory::unref() {
        if (_refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        this->~UserMemory();
        Memory::free(this);
        return;
}

// ---- Ranges

// Maps @p pages more pages of the heap's at @p r's top (within its reservation), zeroed if
// @p zero. Not yet granted.
Error UserMemory::map(Range &r, uint32_t pages, bool zero) {
        if (r.mapped + pages > r.reserved) return Error::NoMem;
        PageRuns::Run got[PageRuns::MaxRuns];
        const size_t  n = PsramWindow::takeHeapPages(pages, Span<PageRuns::Run>(got, PageRuns::MaxRuns));
        if (n == 0) return Error::NoMem;
        if (r.runs.reserve(r.runs.size() + n).isError()) {
                PsramWindow::giveHeapPages(Span<const PageRuns::Run>(got, n));
                return Error::NoMem;
        }
        uint8_t *const at = r.base + size_t(r.mapped) * Page;
        uint32_t       page = r.mapped;
        for (size_t i = 0; i < n; ++i) {
                PsramWindow::mapHeap(r.base + size_t(page) * Page, got[i].at, got[i].pages);
                (void)r.runs.append(Run{got[i].at, got[i].pages, page});
                page += got[i].pages;
        }
        PsramWindow::invalidate(at, size_t(pages) * Page);
        // Another program's data or the kernel's may be in them.
        if (zero) std::memset(at, 0, size_t(pages) * Page);
        r.mapped += pages;
        return Error::Ok;
}

// Unmaps @p r's last @p pages (whole runs; no longer granted) and gives them back.
void UserMemory::unmapTail(Range &r, uint32_t pages) {
        const uint32_t from = r.mapped - pages;
        uint8_t *const at = r.base + size_t(from) * Page;
        PsramWindow::invalidate(at, size_t(pages) * Page);
        PsramWindow::clear(at, pages);
        while (r.runs.size() > 0 && r.runs[r.runs.size() - 1].page >= from) {
                const Run          &last = r.runs[r.runs.size() - 1];
                const PageRuns::Run run{last.at, last.pages};
                PsramWindow::giveHeapPages(Span<const PageRuns::Run>(&run, 1));
                r.runs.truncate(r.runs.size() - 1);
        }
        r.mapped = from;
        return;
}

// The range unmapped (its own pages: the image's mapping ends before) and its addresses given back.
void UserMemory::release(Range &r) {
        if (r.base == nullptr) return;
        for (size_t i = 0; i < r.runs.size(); ++i) {
                uint8_t *at = r.base + size_t(r.runs[i].page) * Page;
                PsramWindow::invalidate(at, size_t(r.runs[i].pages) * Page);
                PsramWindow::clear(at, r.runs[i].pages);
                const PageRuns::Run run{r.runs[i].at, r.runs[i].pages};
                PsramWindow::giveHeapPages(Span<const PageRuns::Run>(&run, 1));
        }
        r.runs.clear();
        PsramWindow::giveRange(r.base, r.reserved);
        r.base = nullptr;
        r.reserved = r.mapped = r.imagePages = 0;
        r.region = r.grantTop = 0;
        r.granted = false;
        return;
}

Error UserMemory::backImage() {
        Mutex::Locker locker(_lock, WaitForever);
        Range        &r = _ranges[0];
        if (r.mapped != 0) return Error::Invalid;
        return map(r, r.imagePages, false); // the image is written over it; the rest zeroed as granted
}

Error UserMemory::start() {
        Mutex::Locker locker(_lock, WaitForever);
        Range        &r = _ranges[0];
        if (r.mapped < r.imagePages) r.mapped = r.imagePages; // ImageWindow mapped them
        uint8_t *const end = r.base + size_t(r.imagePages) * Page;
        uint8_t *const heap = r.base + roundUp(_imageBytes, RangeHeap::Unit);
        if (heap < end) {
                Error err = _heap.addRange(address(heap), uint32_t(end - heap));
                if (err.isError()) return err;
        }
        // Its code and constants (read, run), the granule holding both its last code and its first
        // data (read, write, run: the page it is in is the program's own, never shared,
        // ProgramImage::isSharedPage()), then its data up to the granule its end is in, whatever
        // lies past its end there zeroed. The heap's blocks are granted as made.
        const uintptr_t base = reinterpret_cast<uintptr_t>(r.base);
        const uint32_t  codeDown = _codeBytes / PmpSet::Granule * PmpSet::Granule;
        const uint32_t  codeUp = roundUp(_codeBytes, PmpSet::Granule);
        if (codeDown != 0) {
                Error err = _space->grant(base, codeDown, Rx, 0);
                if (err.isError()) return err;
        }
        if (codeUp != codeDown) {
                Error err = _space->grant(base + codeDown, codeUp - codeDown, Rwx, 0);
                if (err.isError()) return err;
        }
        const uintptr_t imageEnd = base + roundUp(_imageBytes, PmpSet::Granule);
        std::memset(r.base + _imageBytes, 0, imageEnd - base - _imageBytes);
        r.region = base + codeUp;
        r.grantTop = r.region;
        if (imageEnd > r.region) {
                Error err = _space->grant(r.region, imageEnd - r.region, Rw, 0);
                if (err.isError()) return err;
                r.granted = true;
                r.grantTop = imageEnd;
        }
        return Error::Ok;
}

// Grants @p r up to @p end at least (a GrantStep at a time, within what is mapped), what it adds
// zeroed first: it was another program's, or the kernel's.
Error UserMemory::raiseGrant(Range &r, uintptr_t end) {
        if (end <= r.grantTop) return Error::Ok;
        const uintptr_t top = reinterpret_cast<uintptr_t>(r.base) + size_t(r.mapped) * Page;
        uintptr_t       to = (end + GrantStep - 1) / GrantStep * GrantStep;
        if (to > top) to = top;
        std::memset(reinterpret_cast<void *>(r.grantTop), 0, to - r.grantTop);
        Error err = r.granted ? _space->resize(r.region, to - r.region) : _space->grant(r.region, to - r.region, Rw, 0);
        if (err.isError()) return err;
        r.granted = true;
        r.grantTop = to;
        return Error::Ok;
}

// ---- The heap

UserMemory::Range *UserMemory::heapRangeOf(uint32_t at) {
        for (unsigned i = 0; i < _rangeCount; ++i) {
                Range &r = _ranges[i];
                if (at >= address(r.base) && at < address(r.base) + r.mapped * Page) return &r;
        }
        return nullptr;
}

// Room for a block of @p bytes: the last range grown (in its reservation, or the reservation
// grown in place), else a new range.
Error UserMemory::growHeap(uint32_t bytes) {
        Range   &last = _ranges[_rangeCount - 1];
        uint32_t pages = pagesFor(bytes);
        // A growing heap grows by an eighth at least (fewer, longer runs from the heap).
        const uint32_t step = last.mapped / 8 < 16 ? last.mapped / 8 : 16;
        if (pages < step) pages = step;
        if (last.mapped + pages > last.reserved) {
                const uint32_t short_ = last.mapped + pages - last.reserved;
                uint8_t *const after = last.base + size_t(last.reserved) * Page;
                if (PsramWindow::takeRangeAt(after, short_ + HeadroomPages)) {
                        last.reserved += short_ + HeadroomPages;
                } else if (PsramWindow::takeRangeAt(after, short_)) {
                        last.reserved += short_;
                }
        }
        if (last.mapped + pages <= last.reserved) {
                // Mapped, not yet granted: blocks are as they are made (raiseGrant()).
                const uint32_t top = address(last.base) + last.mapped * Page;
                Error          err = map(last, pages, false);
                if (err.isError()) return err;
                err = _heap.addRange(top, pages * Page);
                if (err.isError()) unmapTail(last, pages);
                return err;
        }
        // A range of its own: a pair of entries.
        if (_rangeCount == MaxRanges || _space->freeEntries() < 2) return Error::NoMem;
        Range   &r = _ranges[_rangeCount];
        uint32_t reserve = pages + HeadroomPages;
        uint8_t *at = PsramWindow::takeRange(reserve, false);
        if (at == nullptr) {
                reserve = pages;
                at = PsramWindow::takeRange(reserve, false);
        }
        if (at == nullptr) return Error::NoMem;
        r.base = at;
        r.reserved = reserve;
        r.region = r.grantTop = reinterpret_cast<uintptr_t>(at);
        Error err = map(r, pages, false);
        if (err.isOk()) err = _heap.addRange(address(at), pages * Page);
        if (err.isError()) {
                release(r);
                return err;
        }
        ++_rangeCount;
        return Error::Ok;
}

// After a free: whole runs at a range's top, all free, go back to the PSRAM heap (one page kept
// free there, so a block freed and made again doesn't map and unmap each time).
void UserMemory::trim(const RangeHeap::Freed &f) {
        if (f.bytes == 0) return;
        Range *r = heapRangeOf(f.freeAt);
        if (r == nullptr) return;
        uint32_t free = f.freeBytes;
        for (;;) {
                const uint32_t top = address(r->base) + r->mapped * Page;
                if (f.freeAt + free != top || r->runs.size() == 0) return;
                const Run     &last = r->runs[r->runs.size() - 1];
                const uint32_t start = address(r->base) + last.page * Page;
                const uint32_t bytes = last.pages * Page;
                if (last.page < r->imagePages || start < f.freeAt) return;
                if (last.pages == 1 && free - bytes < Page) return;
                if (_heap.removeEnd(f.freeAt, bytes).isError()) return;
                // No longer granted (on both cores) before it is unmapped.
                if (r->granted && start <= r->region) {
                        _space->revoke(r->region);
                        r->granted = false;
                        r->grantTop = r->region;
                } else if (r->granted && r->grantTop > start) {
                        (void)_space->resize(r->region, start - r->region);
                        r->grantTop = start;
                }
                unmapTail(*r, last.pages);
                free -= bytes;
                if (free == 0) return;
        }
}

void *UserMemory::heapAlloc(uint32_t bytes, uint32_t align, uint32_t owner) {
        auto r = _heap.alloc(bytes, align, owner);
        if (r.error() == Error::NoMem) {
                const uint64_t need = uint64_t(bytes) + align;
                if (need > UINT32_MAX || growHeap(uint32_t(need)).isError()) return nullptr;
                r = _heap.alloc(bytes, align, owner);
        }
        if (r.isError()) return nullptr;
        Range *range = heapRangeOf(r.value());
        if (range == nullptr || raiseGrant(*range, uintptr_t(r.value()) + _heap.sizeOf(r.value(), owner)).isError()) {
                (void)_heap.free(r.value(), owner);
                return nullptr;
        }
        return reinterpret_cast<void *>(uintptr_t(r.value()));
}

// ---- Large blocks

void *UserMemory::allocLarge(uint32_t bytes, uint32_t owner) {
        Large *slot = nullptr;
        for (Large &l : _large)
                if (l.range.base == nullptr && slot == nullptr) slot = &l;
        if (slot == nullptr || _space->freeEntries() < 2 + KeepFree) return nullptr;
        const uint32_t pages = pagesFor(bytes);
        uint8_t       *at = PsramWindow::takeRange(pages, false);
        if (at == nullptr) return nullptr;
        Range &r = slot->range;
        r.base = at;
        r.reserved = pages;
        Error err = map(r, pages, true);
        if (err.isOk()) err = _space->grant(reinterpret_cast<uintptr_t>(at), size_t(pages) * Page, Rw, KeepFree);
        if (err.isError()) {
                release(r);
                return nullptr;
        }
        r.region = reinterpret_cast<uintptr_t>(at);
        slot->bytes = roundUp(bytes, RangeHeap::Unit);
        slot->owner = owner;
        return at;
}

UserMemory::Large *UserMemory::largeAt(const void *p) {
        for (Large &l : _large)
                if (l.range.base != nullptr && l.range.base == p) return &l;
        return nullptr;
}

void UserMemory::freeLarge(Large &l) {
        _space->revoke(l.range.region); // on both cores before its pages go
        release(l.range);
        l.bytes = 0;
        l.owner = 0;
        return;
}

// In place: whole runs off its end, or pages mapped after it where its addresses can grow.
bool UserMemory::resizeLarge(Large &l, uint32_t bytes) {
        Range         &r = l.range;
        const uint32_t pages = pagesFor(bytes);
        if (pages <= r.mapped) {
                uint32_t end = r.mapped;
                while (r.runs.size() > 1 && end - r.runs[r.runs.size() - 1].pages >= pages)
                        end -= r.runs[r.runs.size() - 1].pages;
                if (end < r.mapped) {
                        (void)_space->resize(r.region, size_t(end) * Page);
                        unmapTail(r, r.mapped - end);
                }
                l.bytes = roundUp(bytes, RangeHeap::Unit);
                return true;
        }
        const uint32_t extra = pages - r.mapped;
        if (r.mapped + extra > r.reserved) {
                if (!PsramWindow::takeRangeAt(r.base + size_t(r.reserved) * Page, r.mapped + extra - r.reserved))
                        return false;
                r.reserved = r.mapped + extra;
        }
        if (map(r, extra, true).isError()) return false;
        if (_space->resize(r.region, size_t(r.mapped) * Page).isError()) {
                unmapTail(r, extra);
                return false;
        }
        l.bytes = roundUp(bytes, RangeHeap::Unit);
        return true;
}

// ---- Internal RAM

void *UserMemory::allocInternal(uint32_t bytes, uint32_t align, uint32_t flags, uint32_t owner) {
        Internal *slot = nullptr;
        for (Internal &b : _internal)
                if (b.block == nullptr && slot == nullptr) slot = &b;
        if (slot == nullptr) return nullptr;
        const uint32_t size = uint32_t(UserMode::granules(bytes != 0 ? bytes : 1));
        if (align < PmpSet::Granule) align = PmpSet::Granule;
        const MemSpace space = (flags & BMDS_MEM_DMA) != 0 ? MemSpace::InternalDma : MemSpace::Internal;
        void          *block = Memory::allocAligned(space, size, align);
        if (block == nullptr) return nullptr;
        if (_space->grant(reinterpret_cast<uintptr_t>(block), size, Rw, KeepFree).isError()) {
                Memory::free(block);
                return nullptr;
        }
        std::memset(block, 0, size); // the kernel's, before
        slot->block = block;
        slot->bytes = size;
        slot->owner = owner;
        return block;
}

UserMemory::Internal *UserMemory::internalAt(const void *p) {
        for (Internal &b : _internal)
                if (b.block != nullptr && b.block == p) return &b;
        return nullptr;
}

void UserMemory::freeInternal(Internal &b) {
        _space->revoke(reinterpret_cast<uintptr_t>(b.block)); // on both cores before it is reused
        Memory::free(b.block);
        b = Internal();
        return;
}

bool UserMemory::isInternal(const void *p) {
        Mutex::Locker locker(_lock, WaitForever);
        return internalAt(p) != nullptr;
}

// ---- ProgramMemory

void *UserMemory::alloc(uint32_t bytes, uint32_t align, uint32_t flags, uint32_t owner) {
        if (align < RangeHeap::Unit) align = RangeHeap::Unit;
        if ((flags & BMDS_MEM_DMA) != 0) {
                // Whole cache lines: nothing else of the program's shares one with a DMA buffer.
                if (align < Memory::DmaAlign) align = uint32_t(Memory::DmaAlign);
                if (bytes > UINT32_MAX - Memory::DmaAlign) return nullptr;
                bytes = roundUp(bytes != 0 ? bytes : 1, uint32_t(Memory::DmaAlign));
        }
        Mutex::Locker locker(_lock, WaitForever);
        if ((flags & BMDS_MEM_FAST) != 0) {
                if (void *p = allocInternal(bytes, align, flags, owner); p != nullptr) return p;
        }
        if (bytes >= LargeBytes) {
                if (void *p = allocLarge(bytes, owner); p != nullptr) return p;
        }
        return heapAlloc(bytes, align, owner);
}

void UserMemory::freeLocked(void *p, uint32_t owner, uint32_t &bytes) {
        bytes = 0;
        if (Internal *b = internalAt(p); b != nullptr) {
                if (b->owner != owner) return;
                bytes = b->bytes;
                freeInternal(*b);
                return;
        }
        if (Large *l = largeAt(p); l != nullptr) {
                if (l->owner != owner) return;
                bytes = l->bytes;
                freeLarge(*l);
                return;
        }
        const RangeHeap::Freed f = _heap.free(address(p), owner);
        bytes = f.bytes;
        trim(f);
        return;
}

uint32_t UserMemory::free(void *p, uint32_t owner) {
        if (p == nullptr) return 0;
        Mutex::Locker locker(_lock, WaitForever);
        uint32_t      bytes = 0;
        freeLocked(p, owner, bytes);
        return bytes;
}

uint32_t UserMemory::blockSize(const void *p, uint32_t owner) {
        if (const Internal *b = internalAt(p); b != nullptr) return b->owner == owner ? b->bytes : 0;
        if (const Large *l = largeAt(p); l != nullptr) return l->owner == owner ? l->bytes : 0;
        return _heap.sizeOf(address(p), owner);
}

void *UserMemory::realloc(void *p, uint32_t bytes, uint32_t flags, uint32_t owner) {
        uint32_t old = 0;
        {
                Mutex::Locker locker(_lock, WaitForever);
                old = blockSize(p, owner);
                if (old == 0) return nullptr;
                if (Large *l = largeAt(p); l != nullptr && (flags & BMDS_MEM_FAST) == 0) {
                        if (resizeLarge(*l, bytes)) return p;
                } else if (internalAt(p) == nullptr && (flags & BMDS_MEM_FAST) == 0 &&
                           (bytes < LargeBytes || old >= bytes)) {
                        if (_heap.resize(address(p), bytes, owner).isOk()) {
                                Range *range = heapRangeOf(address(p));
                                if (range != nullptr &&
                                    raiseGrant(*range, uintptr_t(address(p)) + _heap.sizeOf(address(p), owner)).isOk())
                                        return p;
                                (void)_heap.resize(address(p), old, owner);
                        }
                }
        }
        void *moved = alloc(bytes, 0, flags, owner);
        if (moved == nullptr) return nullptr;
        std::memcpy(moved, p, old < bytes ? old : bytes);
        (void)free(p, owner);
        return moved;
}

uint32_t UserMemory::sizeOf(const void *p, uint32_t owner) {
        Mutex::Locker locker(_lock, WaitForever);
        return blockSize(p, owner);
}

bool UserMemory::contains(const void *p, size_t bytes, uint32_t owner) {
        const uint32_t at = address(p);
        if (bytes > UINT32_MAX || at > UINT32_MAX - bytes) return false;
        Mutex::Locker locker(_lock, WaitForever);
        for (const Internal &b : _internal) {
                const uint32_t start = address(b.block);
                if (b.block != nullptr && b.owner == owner && at >= start && at + bytes <= start + b.bytes) return true;
        }
        for (const Large &l : _large) {
                const uint32_t start = address(l.range.base);
                if (l.range.base != nullptr && l.owner == owner && at >= start && at + bytes <= start + l.bytes)
                        return true;
        }
        return _heap.contains(at, uint32_t(bytes), owner);
}

void UserMemory::freeOwner(uint32_t owner) {
        Mutex::Locker locker(_lock, WaitForever);
        for (Internal &b : _internal)
                if (b.block != nullptr && b.owner == owner) freeInternal(b);
        for (Large &l : _large)
                if (l.range.base != nullptr && l.owner == owner) freeLarge(l);
        _heap.freeOwner(owner);
        return;
}

UserMemory::Usage UserMemory::usage() {
        Mutex::Locker locker(_lock, WaitForever);
        Usage         u;
        auto          count = [&](const Range &r) {
                for (size_t i = 0; i < r.runs.size(); ++i) u.mappedPages += r.runs[i].pages;
                u.reservedPages += r.reserved;
        };
        for (unsigned i = 0; i < _rangeCount; ++i) count(_ranges[i]);
        for (const Large &l : _large) {
                if (l.range.base == nullptr) continue;
                count(l.range);
                ++u.large;
        }
        for (const Internal &b : _internal) {
                if (b.block == nullptr) continue;
                u.internalBytes += b.bytes;
                ++u.internal;
        }
        const RangeHeap::Stats s = _heap.stats();
        u.heapUsed = s.usedBytes;
        u.heapFree = s.freeBytes;
        u.ranges = uint8_t(_rangeCount);
        u.freeEntries = uint8_t(_space->freeEntries());
        return u;
}

BMDS_NAMESPACE_END
