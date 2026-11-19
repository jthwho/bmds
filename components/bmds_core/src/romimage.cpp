/**
 * @file      romimage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/blake2s.h>
#include <bmds/pngdecoder.h>
#include <bmds/romimage.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t Format = 2;
        constexpr uint8_t  KnownFlags = RomImage::System | RomImage::User;
        // The times a Timestamp holds (its nanoseconds in an int64_t: 1677 to 2262).
        constexpr int64_t MaxSeconds = INT64_MAX / Timestamp::NsPerSecond - 1;

        bool isTime(int64_t seconds) {
                return seconds >= -MaxSeconds && seconds <= MaxSeconds;
        }

        uint16_t u16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | p[1] << 8);
        }

        uint32_t u32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
        }

        void put16(uint8_t *p, uint32_t v) {
                p[0] = static_cast<uint8_t>(v);
                p[1] = static_cast<uint8_t>(v >> 8);
                return;
        }

        void put32(uint8_t *p, uint32_t v) {
                for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
                return;
        }

        void put64(uint8_t *p, int64_t v) {
                put32(p, static_cast<uint32_t>(static_cast<uint64_t>(v)));
                put32(p + 4, static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
                return;
        }

        int64_t i64(const uint8_t *p) {
                return static_cast<int64_t>(u32(p) | static_cast<uint64_t>(u32(p + 4)) << 32);
        }

        bool allZero(const uint8_t *p, size_t n) {
                for (size_t i = 0; i < n; ++i)
                        if (p[i] != 0) return false;
                return true;
        }

        // A path an entry may have: 1 to MaxPath bytes, no NUL, no leading, trailing or
        // doubled '/', no "." or ".." part.
        bool isPath(StringView path) {
                if (path.isEmpty() || path.size() > RomImage::MaxPath) return false;
                size_t start = 0;
                for (size_t i = 0; i <= path.size(); ++i) {
                        if (i < path.size() && path[i] == '\0') return false;
                        if (i < path.size() && path[i] != '/') continue;
                        const StringView part = path.mid(start, i - start);
                        if (part.isEmpty() || part == "." || part == "..") return false;
                        start = i + 1;
                }
                return true;
        }

        // A tree version's header, read (its fields as the format has them).
        struct VersionHeader {
                        uint32_t recordBytes = 0;
                        uint32_t sequence = 0;
                        uint32_t count = 0;
                        uint32_t pathsAt = 0;
                        uint32_t pathsBytes = 0;
                        uint32_t entryBytes = 0;
        };

        // The version starting at @p at (a sector of the log ending at @p logEnd), if its header
        // is sane and its CRC holds.
        bool readVersion(const uint8_t *base, uint32_t at, uint32_t logEnd, VersionHeader &v) {
                const uint8_t *h = base + at;
                if (logEnd - at < RomImage::VersionHeaderBytes + 4 || std::memcmp(h, "BMRT", 4) != 0) return false;
                v.entryBytes = u16(h + 6);
                if (u16(h + 4) != RomImage::VersionHeaderBytes || v.entryBytes < RomImage::MinEntryBytes ||
                    v.entryBytes > RomImage::MaxEntryBytes || v.entryBytes % 8 != 0)
                        return false;
                v.recordBytes = u32(h + 8);
                v.sequence = u32(h + 12);
                v.count = u32(h + 16);
                v.pathsAt = u32(h + 20);
                v.pathsBytes = u32(h + 24);
                const uint32_t flags = u32(h + 28);
                if (v.recordBytes < RomImage::VersionHeaderBytes + 4 || v.recordBytes > logEnd - at || flags != 0 ||
                    !allZero(h + 72, 8) || !isTime(i64(h + 32)))
                        return false;
                const uint32_t body = v.recordBytes - 4; // the CRC's place
                if (uint64_t(v.count) * v.entryBytes > body - RomImage::VersionHeaderBytes ||
                    v.pathsAt < RomImage::VersionHeaderBytes + v.count * v.entryBytes || v.pathsAt > body ||
                    v.pathsBytes > body - v.pathsAt)
                        return false;
                return Png::crc32(ByteSpan(h, body)) == u32(h + body);
        }

} // namespace

bool RomImage::looksLike(ByteSpan bytes) {
        return bytes.size() >= HeaderBytes && std::memcmp(bytes.data(), "BMRO", 4) == 0;
}

Result<RomImage> RomImage::open(ByteSpan bytes, Span<const uint8_t> firmware) {
        if (!looksLike(bytes)) return makeError<RomImage>(Error::CorruptData);
        const uint8_t *h = bytes.data();
        if (u16(h + 4) != Format) return makeError<RomImage>(Error::NotSupported);
        if (Png::crc32(ByteSpan(h, HeaderBytes - 4)) != u32(h + HeaderBytes - 4))
                return makeError<RomImage>(Error::CorruptData);
        RomImage r;
        r._logOffset = u32(h + 8);
        r._logBytes = u32(h + 12);
        r._dataOffset = u32(h + 16);
        const uint64_t logEnd = uint64_t(r._logOffset) + r._logBytes;
        if (u16(h + 6) != HeaderBytes || u32(h + 20) != 0 || !allZero(h + 24, 36) || r._logOffset < SectorBytes ||
            r._logOffset % SectorBytes != 0 || r._logBytes == 0 || r._logBytes % SectorBytes != 0 ||
            r._dataOffset % SectorBytes != 0 || r._dataOffset < logEnd || r._dataOffset > bytes.size())
                return makeError<RomImage>(Error::CorruptData);
        r._partition = bytes;

        // The newest version that is whole (its CRC) and good (its entries), among those for
        // this firmware (none named, or @p firmware) if any is, else among all. Each sector may
        // start one; a good one's other sectors are its own. A newer one with bad entries (a
        // writer's bug, not a cut) leaves the next newest.
        const bool choose = firmware.size() == HashBytes;
        for (int pass = choose ? 0 : 1; pass < 2; ++pass) {
                uint64_t below = uint64_t(UINT32_MAX) + 1;
                for (;;) {
                        bool          found = false;
                        uint32_t      bestAt = 0;
                        VersionHeader best;
                        for (uint32_t at = r._logOffset; at < logEnd;) {
                                VersionHeader v;
                                if (!readVersion(h, at, static_cast<uint32_t>(logEnd), v)) {
                                        at += SectorBytes;
                                        continue;
                                }
                                const uint8_t *needs = h + at + 40;
                                const bool     fits = allZero(needs, HashBytes) ||
                                                  (choose && std::memcmp(needs, firmware.data(), HashBytes) == 0);
                                if ((pass == 1 || fits) && v.sequence < below &&
                                    (!found || v.sequence > best.sequence)) {
                                        found = true;
                                        best = v;
                                        bestAt = at;
                                }
                                at += (v.recordBytes + SectorBytes - 1) / SectorBytes * SectorBytes;
                        }
                        if (!found) break;
                        r.useVersion(bestAt);
                        if (r.checkEntries().isOk()) return makeResult(r);
                        below = best.sequence;
                }
        }
        return makeError<RomImage>(Error::CorruptData);
}

void RomImage::useVersion(uint32_t at) {
        // Its header was read and its CRC checked (readVersion()).
        const uint8_t *h = _partition.data() + at;
        _count = u32(h + 16);
        _entryBytes = u16(h + 6);
        _entries = at + VersionHeaderBytes;
        _paths = at + u32(h + 20);
        _pathsBytes = u32(h + 24);
        _version.offset = at;
        _version.bytes = u32(h + 8);
        _version.sequence = u32(h + 12);
        _version.written = Timestamp::fromSeconds(i64(h + 32));
        _version.firmware = _partition.mid(at + 40, HashBytes);
        return;
}

uint32_t RomImage::logRecord(uint32_t at, LogRecord &record) const {
        const uint32_t logEnd = _logOffset + _logBytes;
        record = LogRecord();
        if (_partition.isEmpty() || at < _logOffset || at >= logEnd || (at - _logOffset) % SectorBytes != 0)
                return logEnd;
        record.offset = at;
        record.bytes = SectorBytes;
        const uint8_t *p = _partition.data() + at;
        VersionHeader  v;
        if (readVersion(_partition.data(), at, logEnd, v)) {
                RomImage tree = *this;
                tree.useVersion(at);
                record.kind = tree.checkEntries().isOk() ? LogRecord::Kind::Version : LogRecord::Kind::Wrong;
                record.bytes = v.recordBytes;
                record.sequence = v.sequence;
                record.count = v.count;
                record.written = tree._version.written;
                record.firmware = tree._version.firmware;
                return at + (v.recordBytes + SectorBytes - 1) / SectorBytes * SectorBytes;
        }
        if (std::memcmp(p, "BMRT", 4) == 0) {
                // A version's start, but not whole: cut short, or damaged. What its header says.
                record.kind = LogRecord::Kind::Damaged;
                record.sequence = u32(p + 12);
                return at + SectorBytes;
        }
        bool erased = true;
        for (uint32_t i = 0; i < SectorBytes && erased; ++i) erased = p[i] == 0xff;
        record.kind = erased ? LogRecord::Kind::Erased : LogRecord::Kind::Other;
        return at + SectorBytes;
}

Error RomImage::checkEntries() const {
        const uint64_t partitionBytes = _partition.size();
        for (uint32_t i = 0; i < _count; ++i) {
                const uint8_t *e = entryAt(i);
                const uint32_t pathAt = u32(e);
                const uint32_t pathLength = u16(e + 4);
                const uint8_t  alignLog2 = e[6];
                const uint8_t  flags = e[7];
                const uint32_t offset = u32(e + 8);
                const uint32_t size = u32(e + 12);
                if (pathAt > _pathsBytes || pathLength > _pathsBytes - pathAt || (flags & ~KnownFlags) != 0 ||
                    alignLog2 > MaxAlignLog2 || !allZero(e + 61, 3) || !isTime(i64(e + 16)) ||
                    (_entryBytes >= 72 && !allZero(e + 64, 8))) // reserved where the entry has them
                        return Error::CorruptData;
                // Stored as it is: no other size. (A method not known here is fine: §12c.)
                if (e[60] == None && u32(e + 56) != 0) return Error::CorruptData;
                if (offset < _dataOffset || offset > partitionBytes || size > partitionBytes - offset ||
                    offset % (uint32_t(1) << alignLog2) != 0)
                        return Error::CorruptData;
                const StringView path = pathOf(i);
                if (!isPath(path)) return Error::CorruptData;
                if (i > 0 && !(pathOf(i - 1) < path)) return Error::CorruptData;
        }
        // No path both a file and a directory ("a" and "a/b").
        for (uint32_t i = 0; i < _count; ++i)
                if (isDirectory(pathOf(i))) return Error::CorruptData;
        return Error::Ok;
}

StringView RomImage::pathOf(uint32_t index) const {
        // In bounds (open() checked it, or checks it before relying on it).
        const uint8_t *e = entryAt(index);
        const uint32_t at = u32(e);
        const uint32_t length = u16(e + 4);
        if (at > _pathsBytes || length > _pathsBytes - at) return StringView();
        return StringView(reinterpret_cast<const char *>(_partition.data()) + _paths + at, length);
}

uint32_t RomImage::lowerBound(StringView key) const {
        uint32_t low = 0;
        uint32_t high = _count;
        while (low < high) {
                const uint32_t mid = low + (high - low) / 2;
                if (pathOf(mid) < key) {
                        low = mid + 1;
                } else {
                        high = mid;
                }
        }
        return low;
}

RomImage::Entry RomImage::entry(uint32_t index) const {
        Entry e;
        if (index >= _count) return e;
        const uint8_t *p = entryAt(index);
        e.path = pathOf(index);
        const size_t slash = e.path.lastIndexOf('/');
        e.name = slash == StringView::NPos ? e.path : e.path.mid(slash + 1);
        e.alignLog2 = p[6];
        e.flags = p[7];
        e.offset = u32(p + 8);
        e.data = _partition.mid(e.offset, u32(p + 12));
        e.modified = Timestamp::fromSeconds(i64(p + 16));
        e.hash = _partition.mid(_entries + index * _entryBytes + 24, HashBytes);
        e.compression = p[60];
        e.realSize = u32(p + 56);
        return e;
}

bool RomImage::isDirectory(StringView path) const {
        // A path under it starts with @p path and a '/'.
        char key[MaxPath + 2];
        if (path.size() > MaxPath) return false;
        std::memcpy(key, path.data(), path.size());
        key[path.size()] = '/';
        const StringView under(key, path.size() + 1);
        const uint32_t   at = lowerBound(under);
        return at < _count && pathOf(at).startsWith(under);
}

Result<RomImage::Node> RomImage::find(StringView path, bool &noPath) const {
        noPath = false;
        // The path normalized as it is walked: each part along the way must be a directory.
        char   built[MaxPath + 2];
        size_t length = 0;
        size_t i = 0;
        while (i < path.size()) {
                size_t end = path.indexOf('/', i);
                if (end == StringView::NPos) end = path.size();
                const StringView part = path.mid(i, end - i);
                i = end + 1;
                if (part.isEmpty()) continue;
                size_t after = end;
                while (after < path.size() && path[after] == '/') ++after;
                const bool   more = after < path.size();
                const size_t grown = length + (length > 0 ? 1 : 0) + part.size();
                if (grown > MaxPath) {
                        // Longer than any path: not there, and nothing under it.
                        noPath = more;
                        return makeError<Node>(Error::NotExist);
                }
                if (length > 0) built[length++] = '/';
                std::memcpy(built + length, part.data(), part.size());
                length = grown;
                if (more && !isDirectory(StringView(built, length))) {
                        noPath = true;
                        return makeError<Node>(Error::NotExist);
                }
        }
        Node node;
        if (length == 0) {
                node.directory = true;
                node.end = _count;
                return makeResult(node);
        }
        const StringView full(built, length);
        const uint32_t   at = lowerBound(full);
        if (at < _count && pathOf(at) == full) {
                node.index = at;
                return makeResult(node);
        }
        built[length] = '/';
        const StringView under(built, length + 1);
        node.first = lowerBound(under);
        if (node.first >= _count || !pathOf(node.first).startsWith(under)) return makeError<Node>(Error::NotExist);
        built[length] = '0'; // the byte after '/': past every path under it
        node.end = lowerBound(StringView(built, length + 1));
        node.directory = true;
        node.prefixBytes = static_cast<uint32_t>(length + 1);
        return makeResult(node);
}

uint32_t RomImage::nextChild(const Node &dir, uint32_t at, Child &child) const {
        if (!dir.directory || at >= dir.end || at >= _count) return NoEntry;
        const StringView path = pathOf(at);
        const StringView rest = path.mid(dir.prefixBytes);
        const size_t     slash = rest.indexOf('/');
        child = Child();
        if (slash == StringView::NPos) {
                child.name = rest;
                child.node.index = at;
                return at + 1;
        }
        // A directory: named once, its run skipped.
        child.name = rest.left(slash);
        const size_t prefix = dir.prefixBytes + slash;
        char         key[MaxPath + 2];
        std::memcpy(key, path.data(), prefix);
        key[prefix] = '0'; // the byte after '/'
        child.node.directory = true;
        child.node.first = at;
        child.node.end = lowerBound(StringView(key, prefix + 1));
        child.node.prefixBytes = static_cast<uint32_t>(prefix + 1);
        return child.node.end;
}

Timestamp RomImage::newest(uint32_t first, uint32_t end) const {
        if (first >= end || end > _count) return _version.written;
        int64_t seconds = INT64_MIN;
        for (uint32_t i = first; i < end; ++i) {
                const int64_t t = i64(entryAt(i) + 16);
                if (t > seconds) seconds = t;
        }
        return Timestamp::fromSeconds(seconds);
}

uint64_t RomImage::filesBytes() const {
        uint64_t total = 0;
        for (uint32_t i = 0; i < _count; ++i) total += u32(entryAt(i) + 12);
        return total;
}

Error RomImage::verifyFile(uint32_t index) const {
        if (index >= _count) return Error::Invalid;
        const Entry e = entry(index);
        Blake2s     h;
        h.update(e.data.data(), e.data.size());
        uint8_t digest[Blake2s::DigestBytes];
        h.final(digest);
        return std::memcmp(digest, e.hash.data(), HashBytes) == 0 ? Error::Ok : Error::CorruptData;
}

Error RomImage::verify() const {
        for (uint32_t i = 0; i < _count; ++i) {
                const Error err = verifyFile(i);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

Error RomImage::writeHeader(uint32_t logOffset, uint32_t logBytes, uint32_t dataOffset, WritableByteSpan out) {
        if (out.size() < HeaderBytes) return Error::BufferTooSmall;
        if (logOffset < SectorBytes || logOffset % SectorBytes != 0 || logBytes == 0 || logBytes % SectorBytes != 0 ||
            dataOffset % SectorBytes != 0 || dataOffset < uint64_t(logOffset) + logBytes)
                return Error::Invalid;
        uint8_t *h = out.data();
        std::memset(h, 0, HeaderBytes);
        std::memcpy(h, "BMRO", 4);
        put16(h + 4, Format);
        put16(h + 6, HeaderBytes);
        put32(h + 8, logOffset);
        put32(h + 12, logBytes);
        put32(h + 16, dataOffset);
        put32(h + HeaderBytes - 4, Png::crc32(ByteSpan(h, HeaderBytes - 4)));
        return Error::Ok;
}

size_t RomImage::versionBytes(Span<const NewEntry> entries) {
        size_t bytes = VersionHeaderBytes + 4;
        for (const NewEntry &e : entries) bytes += EntryBytes + e.path.size();
        return bytes;
}

Result<size_t> RomImage::writeVersion(Span<const NewEntry> entries, uint32_t sequence, Timestamp written,
                                      Span<const uint8_t> firmware, WritableByteSpan out) {
        if (!firmware.isEmpty() && firmware.size() != HashBytes) return makeError<size_t>(Error::Invalid);
        const size_t bytes = versionBytes(entries);
        if (bytes > UINT32_MAX) return makeError<size_t>(Error::Invalid);
        if (out.size() < bytes) return makeError<size_t>(Error::BufferTooSmall);
        // Each entry as open() would check it, and the order; then no path both a file and a
        // directory (all of a file's would-be children follow it).
        for (size_t i = 0; i < entries.size(); ++i) {
                const NewEntry &e = entries[i];
                if (!isPath(e.path) || (e.flags & ~KnownFlags) != 0 || e.alignLog2 > MaxAlignLog2 ||
                    (e.compression == None && e.realSize != 0) ||
                    e.hash.size() != HashBytes || e.offset % (uint32_t(1) << e.alignLog2) != 0 ||
                    e.size > UINT32_MAX - e.offset)
                        return makeError<size_t>(Error::Invalid);
                if (i > 0 && !(entries[i - 1].path < e.path)) return makeError<size_t>(Error::Invalid);
        }
        for (size_t i = 0; i + 1 < entries.size(); ++i) {
                const StringView path = entries[i].path;
                for (size_t k = i + 1; k < entries.size(); ++k) {
                        const StringView next = entries[k].path;
                        if (!next.startsWith(path)) break; // past every path starting with it
                        if (next.size() > path.size() && next[path.size()] == '/')
                                return makeError<size_t>(Error::Invalid);
                }
        }

        uint8_t       *h = out.data();
        const uint32_t count = static_cast<uint32_t>(entries.size());
        const uint32_t pathsAt = VersionHeaderBytes + count * EntryBytes;
        uint32_t       pathsBytes = 0;
        std::memset(h, 0, bytes);
        for (uint32_t i = 0; i < count; ++i) {
                const NewEntry &e = entries[i];
                uint8_t        *p = h + VersionHeaderBytes + i * EntryBytes;
                put32(p, pathsBytes);
                put16(p + 4, static_cast<uint32_t>(e.path.size()));
                p[6] = e.alignLog2;
                p[7] = e.flags;
                put32(p + 8, e.offset);
                put32(p + 12, e.size);
                put64(p + 16, e.modified.seconds());
                std::memcpy(p + 24, e.hash.data(), HashBytes);
                put32(p + 56, e.realSize);
                p[60] = e.compression;
                std::memcpy(h + pathsAt + pathsBytes, e.path.data(), e.path.size());
                pathsBytes += static_cast<uint32_t>(e.path.size());
        }
        std::memcpy(h, "BMRT", 4);
        put16(h + 4, VersionHeaderBytes);
        put16(h + 6, EntryBytes);
        put32(h + 8, static_cast<uint32_t>(bytes));
        put32(h + 12, sequence);
        put32(h + 16, count);
        put32(h + 20, pathsAt);
        put32(h + 24, pathsBytes);
        put64(h + 32, written.seconds());
        if (!firmware.isEmpty()) std::memcpy(h + 40, firmware.data(), HashBytes);
        put32(h + bytes - 4, Png::crc32(ByteSpan(h, bytes - 4)));
        return makeResult(bytes);
}

BMDS_NAMESPACE_END
