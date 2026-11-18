/**
 * @file      rom.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the ROM's partition (bmds/romimage.h, devplan/rom.md §12c): any bytes as a
 * partition, its header's CRC and each tree version's made right first (so what is fuzzed is the
 * checks behind them, not the CRCs). What opens must keep every file inside the partition, list
 * every file exactly once from the root, find every file by its path, and be accepted again by
 * the writer (whose checks must be no stricter than open()'s); its log walked to the end, its space
 * laid out covering the partition once, and it compared with itself (no change). Seeds:
 * tests/fuzz/make-rom-seeds.sh.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <bmds/pngdecoder.h>
#include <bmds/romimage.h>
#include <bmds/rommanifest.h>
#include <bmds/romspace.h>

using bmds::ByteSpan;
using bmds::RomImage;

namespace {

        uint32_t get32(const uint8_t *p) {
                return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
        }

        void put32(uint8_t *p, uint32_t v) {
                for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
        }

        // The CRCs made right: the header's, and each version-like record's in the log.
        void fixCrcs(std::vector<uint8_t> &b) {
                if (b.size() < RomImage::HeaderBytes) return;
                put32(b.data() + 60, bmds::Png::crc32(ByteSpan(b.data(), 60)));
                const uint64_t logAt = get32(b.data() + 8);
                const uint64_t logEnd = logAt + get32(b.data() + 12);
                for (uint64_t at = logAt; at + RomImage::VersionHeaderBytes + 4 <= logEnd && at + 16 <= b.size();
                     at += RomImage::SectorBytes) {
                        if (std::memcmp(b.data() + at, "BMRT", 4) != 0) continue;
                        const uint32_t bytes = get32(b.data() + at + 8);
                        if (bytes < RomImage::VersionHeaderBytes + 4 || at + bytes > logEnd || at + bytes > b.size())
                                continue;
                        put32(b.data() + at + bytes - 4, bmds::Png::crc32(ByteSpan(b.data() + at, bytes - 4)));
                }
        }

        size_t walk(const RomImage &image, const RomImage::Node &dir, int depth) {
                if (depth > 200) std::abort(); // paths are at most 255 bytes: no deeper than 128
                size_t          files = 0;
                RomImage::Child child;
                for (uint32_t at = dir.first; (at = image.nextChild(dir, at, child)) != RomImage::NoEntry;) {
                        if (child.name.isEmpty()) std::abort();
                        files += child.node.directory ? walk(image, child.node, depth + 1) : 1;
                }
                return files;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        std::vector<uint8_t> b(data, data + size);
        fixCrcs(b);
        auto [image, err] = RomImage::open(ByteSpan(b.data(), b.size()));
        if (err.isError()) return 0;
        const uint8_t *end = b.data() + b.size();
        bool           noPath = false;
        for (uint32_t i = 0; i < image.count(); ++i) {
                const RomImage::Entry e = image.entry(i);
                if (e.data.data() < b.data() + image.dataOffset() || e.data.data() + e.data.size() > end) std::abort();
                if (e.hash.size() != RomImage::HashBytes || e.path.isEmpty()) std::abort();
                const auto found = image.find(e.path, noPath);
                if (found.error().isError() || found.value().directory || found.value().index != i) std::abort();
                (void)image.verifyFile(i);
        }
        const auto root = image.find("/", noPath);
        if (root.error().isError() || walk(image, root.value(), 0) != image.count()) std::abort();
        (void)image.newest(0, image.count());

        // The log, sector by sector, to its end.
        for (uint32_t at = image.logOffset(), steps = 0; at < image.logOffset() + image.logBytes(); ++steps) {
                RomImage::LogRecord r;
                const uint32_t      next = image.logRecord(at, r);
                if (next <= at || steps > image.logBytes() / RomImage::SectorBytes) std::abort();
                at = next;
        }
        // The space: the regions cover the partition once, in order.
        bmds::RomSpace space;
        if (space.build(image, &image).isOk()) {
                uint64_t covered = 0;
                for (size_t i = 0; i < space.regionCount(); ++i) {
                        const bmds::RomSpace::Region &r = space.region(i);
                        if (r.kind != bmds::RomSpace::Kind::File && r.offset != covered) std::abort();
                        if (r.kind == bmds::RomSpace::Kind::File &&
                            r.trees != (bmds::RomSpace::Booted | bmds::RomSpace::Pending))
                                std::abort();
                        const uint64_t end = uint64_t(r.offset) + r.bytes;
                        if (end > covered) covered = end;
                }
                if (covered != b.size() || space.summary().freeNow != space.summary().freeAfterBoot) std::abort();
        }
        if (bmds::RomManifest::compareTrees(image, image, nullptr, nullptr) != 0) std::abort();

        // What open() took, the writer takes.
        std::vector<RomImage::NewEntry> entries;
        for (uint32_t i = 0; i < image.count(); ++i) {
                const RomImage::Entry e = image.entry(i);
                entries.push_back(
                        {e.path, e.flags, e.alignLog2, e.offset, uint32_t(e.data.size()), e.modified, e.hash});
        }
        const bmds::Span<const RomImage::NewEntry> all(entries.data(), entries.size());
        std::vector<uint8_t>                       out(RomImage::versionBytes(all));
        if (RomImage::writeVersion(all, image.version().sequence, image.version().written, image.version().firmware,
                                   bmds::WritableByteSpan(out.data(), out.size()))
                    .error()
                    .isError())
                std::abort();
        return 0;
}
