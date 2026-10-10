/**
 * @file      iconpack.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/iconpack.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t Format = 1;
        constexpr uint16_t MaxSide = 256;

        uint16_t u16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | p[1] << 8);
        }

        uint32_t u32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
        }

} // namespace

Result<IconPack> IconPack::open(ByteSpan bytes) {
        if (reinterpret_cast<uintptr_t>(bytes.data()) % 4 != 0) return makeError<IconPack>(Error::Invalid);
        if (bytes.size() < HeaderBytes || std::memcmp(bytes.data(), "BMIC", 4) != 0)
                return makeError<IconPack>(Error::CorruptData);
        const uint8_t *h = bytes.data();
        if (u16(h + 4) != Format) return makeError<IconPack>(Error::NotSupported);
        const uint32_t headerBytes = u16(h + 6);
        const uint32_t fileBytes = u32(h + 8);
        IconPack       p;
        p._count = u32(h + 12);
        p._table = u32(h + 24);
        p._index = u32(h + 28);
        p._names = u32(h + 32);
        const uint32_t namesBytes = u32(h + 36);
        if (headerBytes != HeaderBytes || fileBytes != bytes.size() || p._count > MaxIcons || p._table < headerBytes ||
            p._table > fileBytes || uint64_t(p._count) * RowBytes > fileBytes - p._table || p._index < headerBytes ||
            p._index > fileBytes || uint64_t(p._count) * 2 > fileBytes - p._index || p._names < headerBytes ||
            p._names > fileBytes || namesBytes > fileBytes - p._names)
                return makeError<IconPack>(Error::CorruptData);
        // The sides: ascending, then zeros.
        for (size_t s = 0; s < MaxSizes; ++s) {
                const uint16_t side = u16(h + 16 + 2 * s);
                if (side > MaxSide || (side != 0 && s > 0 && (p._sides[s - 1] == 0 || side <= p._sides[s - 1])))
                        return makeError<IconPack>(Error::CorruptData);
                p._sides[s] = side;
        }
        p._bytes = bytes;
        for (uint32_t id = 0; id < p._count; ++id) {
                const uint8_t *r = p.row(id);
                const uint32_t at = u32(r);
                const uint32_t length = u16(r + 4);
                if (length == 0 || length > MaxName || at > namesBytes || length > namesBytes - at)
                        return makeError<IconPack>(Error::CorruptData);
                for (size_t s = 0; s < MaxSizes; ++s) {
                        const uint32_t pixels = u32(r + 8 + 4 * s);
                        if (pixels == 0) continue;
                        const uint32_t need = uint32_t(p._sides[s]) * p._sides[s] * 4;
                        if (p._sides[s] == 0 || pixels % 4 != 0 || pixels < headerBytes || pixels > fileBytes ||
                            need > fileBytes - pixels)
                                return makeError<IconPack>(Error::CorruptData);
                }
        }
        // The index: every id once, by name (strictly ascending, so no id twice).
        StringView previous;
        for (uint32_t i = 0; i < p._count; ++i) {
                const uint32_t id = u16(h + p._index + 2 * i);
                if (id >= p._count) return makeError<IconPack>(Error::CorruptData);
                const StringView n = p.name(id);
                if (i > 0 && !(previous < n)) return makeError<IconPack>(Error::CorruptData);
                previous = n;
        }
        return makeResult(p);
}

StringView IconPack::name(uint32_t id) const {
        if (id >= _count) return StringView();
        const uint8_t *r = row(id);
        return StringView(reinterpret_cast<const char *>(_bytes.data()) + _names + u32(r), u16(r + 4));
}

const uint32_t *IconPack::pixels(uint32_t id, uint16_t side) const {
        if (id >= _count || side == 0) return nullptr;
        for (size_t s = 0; s < MaxSizes; ++s) {
                if (_sides[s] != side) continue;
                const uint32_t at = u32(row(id) + 8 + 4 * s);
                return at == 0 ? nullptr : reinterpret_cast<const uint32_t *>(_bytes.data() + at);
        }
        return nullptr;
}

uint32_t IconPack::idOf(StringView name) const {
        uint32_t low = 0;
        uint32_t high = _count;
        while (low < high) {
                const uint32_t   mid = low + (high - low) / 2;
                const uint32_t   id = u16(_bytes.data() + _index + 2 * mid);
                const StringView n = this->name(id);
                if (n == name) return id;
                if (n < name) {
                        low = mid + 1;
                } else {
                        high = mid;
                }
        }
        return NoIcon;
}

BMDS_NAMESPACE_END
