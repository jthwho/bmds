/**
 * @file      romdata.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the ROM's data files the kernel reads in place (devplan/rom.md §12f: they
 * are untrusted now that the ROM can change): the input's first byte picks the reader, the rest is
 * the file. The icon pack (IconPack: every id's name and pixels at every size inside the pack, and
 * each name found again), the time zone bundle (ZoneBundle: every entry, found by its name, its
 * rules), the gamepad database (PadMapping: a line found by the next four bytes' vendor and product,
 * and each line parsed). Nothing may fault, read outside the file or overflow.
 *
 * Seeds: the ROM's own files with the selector byte in front (tests/fuzz/README.md).
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <bmds/iconpack.h>
#include <bmds/padmapping.h>
#include <bmds/zonebundle.h>

using bmds::ByteSpan;

namespace {

        struct MemorySource final : bmds::ZoneBundle::Source {
                        const uint8_t *data;
                        size_t         size;

                        MemorySource(const uint8_t *d, size_t n) : data(d), size(n) {}

                        bmds::Error readAt(uint32_t offset, bmds::WritableByteSpan buf) override {
                                if (offset > size || buf.size() > size - offset) return bmds::Error::CorruptData;
                                std::memcpy(buf.data(), data + offset, buf.size());
                                return bmds::Error::Ok;
                        }
        };

        void iconPack(const uint8_t *data, size_t size) {
                // In memory aligned as the mapped ROM is (its pixels are read where they are).
                std::vector<uint32_t> words((size + 3) / 4 + 1);
                std::memcpy(words.data(), data, size);
                const uint8_t *start = reinterpret_cast<const uint8_t *>(words.data());
                auto [pack, err] = bmds::IconPack::open(ByteSpan(start, size));
                if (err.isError()) return;
                for (uint32_t id = 0; id < pack.count(); ++id) {
                        const bmds::StringView name = pack.name(id);
                        if (name.data() != nullptr &&
                            (reinterpret_cast<const uint8_t *>(name.data()) < start ||
                             reinterpret_cast<const uint8_t *>(name.data()) + name.size() > start + size))
                                std::abort();
                        for (size_t k = 0; k < bmds::IconPack::MaxSizes; ++k) {
                                const uint16_t  side = pack.side(k);
                                const uint32_t *p = side > 0 ? pack.pixels(id, side) : nullptr;
                                if (p == nullptr) continue;
                                const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
                                if (b < start || b + size_t(side) * side * 4 > start + size) std::abort();
                        }
                        const uint32_t found = pack.idOf(name);
                        if (found != bmds::IconPack::NoIcon && found >= pack.count()) std::abort();
                }
        }

        void zoneBundle(const uint8_t *data, size_t size) {
                MemorySource source(data, size);
                auto [bundle, err] = bmds::ZoneBundle::open(source, size);
                if (err.isError()) return;
                for (uint32_t i = 0; i < bundle.count() && i < 4096; ++i) {
                        bmds::ZoneBundle::Entry e;
                        if (bundle.entry(source, i, e).isError()) continue;
                        bmds::ZoneBundle::Entry again;
                        (void)bundle.find(source, e.nameView(), again);
                        (void)bmds::ZoneBundle::rules(source, e);
                }
        }

        void padDatabase(const uint8_t *data, size_t size) {
                if (size < 4) return;
                const uint16_t         vendor = static_cast<uint16_t>(data[0] | data[1] << 8);
                const uint16_t         product = static_cast<uint16_t>(data[2] | data[3] << 8);
                const bmds::StringView text(reinterpret_cast<const char *>(data + 4), size - 4);
                bmds::PadMapping       m;
                bmds::StringView       name;
                if (bmds::PadMapping::find(text, vendor, product, m, &name)) {
                        if (name.data() < text.data() || name.data() + name.size() > text.data() + text.size())
                                std::abort();
                }
                (void)bmds::PadMapping::parse(text, m);
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        if (size < 1) return 0;
        switch (data[0] % 3) {
                case 0: iconPack(data + 1, size - 1); break;
                case 1: zoneBundle(data + 1, size - 1); break;
                default: padDatabase(data + 1, size - 1); break;
        }
        return 0;
}
