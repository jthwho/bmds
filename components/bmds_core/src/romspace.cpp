/**
 * @file      romspace.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <algorithm>
#include <cstring>
#include <bmds/memory.h>
#include <bmds/romspace.h>

BMDS_NAMESPACE_BEGIN

RomSpace::~RomSpace() {
        release();
}

void RomSpace::release() {
        Memory::free(_regions);
        Memory::free(_sectors);
        _regions = nullptr;
        _sectors = nullptr;
        _regionCount = 0;
        _sectorCount = 0;
        _summary = Summary();
        return;
}

Error RomSpace::build(const RomImage &booted, const RomImage *pending) {
        release();
        const uint64_t partitionBytes = booted.partition().size();
        const uint32_t total = booted.count() + (pending != nullptr ? pending->count() : 0);
        const size_t   most = 4 + 2 * size_t(total) + 1; // header, log, reserved, files, the free between
        _regions = static_cast<Region *>(Memory::alloc(MemSpace::Psram, most * sizeof(Region)));
        _sectorCount = static_cast<uint32_t>(partitionBytes / RomImage::SectorBytes);
        _sectors = static_cast<Sector *>(Memory::alloc(MemSpace::Psram, size_t(_sectorCount) * sizeof(Sector) + 1));
        if (_regions == nullptr || _sectors == nullptr) {
                release();
                return Error::NoMem;
        }
        std::memset(_sectors, 0, size_t(_sectorCount) * sizeof(Sector));
        _summary.partitionBytes = partitionBytes;
        _summary.reservedBytes = booted.dataOffset();
        _summary.filesBytes = booted.filesBytes();

        // The files of both trees, by place; one in both (same place, size, path and hash) once.
        Region *files = _regions + 4 + total + 1; // the end of the array holds them while sorting
        size_t  n = 0;
        auto    add = [&](const RomImage &image, uint8_t tree) {
                for (uint32_t i = 0; i < image.count(); ++i) {
                        const RomImage::Entry e = image.entry(i);
                        Region               &r = files[n++];
                        r = Region();
                        r.kind = Kind::File;
                        r.offset = e.offset;
                        r.bytes = static_cast<uint32_t>(e.data.size());
                        r.trees = tree;
                        r.image = &image;
                        r.index = i;
                }
        };
        add(booted, Booted);
        if (pending != nullptr) add(*pending, Pending);
        std::sort(files, files + n, [](const Region &a, const Region &b) {
                if (a.offset != b.offset) return a.offset < b.offset;
                if (a.bytes != b.bytes) return a.bytes < b.bytes;
                const StringView pa = a.image->entry(a.index).path;
                const StringView pb = b.image->entry(b.index).path;
                if (pa != pb) return pa < pb;
                return a.trees < b.trees; // the booted tree's first
        });
        size_t kept = 0;
        for (size_t i = 0; i < n; ++i) {
                if (kept > 0) {
                        Region &last = files[kept - 1];
                        if (last.offset == files[i].offset && last.bytes == files[i].bytes &&
                            (last.trees & files[i].trees) == 0 &&
                            last.image->entry(last.index).path == files[i].image->entry(files[i].index).path &&
                            std::memcmp(last.image->entry(last.index).hash.data(),
                                        files[i].image->entry(files[i].index).hash.data(), RomImage::HashBytes) == 0) {
                                last.trees |= files[i].trees;
                                continue;
                        }
                }
                files[kept++] = files[i];
        }

        // The regions in order: the header and log, then files and the free between them.
        size_t count = 0;
        auto   push = [&](Kind kind, uint64_t offset, uint64_t bytes) {
                Region &r = _regions[count++];
                r = Region();
                r.kind = kind;
                r.offset = static_cast<uint32_t>(offset);
                r.bytes = static_cast<uint32_t>(bytes);
        };
        const uint64_t logEnd = uint64_t(booted.logOffset()) + booted.logBytes();
        push(Kind::Header, 0, booted.logOffset());
        push(Kind::Log, booted.logOffset(), booted.logBytes());
        if (booted.dataOffset() > logEnd) push(Kind::Reserved, logEnd, booted.dataOffset() - logEnd);
        uint64_t cursor = booted.dataOffset();
        uint64_t freeBytes = 0;
        // Where each tree's files have reached: a file overlaps one of its own tree's (the trees
        // may share bytes under two names, which isn't an overlap).
        uint64_t reached[2] = {cursor, cursor};
        for (size_t i = 0; i < kept; ++i) {
                Region        &f = files[i];
                const uint64_t end = uint64_t(f.offset) + f.bytes;
                if (f.offset > cursor) {
                        push(Kind::Free, cursor, f.offset - cursor);
                        freeBytes += f.offset - cursor;
                }
                f.overlaps = f.bytes > 0 && (((f.trees & Booted) != 0 && f.offset < reached[0]) ||
                                             ((f.trees & Pending) != 0 && f.offset < reached[1]));
                if (f.overlaps) ++_summary.overlaps;
                if ((f.trees & Booted) != 0 && end > reached[0]) reached[0] = end;
                if ((f.trees & Pending) != 0 && end > reached[1]) reached[1] = end;
                _regions[count++] = f;
                ++_summary.files;
                if (end > cursor) cursor = end;
        }
        if (partitionBytes > cursor) {
                push(Kind::Free, cursor, partitionBytes - cursor);
                freeBytes += partitionBytes - cursor;
        }
        _regionCount = count;

        // The sectors: the header and log used for good; each file's sectors by its trees.
        const uint32_t reserved = (booted.dataOffset() + RomImage::SectorBytes - 1) / RomImage::SectorBytes;
        for (uint32_t s = 0; s < reserved && s < _sectorCount; ++s) _sectors[s].flags = UsedNow | UsedAfter;
        const uint8_t after = pending != nullptr ? Pending : Booted;
        for (size_t i = 0; i < _regionCount; ++i) {
                const Region &r = _regions[i];
                if (r.kind != Kind::File || r.bytes == 0) continue;
                const uint32_t first = r.offset / RomImage::SectorBytes;
                const uint32_t last = static_cast<uint32_t>((uint64_t(r.offset) + r.bytes - 1) / RomImage::SectorBytes);
                for (uint32_t s = first; s <= last && s < _sectorCount; ++s) {
                        _sectors[s].flags |= UsedNow;
                        if ((r.trees & after) != 0) _sectors[s].flags |= UsedAfter;
                        if (_sectors[s].files < 255) ++_sectors[s].files;
                }
        }

        // The totals, in whole sectors and pages.
        constexpr uint32_t PageSectors = RomImage::PageBytes / RomImage::SectorBytes;
        uint64_t           run = 0;
        uint64_t           longest = 0;
        uint64_t           pageRun = 0;
        uint64_t           longestPages = 0;
        uint64_t           freeNow = 0;
        uint64_t           freeAfter = 0;
        for (uint32_t s = 0; s < _sectorCount; ++s) {
                const bool free = (_sectors[s].flags & UsedNow) == 0;
                if (free) ++freeNow;
                if ((_sectors[s].flags & UsedAfter) == 0) ++freeAfter;
                run = free ? run + 1 : 0;
                if (run > longest) longest = run;
                if (s % PageSectors == PageSectors - 1) {
                        bool page = true;
                        for (uint32_t k = s + 1 - PageSectors; k <= s && page; ++k)
                                page = (_sectors[k].flags & UsedNow) == 0;
                        pageRun = page ? pageRun + 1 : 0;
                        if (pageRun > longestPages) longestPages = pageRun;
                }
        }
        _summary.freeNow = freeNow * RomImage::SectorBytes;
        _summary.freeAfterBoot = freeAfter * RomImage::SectorBytes;
        _summary.largestNow = longest * RomImage::SectorBytes;
        _summary.largestPagesNow = longestPages * RomImage::PageBytes;
        _summary.holeBytes = _summary.freeNow - _summary.largestNow;
        _summary.paddingBytes = freeBytes > _summary.freeNow ? freeBytes - _summary.freeNow : 0;
        return Error::Ok;
}

BMDS_NAMESPACE_END
