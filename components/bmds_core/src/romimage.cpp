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

        constexpr uint16_t Format = 1;
        constexpr uint8_t  KindFile = 0;
        constexpr uint8_t  KindDirectory = 1;
        constexpr uint8_t  KnownFlags = RomImage::System | RomImage::Mappable;

        uint16_t u16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | p[1] << 8);
        }

        uint32_t u32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
        }

        int64_t i64(const uint8_t *p) {
                return static_cast<int64_t>(u32(p) | static_cast<uint64_t>(u32(p + 4)) << 32);
        }

        // A name an entry may have: no '/', no NUL, not "." or "..".
        bool isName(StringView name) {
                if (name.isEmpty() || name.size() > RomImage::MaxName || name == "." || name == "..") return false;
                for (char c : name)
                        if (c == '/' || c == '\0') return false;
                return true;
        }

} // namespace

bool RomImage::looksLike(ByteSpan bytes) {
        return bytes.size() >= HeaderBytes && std::memcmp(bytes.data(), "BMRO", 4) == 0;
}

Result<RomImage> RomImage::open(ByteSpan bytes) {
        if (!looksLike(bytes)) return makeError<RomImage>(Error::CorruptData);
        const uint8_t *h = bytes.data();
        if (u16(h + 4) != Format) return makeError<RomImage>(Error::NotSupported);
        if (Png::crc32(ByteSpan(h, HeaderBytes - 4)) != u32(h + HeaderBytes - 4))
                return makeError<RomImage>(Error::CorruptData);
        const uint32_t headerBytes = u16(h + 6);
        const uint32_t imageBytes = u32(h + 8);
        RomImage       r;
        r._count = u32(h + 12);
        r._entries = u32(h + 16);
        r._names = u32(h + 20);
        const uint32_t namesBytes = u32(h + 24);
        if (headerBytes != HeaderBytes || imageBytes < HeaderBytes || imageBytes > bytes.size() || r._count == 0 ||
            r._count > MaxEntries || r._entries < headerBytes || r._entries > imageBytes ||
            uint64_t(r._count) * EntryBytes > imageBytes - r._entries || r._names < headerBytes ||
            r._names > imageBytes || namesBytes > imageBytes - r._names)
                return makeError<RomImage>(Error::CorruptData);
        r._bytes = bytes.mid(0, imageBytes);
        r._buildTime = Timestamp::fromSeconds(i64(h + 64));
        // Every entry, once: its name and bytes inside the image, and the directories a tree.
        uint32_t next = 1; // the next directory's first child
        for (uint32_t i = 0; i < r._count; ++i) {
                const uint8_t *e = r.entryAt(i);
                const uint32_t nameAt = u32(e);
                const uint32_t nameLength = u16(e + 4);
                const uint8_t  kind = e[6];
                const uint8_t  flags = e[7];
                const uint32_t offset = u32(e + 8);
                const uint32_t size = u32(e + 12);
                if (nameAt > namesBytes || nameLength > namesBytes - nameAt || (flags & ~KnownFlags) != 0)
                        return makeError<RomImage>(Error::CorruptData);
                const StringView name(reinterpret_cast<const char *>(h) + r._names + nameAt, nameLength);
                if (i == 0 ? (nameLength != 0 || kind != KindDirectory) : !isName(name))
                        return makeError<RomImage>(Error::CorruptData);
                if (kind == KindDirectory) {
                        if (flags != 0 || offset != next || size > r._count - next)
                                return makeError<RomImage>(Error::CorruptData);
                        next += size;
                        StringView previous;
                        for (uint32_t c = offset; c < offset + size; ++c) {
                                const StringView child = r.nameOf(r.entryAt(c));
                                if (c > offset && !(previous < child)) return makeError<RomImage>(Error::CorruptData);
                                previous = child;
                        }
                } else if (kind == KindFile) {
                        if (offset < headerBytes || offset > imageBytes || size > imageBytes - offset ||
                            ((flags & Mappable) != 0 && offset % MapAlign != 0))
                                return makeError<RomImage>(Error::CorruptData);
                } else {
                        return makeError<RomImage>(Error::CorruptData);
                }
                // A directory's children come after it (so the walk above can't loop).
                if (i > 0 && i >= next) return makeError<RomImage>(Error::CorruptData);
        }
        if (next != r._count) return makeError<RomImage>(Error::CorruptData);
        return makeResult(r);
}

StringView RomImage::nameOf(const uint8_t *e) const {
        // Checked by open() (a child's name is checked before it is compared: it may come later
        // in the walk, so its bounds are checked here too).
        const uint32_t at = u32(e);
        const uint32_t length = u16(e + 4);
        const uint32_t limit = static_cast<uint32_t>(_bytes.size()) - _names;
        if (at > limit || length > limit - at) return StringView();
        return StringView(reinterpret_cast<const char *>(_bytes.data()) + _names + at, length);
}

RomImage::Entry RomImage::entry(uint32_t index) const {
        Entry e;
        if (index >= _count) return e;
        const uint8_t *p = entryAt(index);
        e.name = nameOf(p);
        e.directory = p[6] == KindDirectory;
        e.flags = p[7];
        if (e.directory) {
                e.first = u32(p + 8);
                e.count = u32(p + 12);
        } else {
                e.data = _bytes.mid(u32(p + 8), u32(p + 12));
        }
        return e;
}

uint32_t RomImage::findChild(uint32_t dir, StringView name) const {
        const Entry d = entry(dir);
        if (!d.directory) return NoEntry;
        uint32_t low = d.first;
        uint32_t high = d.first + d.count;
        while (low < high) {
                const uint32_t   mid = low + (high - low) / 2;
                const StringView n = nameOf(entryAt(mid));
                if (n == name) return mid;
                if (n < name) {
                        low = mid + 1;
                } else {
                        high = mid;
                }
        }
        return NoEntry;
}

Result<uint32_t> RomImage::find(StringView path, bool &noPath) const {
        noPath = false;
        if (_count == 0) return makeError<uint32_t>(Error::NotExist);
        uint32_t at = 0;
        size_t   i = 0;
        while (i < path.size()) {
                size_t end = path.indexOf('/', i);
                if (end == StringView::NPos) end = path.size();
                const StringView part = path.mid(i, end - i);
                i = end + 1;
                if (part.isEmpty()) continue;
                // A component after this one: this one must be a directory that is there.
                size_t after = end;
                while (after < path.size() && path[after] == '/') ++after;
                const bool     more = after < path.size();
                const uint32_t child = findChild(at, part);
                if (child == NoEntry) {
                        noPath = more || !entry(at).directory;
                        return makeError<uint32_t>(Error::NotExist);
                }
                at = child;
                if (more && !entry(at).directory) {
                        noPath = true;
                        return makeError<uint32_t>(Error::NotExist);
                }
        }
        return makeResult(at);
}

Error RomImage::verify() const {
        if (_bytes.size() < HeaderBytes) return Error::CorruptData;
        Blake2s h;
        h.update(_bytes.data() + HeaderBytes, _bytes.size() - HeaderBytes);
        uint8_t digest[Blake2s::DigestBytes];
        h.final(digest);
        return std::memcmp(digest, _bytes.data() + 32, HashBytes) == 0 ? Error::Ok : Error::CorruptData;
}

BMDS_NAMESPACE_END
