/**
 * @file      romeditor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <algorithm>
#include <cstring>
#include <bmds/blake2s.h>
#include <bmds/memory.h>
#include <bmds/romeditor.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Sector = RomImage::SectorBytes;
        constexpr uint32_t Page = RomImage::PageBytes;
        constexpr uint32_t ChunkBytes = 4096;
        constexpr uint8_t  PageLog2 = 16;

        // A sector's users (RomEditor::_used).
        constexpr uint8_t UsedBooted = 0x01;
        constexpr uint8_t UsedCommitted = 0x02; // the committed tree's, and the header and log
        constexpr uint8_t UsedNew = 0x04;       // this change's
        constexpr uint8_t ToErase = 0x08;       // this change erases it before writing

        constexpr uint8_t NoHash[RomImage::HashBytes] = {};

        uint64_t alignUp(uint64_t v, uint64_t a) {
                return (v + a - 1) / a * a;
        }

        bool isZero(Span<const uint8_t> bytes) {
                for (uint8_t b : bytes)
                        if (b != 0) return false;
                return true;
        }

        // A memory block freed when it goes.
        struct Block {
                        uint8_t *data = nullptr;
                        explicit Block(MemSpace space, size_t bytes)
                            : data(static_cast<uint8_t *>(Memory::alloc(space, bytes))) {}
                        Block(const Block &) = delete;
                        Block &operator=(const Block &) = delete;
                        ~Block() { Memory::free(data); }
        };

} // namespace

Result<size_t> RomEditor::MemorySource::read(WritableByteSpan buf) {
        const size_t n = std::min(buf.size(), _bytes.size() - _at);
        std::memcpy(buf.data(), _bytes.data() + _at, n);
        _at += n;
        return makeResult(n);
}

RomEditor::RomEditor(RomFlash &flash, const RomImage *booted, Span<const uint8_t> firmware)
    : _flash(flash), _booted(booted), _firmware(firmware) {}

RomEditor::~RomEditor() {
        Memory::free(_used);
}

Error RomEditor::load() {
        auto [image, err] = RomImage::open(_flash.bytes(), _firmware);
        _loaded = err.isOk();
        if (err.isError()) return Error::CorruptData;
        _committed = image;
        return Error::Ok;
}

bool RomEditor::hasPending() const {
        return _loaded && _booted != nullptr && _committed.version().offset != _booted->version().offset;
}

void RomEditor::setProgress(Progress progress, void *context) {
        _progress = progress;
        _progressContext = context;
        return;
}

void RomEditor::tell(uint64_t done, uint64_t total) {
        if (_progress != nullptr) _progress(_progressContext, done, total);
        return;
}

void RomEditor::mark(uint64_t offset, uint64_t bytes) {
        if (bytes == 0) return;
        const uint64_t last = (offset + bytes - 1) / Sector;
        for (uint64_t s = offset / Sector; s <= last && s < _usedCount; ++s) _used[s] |= UsedCommitted;
        return;
}

Error RomEditor::buildUsage() {
        const uint32_t count = static_cast<uint32_t>(_flash.bytes().size() / Sector);
        if (count != _usedCount) {
                Memory::free(_used);
                _used = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, count + 1));
                _usedCount = _used != nullptr ? count : 0;
                if (_used == nullptr) return Error::NoMem;
        }
        std::memset(_used, 0, _usedCount);
        mark(0, _committed.dataOffset()); // the header and the log
        for (uint32_t i = 0; i < _committed.count(); ++i) {
                const RomImage::Entry e = _committed.entry(i);
                mark(e.offset, e.data.size());
        }
        if (_booted != nullptr) {
                for (uint32_t i = 0; i < _booted->count(); ++i) {
                        const RomImage::Entry e = _booted->entry(i);
                        if (e.data.isEmpty()) continue;
                        const uint64_t last = (uint64_t(e.offset) + e.data.size() - 1) / Sector;
                        for (uint64_t s = e.offset / Sector; s <= last && s < _usedCount; ++s) _used[s] |= UsedBooted;
                }
        }
        return Error::Ok;
}

Error RomEditor::allocate(List<Planned> &planned, bool dryRun) {
        (void)dryRun;
        // The pages first, from the top down: each file its own, at its alignment.
        uint64_t needed = 0;
        bool     fits = true;
        uint64_t runBytes = 0;
        for (Planned &p : planned) {
                if (p.source == nullptr) continue;
                if (p.entry.alignLog2 < PageLog2) {
                        runBytes = alignUp(runBytes, uint64_t(1) << p.entry.alignLog2);
                        p.entry.offset = static_cast<uint32_t>(runBytes); // within the run, for now
                        runBytes += p.entry.size;
                        continue;
                }
                const uint64_t unit = uint64_t(1) << p.entry.alignLog2;
                const uint64_t bytes = alignUp(p.entry.size, Page);
                needed += bytes;
                if (bytes == 0) {
                        p.entry.offset = static_cast<uint32_t>(alignUp(_committed.dataOffset(), unit)); // takes nothing
                        continue;
                }
                const uint64_t top = _flash.bytes().size();
                bool           placed = false;
                if (top >= bytes) {
                        for (uint64_t at = (top - bytes) / unit * unit; at >= _committed.dataOffset(); at -= unit) {
                                bool free = true;
                                for (uint64_t s = at / Sector; s < (at + bytes) / Sector && free; ++s) free = isFree(s);
                                if (free) {
                                        for (uint64_t s = at / Sector; s < (at + bytes) / Sector; ++s)
                                                _used[s] |= UsedNew;
                                        p.entry.offset = static_cast<uint32_t>(at);
                                        placed = true;
                                        break;
                                }
                                if (at < unit) break;
                        }
                }
                if (!placed) fits = false;
        }
        // The rest together: the lowest run of free sectors that holds them.
        const uint64_t runSectors = alignUp(runBytes, Sector) / Sector;
        needed += runSectors * Sector;
        uint64_t base = _committed.dataOffset();
        if (runSectors > 0) {
                bool     placed = false;
                uint64_t run = 0;
                for (uint64_t s = _committed.dataOffset() / Sector; s < _usedCount; ++s) {
                        run = isFree(s) ? run + 1 : 0;
                        if (run == runSectors) {
                                base = (s + 1 - run) * Sector;
                                placed = true;
                                break;
                        }
                }
                if (!placed) fits = false;
        }
        if (!fits) {
                // Why: what is free now, the largest run, and what will be after the next boot.
                _shortfall = Shortfall();
                _shortfall.needed = needed;
                uint64_t run = 0;
                for (uint32_t s = 0; s < _usedCount; ++s) {
                        const bool now = (_used[s] & (UsedBooted | UsedCommitted)) == 0;
                        if (now) _shortfall.freeNow += Sector;
                        if ((_used[s] & UsedCommitted) == 0) _shortfall.freeAfterBoot += Sector;
                        run = now ? run + 1 : 0;
                        if (run * Sector > _shortfall.largestNow) _shortfall.largestNow = run * Sector;
                }
                return Error::NoSpace;
        }
        for (Planned &p : planned)
                if (p.source != nullptr && p.entry.alignLog2 < PageLog2) p.entry.offset += static_cast<uint32_t>(base);
        for (uint64_t s = base / Sector; s < base / Sector + runSectors; ++s) _used[s] |= UsedNew;
        return Error::Ok;
}

Error RomEditor::eraseRun(uint64_t offset, uint64_t bytes) {
        if (bytes == 0) return Error::Ok;
        const uint64_t from = offset / Sector * Sector;
        const uint64_t to = alignUp(offset + bytes, Sector);
        return _flash.erase(static_cast<uint32_t>(from), static_cast<uint32_t>(to - from));
}

Error RomEditor::writeFiles(List<Planned> &planned, Report &report) {
        uint64_t total = 0;
        for (const Planned &p : planned) {
                if (p.source == nullptr) continue;
                total += p.entry.size;
                if (p.entry.size == 0) continue;
                const uint64_t last = (uint64_t(p.entry.offset) + p.entry.size - 1) / Sector;
                for (uint64_t s = p.entry.offset / Sector; s <= last && s < _usedCount; ++s) _used[s] |= ToErase;
        }
        if (total == 0) {
                for (Planned &p : planned)
                        if (p.source != nullptr) {
                                Blake2s h;
                                h.final(p.hash);
                                ++report.written;
                        }
                return Error::Ok;
        }
        Block chunk(MemSpace::Internal, ChunkBytes);
        if (chunk.data == nullptr) return Error::NoMem;
        // Erased first: every sector the new files cover, once, in runs (the chip erases 64 KB
        // blocks at once where a run covers them).
        Error err = Error::Ok;
        for (uint32_t s = 0; s < _usedCount && err.isOk();) {
                if ((_used[s] & ToErase) == 0) {
                        ++s;
                        continue;
                }
                uint32_t end = s;
                while (end < _usedCount && (_used[end] & ToErase) != 0) _used[end++] &= static_cast<uint8_t>(~ToErase);
                err = eraseRun(uint64_t(s) * Sector, uint64_t(end - s) * Sector);
                s = end;
        }
        if (err.isError()) return err;
        uint64_t done = 0;
        tell(0, total);
        for (Planned &p : planned) {
                if (p.source == nullptr) continue;
                Blake2s  h;
                uint64_t at = p.entry.offset;
                uint64_t left = p.entry.size;
                while (left > 0) {
                        auto [n, rerr] =
                                p.source->read(WritableByteSpan(chunk.data, std::min<uint64_t>(left, ChunkBytes)));
                        if (rerr.isError()) return rerr;
                        if (n == 0) return Error::IoError; // shorter than it said
                        err = _flash.write(static_cast<uint32_t>(at), ByteSpan(chunk.data, n));
                        if (err.isError()) return err;
                        h.update(chunk.data, n);
                        at += n;
                        left -= n;
                        done += n;
                        tell(done, total);
                }
                h.final(p.hash);
                // Read back from the chip: the same bytes.
                Blake2s back;
                at = p.entry.offset;
                left = p.entry.size;
                while (left > 0) {
                        const size_t n = static_cast<size_t>(std::min<uint64_t>(left, ChunkBytes));
                        err = _flash.read(static_cast<uint32_t>(at), WritableByteSpan(chunk.data, n));
                        if (err.isError()) return err;
                        back.update(chunk.data, n);
                        at += n;
                        left -= n;
                }
                uint8_t digest[RomImage::HashBytes];
                back.final(digest);
                if (std::memcmp(digest, p.hash, sizeof(digest)) != 0) return Error::IoError;
                ++report.written;
                report.bytesWritten += p.entry.size;
        }
        return Error::Ok;
}

Error RomEditor::commit(List<Planned> &planned, Span<const uint8_t> firmware, Report &report) {
        // The new tree, by path; new files' hashes where they are now.
        std::sort(planned.begin(), planned.end(),
                  [](const Planned &a, const Planned &b) { return a.entry.path < b.entry.path; });
        List<RomImage::NewEntry> entries;
        if (entries.reserve(planned.size()).isError()) return Error::NoMem;
        for (Planned &p : planned) {
                if (p.source != nullptr) p.entry.hash = Span<const uint8_t>(p.hash, RomImage::HashBytes);
                (void)entries.append(p.entry);
        }
        const Span<const RomImage::NewEntry> all(entries.data(), entries.size());
        const size_t                         bytes = RomImage::versionBytes(all);

        // The log: whole versions' sectors; those of the booted, the committed and any newer
        // (passed over, for another firmware) are kept; the rest may be erased for this one.
        const uint32_t logFirst = _committed.logOffset() / Sector;
        const uint32_t logCount = _committed.logBytes() / Sector;
        Block          state(MemSpace::Psram, logCount + 1); // 1 erased, 2 kept
        if (state.data == nullptr) return Error::NoMem;
        std::memset(state.data, 0, logCount);
        uint32_t sequence = 0;
        for (uint32_t at = _committed.logOffset(); at < _committed.logOffset() + _committed.logBytes();) {
                RomImage::LogRecord r;
                const uint32_t      next = _committed.logRecord(at, r);
                using Kind = RomImage::LogRecord::Kind;
                if (r.kind == Kind::Erased) state.data[at / Sector - logFirst] = 1;
                if (r.kind == Kind::Version || r.kind == Kind::Wrong) {
                        sequence = std::max(sequence, r.sequence);
                        const bool kept = r.kind == Kind::Version &&
                                          (r.offset == _committed.version().offset ||
                                           (_booted != nullptr && r.offset == _booted->version().offset) ||
                                           r.sequence > _committed.version().sequence);
                        if (kept)
                                for (uint32_t s = at; s < next; s += Sector) state.data[s / Sector - logFirst] = 2;
                }
                at = next;
        }
        if (sequence == UINT32_MAX) return Error::Full;
        Block version(MemSpace::Psram, bytes);
        if (version.data == nullptr) return Error::NoMem;
        auto [written, werr] =
                RomImage::writeVersion(all, sequence + 1, _now, firmware, WritableByteSpan(version.data, bytes));
        if (werr.isError()) return werr;
        // Where it goes: erased sectors if there are enough together, else any not kept.
        const uint32_t needed = static_cast<uint32_t>(alignUp(bytes, Sector) / Sector);
        uint32_t       first = UINT32_MAX;
        for (int pass = 0; pass < 2 && first == UINT32_MAX; ++pass) {
                uint32_t run = 0;
                for (uint32_t s = 0; s < logCount; ++s) {
                        run = (pass == 0 ? state.data[s] == 1 : state.data[s] != 2) ? run + 1 : 0;
                        if (run == needed) {
                                first = s + 1 - run;
                                break;
                        }
                }
        }
        if (first == UINT32_MAX) return Error::Full; // the log has no room (rom.md §12e)
        const uint32_t offset = (logFirst + first) * Sector;
        for (uint32_t s = first; s < first + needed; ++s) {
                if (state.data[s] == 1) continue;
                const Error err = _flash.erase((logFirst + s) * Sector, Sector);
                if (err.isError()) return err;
        }
        Error err = _flash.write(offset, ByteSpan(version.data, written));
        if (err.isError()) return err;
        // Read back: the same bytes, then the tree it makes is the committed one.
        Block back(MemSpace::Psram, written);
        if (back.data == nullptr) return Error::NoMem;
        err = _flash.read(offset, WritableByteSpan(back.data, written));
        if (err.isError()) return err;
        if (std::memcmp(back.data, version.data, written) != 0) return Error::IoError;
        report.sequence = sequence + 1;
        return load();
}

void RomEditor::keep(List<Planned> &planned, uint32_t index) {
        const RomImage::Entry e = _committed.entry(index);
        Planned               p;
        p.entry.path = e.path;
        p.entry.flags = e.flags;
        p.entry.alignLog2 = e.alignLog2;
        p.entry.offset = e.offset;
        p.entry.size = static_cast<uint32_t>(e.data.size());
        p.entry.modified = e.modified;
        p.entry.hash = e.hash;
        (void)planned.append(p);
        return;
}

Error RomEditor::install(Span<const Add> adds, Report &report) {
        report = Report();
        _failedPath = StringView();
        if (!_loaded) return Error::InvalidState;
        if (adds.isEmpty()) return Error::Invalid;
        // The adds by path, each once.
        List<uint32_t> order;
        if (order.reserve(adds.size()).isError()) return Error::NoMem;
        for (uint32_t i = 0; i < adds.size(); ++i) (void)order.append(i);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return adds[a].path < adds[b].path; });
        for (size_t i = 0; i < order.size(); ++i) {
                const Add &a = adds[order[i]];
                if (a.source == nullptr || a.source->size() > UINT32_MAX || a.alignLog2 > RomImage::MaxAlignLog2 ||
                    (i > 0 && adds[order[i - 1]].path == a.path)) {
                        _failedPath = a.path;
                        return Error::Invalid;
                }
        }
        // The committed tree with the adds merged in, by path.
        List<Planned> planned;
        if (planned.reserve(_committed.count() + adds.size()).isError()) return Error::NoMem;
        uint32_t c = 0;
        size_t   k = 0;
        while (c < _committed.count() || k < order.size()) {
                const StringView have = c < _committed.count() ? _committed.entry(c).path : StringView();
                if (k >= order.size() || (c < _committed.count() && have < adds[order[k]].path)) {
                        keep(planned, c++);
                        continue;
                }
                const Add &a = adds[order[k++]];
                if (c < _committed.count() && have == a.path) ++c; // replaced
                Planned p;
                p.entry.path = a.path;
                p.entry.flags = a.flags;
                p.entry.alignLog2 = a.alignLog2;
                p.entry.size = static_cast<uint32_t>(a.source->size());
                p.entry.modified = a.modified;
                p.entry.hash = Span<const uint8_t>(NoHash, sizeof(NoHash));
                p.source = a.source;
                (void)planned.append(p);
        }
        // Checked as the version will be (paths, a file and a directory at once) before
        // anything is written.
        {
                List<RomImage::NewEntry> entries;
                if (entries.reserve(planned.size()).isError()) return Error::NoMem;
                for (const Planned &p : planned) {
                        RomImage::NewEntry e = p.entry;
                        if (p.source != nullptr) e.offset = 0;
                        (void)entries.append(e);
                }
                const Span<const RomImage::NewEntry> all(entries.data(), entries.size());
                Block                                scratch(MemSpace::Psram, RomImage::versionBytes(all));
                if (scratch.data == nullptr) return Error::NoMem;
                const auto checked = RomImage::writeVersion(
                        all, 1, _now, {}, WritableByteSpan(scratch.data, RomImage::versionBytes(all)));
                if (checked.error().isError()) {
                        // Which add: the first whose path is refused, or that a neighbour's contradicts.
                        for (size_t i = 0; i < order.size() && _failedPath.isEmpty(); ++i) {
                                const StringView path = adds[order[i]].path;
                                bool             unused = false;
                                const auto       there = _committed.find(path, unused);
                                if (path.isEmpty() || path.startsWith('/') || path.endsWith('/') ||
                                    path.contains("//") || (there.error().isOk() && there.value().directory))
                                        _failedPath = path;
                                for (size_t k2 = 0; k2 < path.size() && _failedPath.isEmpty(); ++k2) {
                                        if (path[k2] != '/') continue;
                                        const auto up = _committed.find(path.left(k2), unused);
                                        if (up.error().isOk() && !up.value().directory) _failedPath = path;
                                }
                        }
                        if (_failedPath.isEmpty()) _failedPath = adds[order[0]].path;
                        return Error::Invalid;
                }
        }
        Error err = buildUsage();
        if (err.isOk()) err = allocate(planned, false);
        if (err.isOk()) err = writeFiles(planned, report);
        if (err.isOk()) err = commit(planned, {}, report);
        return err;
}

Error RomEditor::remove(Span<const StringView> paths, bool recursive, Report &report) {
        report = Report();
        _failedPath = StringView();
        if (!_loaded) return Error::InvalidState;
        if (paths.isEmpty()) return Error::Invalid;
        Block gone(MemSpace::Psram, _committed.count() + 1);
        if (gone.data == nullptr) return Error::NoMem;
        std::memset(gone.data, 0, _committed.count());
        for (const StringView path : paths) {
                bool       noPath = false;
                const auto found = _committed.find(path, noPath);
                if (found.error().isError()) {
                        _failedPath = path;
                        return Error::NotExist;
                }
                if (found.value().directory && !recursive) {
                        _failedPath = path;
                        return Error::IsDir;
                }
                if (found.value().directory) {
                        for (uint32_t i = found.value().first; i < found.value().end; ++i) gone.data[i] = 1;
                } else {
                        gone.data[found.value().index] = 1;
                }
        }
        List<Planned> planned;
        if (planned.reserve(_committed.count()).isError()) return Error::NoMem;
        for (uint32_t i = 0; i < _committed.count(); ++i) {
                if (gone.data[i] != 0) {
                        ++report.removed;
                } else {
                        keep(planned, i);
                }
        }
        if (report.removed == 0) return Error::Ok; // an empty directory isn't there: never here
        // The user's change: for any firmware (only an update's tree is for its own, §12f).
        return commit(planned, {}, report);
}

Error RomEditor::discard(Report &report) {
        report = Report();
        if (!_loaded) return Error::InvalidState;
        if (!hasPending()) return Error::Ok;
        // A version of the booted tree's files, which are where they were: kept from the
        // booted tree (as the committed one is in keep()).
        List<Planned> planned;
        if (planned.reserve(_booted->count()).isError()) return Error::NoMem;
        for (uint32_t i = 0; i < _booted->count(); ++i) {
                const RomImage::Entry e = _booted->entry(i);
                Planned               p;
                p.entry.path = e.path;
                p.entry.flags = e.flags;
                p.entry.alignLog2 = e.alignLog2;
                p.entry.offset = e.offset;
                p.entry.size = static_cast<uint32_t>(e.data.size());
                p.entry.modified = e.modified;
                p.entry.hash = e.hash;
                (void)planned.append(p);
        }
        uint8_t firmware[RomImage::HashBytes];
        std::memcpy(firmware, _booted->version().firmware.data(), sizeof(firmware));
        return commit(planned,
                      isZero(Span<const uint8_t>(firmware, sizeof(firmware)))
                              ? Span<const uint8_t>()
                              : Span<const uint8_t>(firmware, sizeof(firmware)),
                      report);
}

Error RomEditor::compact(Report &report, bool &more) {
        report = Report();
        more = false;
        if (!_loaded) return Error::InvalidState;
        Error err = buildUsage();
        if (err.isError()) return err;
        // From the top down, each file (but those on pages, and empty ones) into the lowest run of
        // free sectors below it that holds it: holes filled, the free space gathered at the top,
        // and only what fills a hole moved. Never up.
        const uint32_t count = _committed.count();
        List<uint32_t> order;
        if (order.reserve(count).isError()) return Error::NoMem;
        for (uint32_t i = 0; i < count; ++i) {
                const RomImage::Entry e = _committed.entry(i);
                if (e.alignLog2 < PageLog2 && !e.data.isEmpty()) (void)order.append(i);
        }
        std::sort(order.begin(), order.end(),
                  [&](uint32_t a, uint32_t b) { return _committed.entry(a).offset > _committed.entry(b).offset; });
        List<Planned>      planned;
        List<MemorySource> sources;
        if (planned.reserve(count).isError() || sources.reserve(count).isError()) return Error::NoMem;
        for (uint32_t k = 0; k < count; ++k) keep(planned, k);
        for (uint32_t index : order) {
                const RomImage::Entry e = _committed.entry(index);
                const uint64_t        need = alignUp(e.data.size(), Sector) / Sector;
                const uint64_t        below = e.offset / Sector; // the sectors under its first
                uint64_t              run = 0;
                uint64_t              at = UINT64_MAX;
                for (uint64_t s = _committed.dataOffset() / Sector; s < below; ++s) {
                        run = isFree(static_cast<uint32_t>(s)) ? run + 1 : 0;
                        if (run == need) {
                                at = s + 1 - run;
                                break;
                        }
                }
                if (at == UINT64_MAX) continue;
                for (uint64_t s = at; s < at + need; ++s) _used[s] |= UsedNew;
                (void)sources.append(MemorySource(e.data));
                planned[index].source = &sources[sources.size() - 1];
                planned[index].entry.offset = static_cast<uint32_t>(at * Sector);
        }
        if (sources.size() == 0) return Error::Ok; // nothing below anything that holds it
        err = writeFiles(planned, report);
        report.moved = report.written;
        report.written = 0;
        report.bytesWritten = 0;
        if (err.isOk()) err = commit(planned, {}, report); // for any firmware, as the user's changes are
        // Their old places free at the next boot (in recovery: at once), for the files above them.
        more = err.isOk();
        return err;
}

Error RomEditor::plan(const RomImage &package, List<Planned> &planned, Displaced displaced, void *context,
                      Report &report) {
        report = Report();
        if (planned.reserve(_committed.count() + package.count()).isError()) return Error::NoMem;
        // A user's file the package's takes the place of: at its path, a directory of the
        // package's there, or a file of the package's along its path.
        auto taken = [&](StringView path) {
                bool       unused = false;
                const auto there = package.find(path, unused);
                if (there.error().isOk()) return true;
                for (size_t k = 0; k < path.size(); ++k) {
                        if (path[k] != '/') continue;
                        const auto up = package.find(path.left(k), unused);
                        if (up.error().isOk() && !up.value().directory) return true;
                }
                return false;
        };
        uint32_t c = 0;
        uint32_t k = 0;
        while (c < _committed.count() || k < package.count()) {
                const RomImage::Entry have = c < _committed.count() ? _committed.entry(c) : RomImage::Entry();
                const RomImage::Entry want = k < package.count() ? package.entry(k) : RomImage::Entry();
                const bool haveOnly = k >= package.count() || (c < _committed.count() && have.path < want.path);
                const bool wantOnly = c >= _committed.count() || (k < package.count() && want.path < have.path);
                if (haveOnly) {
                        if ((have.flags & RomImage::User) == 0) {
                                ++report.removed; // the system's, which the update no longer has
                        } else if (taken(have.path)) {
                                ++report.displaced;
                                if (displaced != nullptr) displaced(context, have.path);
                        } else {
                                keep(planned, c);
                        }
                        ++c;
                        continue;
                }
                if (!wantOnly) {
                        // At one path in both: the same system file, where its alignment allows, stays.
                        const bool user = (have.flags & RomImage::User) != 0;
                        if (!user && have.data.size() == want.data.size() && have.flags == want.flags &&
                            have.offset % (uint32_t(1) << want.alignLog2) == 0 &&
                            std::memcmp(have.hash.data(), want.hash.data(), RomImage::HashBytes) == 0) {
                                keep(planned, c);
                                planned[planned.size() - 1].entry.alignLog2 = want.alignLog2;
                                ++report.kept;
                                ++c;
                                ++k;
                                continue;
                        }
                        if (user) {
                                ++report.displaced;
                                if (displaced != nullptr) displaced(context, have.path);
                        }
                        ++c;
                }
                Planned p;
                p.entry.path = want.path;
                p.entry.flags = static_cast<uint8_t>(want.flags & ~RomImage::User);
                p.entry.alignLog2 = want.alignLog2;
                p.entry.size = static_cast<uint32_t>(want.data.size());
                p.entry.modified = want.modified;
                p.entry.hash = Span<const uint8_t>(NoHash, sizeof(NoHash));
                (void)planned.append(p);
                ++k;
        }
        return Error::Ok;
}

Error RomEditor::checkApply(const RomImage &package, Displaced displaced, void *context) {
        if (!_loaded) return Error::InvalidState;
        List<Planned> planned;
        Report        report;
        Error         err = plan(package, planned, displaced, context, report);
        // Sources only mark what is to be written (nothing is, in a check).
        MemorySource none{ByteSpan()};
        for (Planned &p : planned)
                if (p.entry.hash.data() == NoHash) p.source = &none;
        if (err.isOk()) err = buildUsage();
        if (err.isOk()) err = allocate(planned, true);
        return err;
}

Error RomEditor::apply(const RomImage &package, Span<const uint8_t> firmware, Displaced displaced, void *context,
                       Report &report) {
        if (!_loaded) return Error::InvalidState;
        List<Planned> planned;
        Error         err = plan(package, planned, displaced, context, report);
        if (err.isError()) return err;
        // The package's bytes for those written.
        List<MemorySource> sources;
        if (sources.reserve(package.count()).isError()) return Error::NoMem;
        for (Planned &p : planned) {
                if (p.entry.hash.data() != NoHash) continue;
                bool       unused = false;
                const auto found = package.find(p.entry.path, unused);
                if (found.error().isError()) return Error::CorruptData;
                (void)sources.append(MemorySource(package.entry(found.value().index).data));
                p.source = &sources[sources.size() - 1];
        }
        Report done = report;
        err = buildUsage();
        if (err.isOk()) err = allocate(planned, false);
        if (err.isOk()) err = writeFiles(planned, done);
        if (err.isOk()) err = commit(planned, firmware, done);
        report = done;
        return err;
}

BMDS_NAMESPACE_END
