/**
 * @file      zonebundle.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/zonebundle.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t Format = 1;
        constexpr uint32_t MaxEntries = 65536;
        constexpr uint32_t MaxEntryBytes = 136; // a name of up to 127 bytes, as later bundles might have
        constexpr uint32_t MinTzif = 44;        // a TZif header (musl refuses less)
        constexpr size_t   FooterWindow = 64;   // the footer's line, and the newline before it

        uint16_t u16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | p[1] << 8);
        }

        uint32_t u32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
        }

} // namespace

Result<ZoneBundle> ZoneBundle::open(Source &source, uint64_t fileSize) {
        if (fileSize < HeaderBytes || fileSize > UINT32_MAX) return makeError<ZoneBundle>(Error::CorruptData);
        uint8_t header[HeaderBytes];
        Error   err = source.readAt(0, WritableByteSpan(header, sizeof(header)));
        if (err.isError()) return makeError<ZoneBundle>(err);
        if (std::memcmp(header, "BMTZ", 4) != 0) return makeError<ZoneBundle>(Error::CorruptData);
        if (u16(header + 4) != Format) return makeError<ZoneBundle>(Error::NotSupported);
        ZoneBundle     b;
        const uint32_t headerBytes = u16(header + 6);
        b._count = u32(header + 16);
        b._indexOffset = u32(header + 20);
        b._entryBytes = u32(header + 24);
        b._fileSize = u32(header + 28);
        if (headerBytes < HeaderBytes || b._fileSize != fileSize || b._count > MaxEntries || b._entryBytes < 16 ||
            b._entryBytes > MaxEntryBytes || b._indexOffset < headerBytes || b._indexOffset > b._fileSize ||
            uint64_t(b._count) * b._entryBytes > b._fileSize - b._indexOffset)
                return makeError<ZoneBundle>(Error::CorruptData);
        b._dataStart = b._indexOffset + b._count * b._entryBytes;
        for (size_t i = 0; i < MaxVersion && header[8 + i] != 0; ++i) {
                const char c = static_cast<char>(header[8 + i]);
                if (c < '!' || c > '~') return makeError<ZoneBundle>(Error::CorruptData);
                b._version[i] = c;
        }
        return makeResult(b);
}

Error ZoneBundle::entry(Source &source, uint32_t index, Entry &out) const {
        if (index >= _count) return Error::OutOfRange;
        uint8_t     raw[MaxEntryBytes];
        const Error err = source.readAt(_indexOffset + index * _entryBytes, WritableByteSpan(raw, _entryBytes));
        if (err.isError()) return err;
        const size_t nameBytes = _entryBytes - 8;
        const size_t length = strnlen(reinterpret_cast<const char *>(raw), nameBytes);
        if (length == 0 || length == nameBytes || length > MaxName) return Error::CorruptData;
        std::memcpy(out.name, raw, length);
        out.name[length] = '\0';
        out.offset = u32(raw + nameBytes);
        out.length = u32(raw + nameBytes + 4);
        if (out.offset < _dataStart || out.length < MinTzif || out.offset > _fileSize ||
            out.length > _fileSize - out.offset)
                return Error::CorruptData;
        return Error();
}

Result<uint32_t> ZoneBundle::lowerBound(Source &source, StringView key) const {
        uint32_t low = 0;
        uint32_t high = _count;
        Entry    e;
        while (low < high) {
                const uint32_t mid = low + (high - low) / 2;
                const Error    err = entry(source, mid, e);
                if (err.isError()) return makeError<uint32_t>(err);
                if (e.nameView() < key) {
                        low = mid + 1;
                } else {
                        high = mid;
                }
        }
        return makeResult(low);
}

Error ZoneBundle::find(Source &source, StringView name, Entry &out) const {
        if (name.isEmpty() || name.size() > MaxName) return Error::NotExist;
        const Result<uint32_t> at = lowerBound(source, name);
        if (at.error().isError()) return at.error();
        if (at.value() == _count) return Error::NotExist;
        const Error err = entry(source, at.value(), out);
        if (err.isError()) return err;
        return out.nameView() == name ? Error() : Error::NotExist;
}

Result<TimeZone> ZoneBundle::rules(Source &source, const Entry &entry) {
        uint8_t version = 0;
        Error   err = source.readAt(entry.offset + 4, WritableByteSpan(&version, 1));
        if (err.isError()) return makeError<TimeZone>(err);
        if (version < '2') return makeError<TimeZone>(Error::NotSupported);
        // "...\nRULES\n" at the very end: the last line.
        char         tail[FooterWindow];
        const size_t n = entry.length < sizeof(tail) ? entry.length : sizeof(tail);
        err = source.readAt(entry.offset + entry.length - static_cast<uint32_t>(n),
                            WritableByteSpan(reinterpret_cast<uint8_t *>(tail), n));
        if (err.isError()) return makeError<TimeZone>(err);
        if (tail[n - 1] != '\n') return makeError<TimeZone>(Error::CorruptData);
        const StringView text(tail, n - 1);
        const size_t     start = text.lastIndexOf('\n');
        if (start == StringView::NPos) return makeError<TimeZone>(Error::CorruptData);
        const StringView spec = text.mid(start + 1);
        if (spec.isEmpty()) return makeError<TimeZone>(Error::NotSupported);
        const Result<TimeZone> zone = TimeZone::parse(spec);
        if (zone.error().isError()) return makeError<TimeZone>(Error::CorruptData);
        return zone;
}

BMDS_NAMESPACE_END
