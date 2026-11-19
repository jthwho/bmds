/**
 * @file      rom.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ROM (devplan/rom.md): partitions as tools/mkrom.py writes them (format
 * 2, §12c) read by RomImage (the newest good tree version chosen, damage
 * refused), mounted in the VFS (read-only, implied directories, links,
 * Vfs::map()), and the bmds icon theme read by IconPack, its every icon
 * compared with the PNG it came from as bmds_core decodes it.
 */

#include <doctest.h>

#ifdef BMDS_TEST_ROM

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/fileformat.h>
#include <bmds/iconid.h>
#include <bmds/iconpack.h>
#include <bmds/memoryblockdevice.h>
#include <bmds/pngdecoder.h>
#include <bmds/romimage.h>
#include <bmds/vfs.h>
#include <bmds/rommanifest.h>
#include <bmds/romspace.h>
#include <romfiles.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileInfo;
using bmds::FileType;
using bmds::IconId;
using bmds::IconPack;
using bmds::RomImage;
using bmds::StringView;
using bmds::WaitForever;
using bmds::WritableByteSpan;
namespace Vfs = bmds::Vfs;

namespace {

        const Vfs::Context Root{"/", nullptr};

        // A file's bytes in memory aligned to a page, as a mapped flash partition is.
        struct Loaded {
                        uint8_t *data = nullptr;
                        size_t   size = 0;

                        explicit Loaded(const char *path) {
                                std::ifstream     in(path, std::ios::binary);
                                std::stringstream s;
                                s << in.rdbuf();
                                const std::string bytes = s.str();
                                size = bytes.size();
                                data = static_cast<uint8_t *>(std::aligned_alloc(4096, (size + 4095) / 4096 * 4096));
                                REQUIRE(data != nullptr);
                                std::memcpy(data, bytes.data(), size);
                        }

                        Loaded(const Loaded &) = delete;
                        Loaded &operator=(const Loaded &) = delete;
                        ~Loaded() { std::free(data); }

                        ByteSpan span() const { return ByteSpan(data, size); }
        };

        std::string readHostFile(const std::string &path) {
                std::ifstream     in(path, std::ios::binary);
                std::stringstream s;
                s << in.rdbuf();
                return s.str();
        }

        std::string text(ByteSpan bytes) {
                return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }

        uint32_t entryOf(const RomImage &image, const char *path) {
                bool       noPath = false;
                const auto found = image.find(StringView(path), noPath);
                REQUIRE(found.error().isOk());
                REQUIRE_FALSE(found.value().directory);
                return found.value().index;
        }

        // A directory's children, as "name" for a file and "name/" for a directory.
        std::vector<std::string> childrenOf(const RomImage &image, const char *path) {
                bool       noPath = false;
                const auto found = image.find(StringView(path), noPath);
                REQUIRE(found.error().isOk());
                REQUIRE(found.value().directory);
                std::vector<std::string> out;
                RomImage::Child          child;
                for (uint32_t at = found.value().first;
                     (at = image.nextChild(found.value(), at, child)) != RomImage::NoEntry;)
                        out.push_back(std::string(child.name.data(), child.name.size()) +
                                      (child.node.directory ? "/" : ""));
                return out;
        }

        // The files reached by listing from @p dir down (a damaged table must still end).
        size_t walk(const RomImage &image, const RomImage::Node &dir, int depth) {
                size_t          files = 0;
                RomImage::Child child;
                for (uint32_t at = dir.first; (at = image.nextChild(dir, at, child)) != RomImage::NoEntry;) {
                        if (!child.node.directory) {
                                ++files;
                        } else if (depth < 300) {
                                files += walk(image, child.node, depth + 1);
                        }
                }
                return files;
        }

        // The files reached by listing from the root.
        size_t walkAll(const RomImage &image) {
                bool       noPath = false;
                const auto root = image.find("/", noPath);
                REQUIRE(root.error().isOk());
                return walk(image, root.value(), 0);
        }

        // Reads the whole file at @p path through the VFS.
        std::string readVfs(const char *path) {
                auto [file, err] = Vfs::open(StringView(path), Vfs::Read, Root);
                REQUIRE(err.isOk());
                std::string out;
                uint8_t     buf[7]; // small reads, across the file
                for (;;) {
                        auto [n, rerr] = file->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                        REQUIRE(rerr.isOk());
                        out.append(reinterpret_cast<const char *>(buf), n);
                }
                return out;
        }

        void put32(uint8_t *p, uint32_t v) {
                for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
        }

        uint32_t get32(const uint8_t *p) {
                return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
        }

        // The partition header's CRC made right again after a change to it.
        void fixCrc(std::vector<uint8_t> &image) {
                put32(image.data() + RomImage::HeaderBytes - 4,
                      bmds::Png::crc32(ByteSpan(image.data(), RomImage::HeaderBytes - 4)));
        }

        // The tree version at @p at: its CRC made right again after a change to it.
        void fixVersionCrc(std::vector<uint8_t> &image, uint32_t at) {
                const uint32_t body = get32(image.data() + at + 8) - 4;
                put32(image.data() + at + body, bmds::Png::crc32(ByteSpan(image.data() + at, body)));
        }

        // mkrom writes one version, at the log's start.
        constexpr uint32_t FirstVersion = RomImage::SectorBytes;

} // namespace

TEST_CASE("rom: mkrom's partition read: paths in order, directories implied, contents, the hash") {
        const Loaded rom(BMDS_TEST_ROM);
        auto [image, err] = RomImage::open(rom.span());
        REQUIRE(err.isOk());
        CHECK(image.verify().isOk());
        // The manifest mkrom wrote: every file, as built.
        const auto counts = bmds::RomManifest::compare(
                image, bmds::Span<const bmds::RomManifestEntry>(RomManifestFiles, RomManifestCount), nullptr, nullptr);
        CHECK(counts.matches());
        CHECK(counts.good == image.count());
        CHECK(RomManifestCount == image.count());
        CHECK(image.version().sequence == 1);
        CHECK(image.version().offset == FirstVersion);
        CHECK(image.version().written.seconds() > 0);
        CHECK(std::all_of(image.version().firmware.begin(), image.version().firmware.end(),
                          [](uint8_t b) { return b == 0; }));
        CHECK(image.logOffset() == RomImage::SectorBytes);
        CHECK(image.dataOffset() % RomImage::PageBytes == 0);

        // The root's children, files and directories, in the order of the paths' bytes: "docs.txt"
        // before docs's files ('.' before '/'), each directory once.
        CHECK(childrenOf(image, "") == std::vector<std::string>{"B.txt", "a-b.txt", "a.txt", "docs.txt", "docs/",
                                                                "hello.txt", "images/", "share/"});
        CHECK(childrenOf(image, "/docs") == std::vector<std::string>{"deep/", "hello-link", "readme.txt"});
        CHECK(childrenOf(image, "docs/deep/") == std::vector<std::string>{"nested/"});
        CHECK(walkAll(image) == image.count());

        // Files' bytes as on the host; paths with and without the leading slash, and doubled ones.
        const std::string dir = std::string(BMDS_ROOT_DIR) + "/tests/host/rom/";
        CHECK(text(image.entry(entryOf(image, "/hello.txt")).data) == readHostFile(dir + "hello.txt"));
        const RomImage::Entry leaf = image.entry(entryOf(image, "docs//deep/nested/leaf.txt"));
        CHECK(text(leaf.data) == readHostFile(dir + "docs/deep/nested/leaf.txt"));
        CHECK(leaf.path == "docs/deep/nested/leaf.txt");
        CHECK(leaf.name == "leaf.txt");
        CHECK(leaf.modified.ns == image.version().written.ns);
        bool noPath = true;
        CHECK(image.find("", noPath).value().directory);
        CHECK(image.find("/", noPath).value().directory);
        CHECK(image.find("/", noPath).value().end == image.count());
        CHECK(image.find("/docs/deep", noPath).value().directory);
        CHECK(image.find("/docs/deep/", noPath).value().directory);

        // Missing: the last component (noPath false), or one along the way, or a file in the way.
        CHECK(image.find("/docs/nothing", noPath).error() == Error::NotExist);
        CHECK_FALSE(noPath);
        CHECK(image.find("/doc", noPath).error() == Error::NotExist); // a prefix of a name isn't it
        CHECK_FALSE(noPath);
        CHECK(image.find("/nothing/readme.txt", noPath).error() == Error::NotExist);
        CHECK(noPath);
        CHECK(image.find("/hello.txt/x", noPath).error() == Error::NotExist);
        CHECK(noPath);
        const std::string longName(300, 'x');
        CHECK(image.find(StringView(longName.c_str()), noPath).error() == Error::NotExist);
        CHECK_FALSE(noPath);
        const std::string longDir = "/" + longName + "/y";
        CHECK(image.find(StringView(longDir.c_str()), noPath).error() == Error::NotExist);
        CHECK(noPath);

        // Each file at its alignment: --align 4k=images/*, 64k=a.txt; the rest a cache line.
        const RomImage::Entry png = image.entry(entryOf(image, "/images/grey.png"));
        CHECK(png.alignLog2 == 12);
        CHECK(png.offset % 4096 == 0);
        CHECK((png.data.data() - rom.data) % 4096 == 0);
        CHECK(text(png.data) == readHostFile(std::string(BMDS_ROOT_DIR) + "/tests/host/png/grey.png"));
        // A page-aligned file owns its last page: 0 after its end, nothing else in it (rom.md §6).
        {
                const size_t end = png.offset + png.data.size();
                for (size_t i = end; i < (end + 4095) / 4096 * 4096; ++i) CHECK(rom.data[i] == 0);
                for (uint32_t i = 0; i < image.count(); ++i) {
                        const RomImage::Entry o = image.entry(i);
                        if (o.path == png.path || o.data.isEmpty()) continue;
                        CHECK((o.offset >= (end + 4095) / 4096 * 4096 || o.offset + o.data.size() <= png.offset));
                }
        }
        const RomImage::Entry page = image.entry(entryOf(image, "/a.txt"));
        CHECK(page.alignLog2 == 16);
        CHECK(page.offset % RomImage::PageBytes == 0);
        CHECK(page.offset == image.dataOffset()); // on pages, first
        for (uint32_t i = 0; i < image.count(); ++i) {
                const RomImage::Entry e = image.entry(i);
                CHECK(e.offset % (1u << e.alignLog2) == 0);
                CHECK(e.alignLog2 >= 6);
                CHECK(e.flags == (e.path == "docs/hello-link" ? RomImage::System : 0));
                // Nothing else on a 64 KB-aligned file's pages.
                if (e.path != "a.txt") CHECK(e.offset >= page.offset + RomImage::PageBytes);
        }

        // The link: a System file holding Cygwin's link.
        CHECK(text(image.entry(entryOf(image, "/docs/hello-link")).data).rfind("!<symlink>", 0) == 0);
}

TEST_CASE("rom: damaged partitions are refused, never read past") {
        const Loaded               rom(BMDS_TEST_ROM);
        const std::vector<uint8_t> good(rom.data, rom.data + rom.size);

        auto opens = [](const std::vector<uint8_t> &bytes) {
                return RomImage::open(ByteSpan(bytes.data(), bytes.size())).error();
        };
        CHECK(opens(good) == Error::Ok);

        std::vector<uint8_t> bad = good;
        bad[0] = 'X';
        CHECK(opens(bad) == Error::CorruptData);
        bad = good;
        bad[12] ^= 1; // the log's size, under the header's CRC
        CHECK(opens(bad) == Error::CorruptData);
        bad = good;
        bad[4] = 1; // format 1 (not read any more)
        CHECK(opens(bad) == Error::NotSupported);
        bad = std::vector<uint8_t>(good.begin(), good.end() - 1); // cut short: the last file is outside
        CHECK(opens(bad) == Error::CorruptData);
        CHECK(opens(std::vector<uint8_t>(good.begin(), good.begin() + 40)) == Error::CorruptData);
        // More bytes after it (a partition larger than its image) are fine.
        bad = good;
        bad.resize(good.size() + 8192, 0xff);
        CHECK(opens(bad) == Error::Ok);
        // The log reaching past the files' start: refused, the header's CRC right.
        bad = good;
        put32(bad.data() + 12, get32(bad.data() + 16));
        fixCrc(bad);
        CHECK(opens(bad) == Error::CorruptData);

        // A file's content changed: it opens, its hash says so.
        bad = good;
        bad.back() ^= 0x80;
        {
                auto [image, err] = RomImage::open(ByteSpan(bad.data(), bad.size()));
                REQUIRE(err.isOk());
                CHECK(image.verify() == Error::CorruptData);
                uint32_t damaged = 0;
                for (uint32_t i = 0; i < image.count(); ++i) damaged += image.verifyFile(i).isError() ? 1 : 0;
                CHECK(damaged == 1);
        }

        // The only version damaged (a byte of its entries, its CRC not made right): no tree.
        const uint32_t entries = FirstVersion + RomImage::VersionHeaderBytes;
        bad = good;
        bad[entries + 3] ^= 1;
        CHECK(opens(bad) == Error::CorruptData);

        // Whole (the CRC made right) but wrong: each refused.
        auto refused = [&](auto change) {
                std::vector<uint8_t> v = good;
                change(v);
                fixVersionCrc(v, FirstVersion);
                return opens(v) == Error::CorruptData;
        };
        CHECK(refused([&](std::vector<uint8_t> &v) { put32(v.data() + entries + 12, 0x7fffffff); })); // a size
        CHECK(refused([&](std::vector<uint8_t> &v) { put32(v.data() + entries + 8, 0); }));           // in the log
        CHECK(refused([&](std::vector<uint8_t> &v) { v[entries + 6] = 30; }));                        // an alignment
        CHECK(refused([&](std::vector<uint8_t> &v) { v[entries + 7] = 0x80; }));                      // a flag
        CHECK(refused([&](std::vector<uint8_t> &v) { v[entries + 61] = 1; }));               // compression's reserved
        CHECK(refused([&](std::vector<uint8_t> &v) { v[entries + 64] = 1; }));               // reserved
        CHECK(refused([&](std::vector<uint8_t> &v) { put32(v.data() + entries + 56, 5); })); // a size, not compressed
        // A time no Timestamp holds (found by rom_fuzz: its nanoseconds overflowed).
        CHECK(refused([&](std::vector<uint8_t> &v) { put32(v.data() + entries + 20, 0x40000000); }));
        CHECK(refused([&](std::vector<uint8_t> &v) { put32(v.data() + FirstVersion + 36, 0x80000000); })); // reserved
        CHECK(refused([&](std::vector<uint8_t> &v) {
                // Two entries swapped: out of order.
                std::swap_ranges(v.begin() + entries, v.begin() + entries + RomImage::EntryBytes,
                                 v.begin() + entries + RomImage::EntryBytes);
        }));
        CHECK(refused([&](std::vector<uint8_t> &v) {
                // Entry 0 named as entry 1 is: not strictly increasing.
                std::memcpy(v.data() + entries, v.data() + entries + RomImage::EntryBytes, 6);
        }));
        {
                auto [image, err] = RomImage::open(rom.span());
                REQUIRE(err.isOk());
                // A path that is a file and a directory: "docs" (docs.txt's path, cut) beside docs/.
                const uint32_t at = entryOf(image, "/docs.txt");
                CHECK(refused([&](std::vector<uint8_t> &v) { v[entries + at * RomImage::EntryBytes + 4] = 4; }));
                // A path with a doubled '/', and one with a "..".
                const uint32_t leaf = entryOf(image, "/docs/deep/nested/leaf.txt");
                const uint32_t pathsAt = get32(good.data() + FirstVersion + 20);
                const uint32_t leafPath =
                        FirstVersion + pathsAt + get32(good.data() + entries + leaf * RomImage::EntryBytes);
                CHECK(refused(
                        [&](std::vector<uint8_t> &v) { v[leafPath + 5] = '/'; })); // "docs//eep/...", "docs/../x/..."
                CHECK(refused([&](std::vector<uint8_t> &v) { std::memcpy(v.data() + leafPath + 5, "../x", 4); }));
        }
        // An entry count reaching past the paths.
        CHECK(refused([&](std::vector<uint8_t> &v) { put32(v.data() + FirstVersion + 16, 0x10000); }));
}

TEST_CASE("rom: the newest good tree version is the tree") {
        const Loaded               rom(BMDS_TEST_ROM);
        const std::vector<uint8_t> good(rom.data, rom.data + rom.size);
        const uint32_t             bytes = get32(good.data() + FirstVersion + 8);
        const uint32_t             second = FirstVersion + (bytes + 4095) / 4096 * 4096;

        // A second version after the first: sequence 2, its first file's time changed.
        std::vector<uint8_t> two = good;
        std::memcpy(two.data() + second, good.data() + FirstVersion, bytes);
        put32(two.data() + second + 12, 2);
        put32(two.data() + second + RomImage::VersionHeaderBytes + 16, 12345);
        put32(two.data() + second + RomImage::VersionHeaderBytes + 20, 0);
        fixVersionCrc(two, second);
        {
                auto [image, err] = RomImage::open(ByteSpan(two.data(), two.size()));
                REQUIRE(err.isOk());
                CHECK(image.version().sequence == 2);
                CHECK(image.version().offset == second);
                CHECK(image.entry(0).modified.seconds() == 12345);
        }
        // Older after newer in the log: the sequence decides, not the place.
        {
                std::vector<uint8_t> swapped = two;
                put32(swapped.data() + FirstVersion + 12, 3);
                fixVersionCrc(swapped, FirstVersion);
                auto [image, err] = RomImage::open(ByteSpan(swapped.data(), swapped.size()));
                REQUIRE(err.isOk());
                CHECK(image.version().sequence == 3);
                CHECK(image.version().offset == FirstVersion);
        }
        // The newer cut short (its CRC fails): the older.
        {
                std::vector<uint8_t> cut = two;
                std::fill(cut.begin() + second + bytes / 2, cut.begin() + second + bytes, 0xff);
                auto [image, err] = RomImage::open(ByteSpan(cut.data(), cut.size()));
                REQUIRE(err.isOk());
                CHECK(image.version().sequence == 1);
        }
        // The newer whole but wrong (a file outside the partition): the older.
        {
                std::vector<uint8_t> wrong = two;
                put32(wrong.data() + second + RomImage::VersionHeaderBytes + 12, 0x7fffffff);
                fixVersionCrc(wrong, second);
                auto [image, err] = RomImage::open(ByteSpan(wrong.data(), wrong.size()));
                REQUIRE(err.isOk());
                CHECK(image.version().sequence == 1);
        }
        // An empty tree is a tree: the root, nothing in it.
        {
                std::vector<uint8_t> empty = two;
                put32(empty.data() + second + 8, RomImage::VersionHeaderBytes + 4);
                put32(empty.data() + second + 16, 0);
                put32(empty.data() + second + 20, RomImage::VersionHeaderBytes);
                put32(empty.data() + second + 24, 0);
                fixVersionCrc(empty, second);
                auto [image, err] = RomImage::open(ByteSpan(empty.data(), empty.size()));
                REQUIRE(err.isOk());
                CHECK(image.count() == 0);
                CHECK(childrenOf(image, "/").empty());
                bool noPath = true;
                CHECK(image.find("/hello.txt", noPath).error() == Error::NotExist);
                CHECK_FALSE(noPath);
        }
}

TEST_CASE("rom: the writer makes mkrom's header and tree version, and refuses what open() would") {
        const Loaded rom(BMDS_TEST_ROM);
        auto [image, err] = RomImage::open(rom.span());
        REQUIRE(err.isOk());

        // The header, byte for byte.
        uint8_t header[RomImage::HeaderBytes];
        REQUIRE(RomImage::writeHeader(image.logOffset(), image.logBytes(), image.dataOffset(),
                                      WritableByteSpan(header, sizeof(header)))
                        .isOk());
        CHECK(std::memcmp(header, rom.data, sizeof(header)) == 0);
        CHECK(RomImage::writeHeader(4096, 4096, 4096, WritableByteSpan(header, sizeof(header))) == Error::Invalid);
        CHECK(RomImage::writeHeader(4096, 100, 65536, WritableByteSpan(header, sizeof(header))) == Error::Invalid);
        CHECK(RomImage::writeHeader(4096, 4096, 65536, WritableByteSpan(header, 10)) == Error::BufferTooSmall);

        // The tree version from what was read: mkrom's, byte for byte.
        std::vector<RomImage::NewEntry> entries;
        for (uint32_t i = 0; i < image.count(); ++i) {
                const RomImage::Entry e = image.entry(i);
                entries.push_back(
                        {e.path, e.flags, e.alignLog2, e.offset, uint32_t(e.data.size()), e.modified, e.hash});
        }
        const bmds::Span<const RomImage::NewEntry> all(entries.data(), entries.size());
        const size_t                               bytes = RomImage::versionBytes(all);
        CHECK(bytes == image.version().bytes);
        std::vector<uint8_t> out(bytes);
        auto [written, werr] =
                RomImage::writeVersion(all, 1, image.version().written, {}, WritableByteSpan(out.data(), out.size()));
        REQUIRE(werr.isOk());
        CHECK(written == bytes);
        CHECK(std::memcmp(out.data(), rom.data + FirstVersion, bytes) == 0);

        // Written as the next version, with a firmware named and a file left out: the tree.
        std::vector<uint8_t> partition(rom.data, rom.data + rom.size);
        const uint32_t       second = FirstVersion + uint32_t(bytes + 4095) / 4096 * 4096;
        uint8_t              firmware[RomImage::HashBytes];
        std::memset(firmware, 0xab, sizeof(firmware));
        std::vector<RomImage::NewEntry> fewer = entries;
        fewer.erase(fewer.begin()); // "B.txt"
        auto [n, nerr] = RomImage::writeVersion(bmds::Span<const RomImage::NewEntry>(fewer.data(), fewer.size()), 2,
                                                bmds::Timestamp::fromSeconds(1800000000),
                                                bmds::Span<const uint8_t>(firmware, sizeof(firmware)),
                                                WritableByteSpan(partition.data() + second, partition.size() - second));
        REQUIRE(nerr.isOk());
        {
                auto [next, oerr] = RomImage::open(ByteSpan(partition.data(), partition.size()));
                REQUIRE(oerr.isOk());
                CHECK(next.version().sequence == 2);
                CHECK(next.version().written.seconds() == 1800000000);
                CHECK(next.version().firmware[0] == 0xab);
                CHECK(next.count() == image.count() - 1);
                bool noPath = false;
                CHECK(next.find("/B.txt", noPath).error() == Error::NotExist);
                CHECK(next.verify().isOk());
        }

        // Refused: each as open() would refuse it.
        auto refuses = [&](auto change) {
                std::vector<RomImage::NewEntry> v = entries;
                change(v);
                std::vector<uint8_t> buf(RomImage::versionBytes({v.data(), v.size()}) + 64);
                return RomImage::writeVersion({v.data(), v.size()}, 2, {}, {}, WritableByteSpan(buf.data(), buf.size()))
                               .error() == Error::Invalid;
        };
        CHECK(refuses([](auto &v) { std::swap(v[0], v[1]); })); // out of order
        CHECK(refuses([](auto &v) { v[1] = v[0]; }));           // twice
        CHECK(refuses([](auto &v) { v[0].path = "/B.txt"; }));  // a leading '/'
        CHECK(refuses([](auto &v) { v[0].path = "B.txt/"; }));  // a trailing '/'
        CHECK(refuses([](auto &v) { v[0].flags = 0x80; }));     // a flag
        CHECK(refuses([](auto &v) { v[0].alignLog2 = 25; }));   // an alignment
        CHECK(refuses([](auto &v) {
                v[0].offset += 1;
                v[0].alignLog2 = 6;
        }));                                                               // off its alignment
        CHECK(refuses([](auto &v) { v[0].hash = v[0].hash.mid(0, 16); })); // a short hash
        // "docs" before docs.txt and docs/: a file and a directory.
        CHECK(refuses([](auto &v) {
                v.insert(v.begin() + 3, v[4]);
                v[3].path = "docs";
        }));
        std::vector<uint8_t> small(10);
        CHECK(RomImage::writeVersion(all, 1, {}, {}, WritableByteSpan(small.data(), small.size())).error() ==
              Error::BufferTooSmall);
        CHECK(RomImage::writeVersion(all, 1, {}, bmds::Span<const uint8_t>(firmware, 5),
                                     WritableByteSpan(out.data(), out.size()))
                      .error() == Error::Invalid);
}

namespace {

        // The test ROM's entries as the writer takes them.
        std::vector<RomImage::NewEntry> newEntries(const RomImage &image) {
                std::vector<RomImage::NewEntry> out;
                for (uint32_t i = 0; i < image.count(); ++i) {
                        const RomImage::Entry e = image.entry(i);
                        out.push_back(
                                {e.path, e.flags, e.alignLog2, e.offset, uint32_t(e.data.size()), e.modified, e.hash});
                }
                return out;
        }

        // @p entries written as a version with @p sequence at @p at (a sector of the log).
        void writeAt(std::vector<uint8_t> &partition, uint32_t at, const std::vector<RomImage::NewEntry> &entries,
                     uint32_t sequence, const uint8_t *firmware = nullptr) {
                const bmds::Span<const RomImage::NewEntry> all(entries.data(), entries.size());
                auto [n, err] = RomImage::writeVersion(
                        all, sequence, bmds::Timestamp::fromSeconds(1800000000 + sequence),
                        firmware != nullptr ? bmds::Span<const uint8_t>(firmware, RomImage::HashBytes)
                                            : bmds::Span<const uint8_t>(),
                        WritableByteSpan(partition.data() + at, partition.size() - at));
                REQUIRE(err.isOk());
                (void)n;
        }

        uint32_t sectorAfter(uint32_t at, size_t bytes) {
                return at + uint32_t(bytes + RomImage::SectorBytes - 1) / RomImage::SectorBytes * RomImage::SectorBytes;
        }

        const bmds::Span<const bmds::RomManifestEntry> TestManifest(RomManifestFiles, RomManifestCount);

} // namespace

TEST_CASE("rom: a tree for this firmware is preferred; one for another is passed over while there is one") {
        const Loaded rom(BMDS_TEST_ROM);
        auto [image, err] = RomImage::open(rom.span());
        REQUIRE(err.isOk());
        std::vector<uint8_t> partition(rom.data, rom.data + rom.size);
        const auto           entries = newEntries(image);
        const uint32_t       second = sectorAfter(FirstVersion, image.version().bytes);
        uint8_t              mine[RomImage::HashBytes], other[RomImage::HashBytes];
        std::memset(mine, 0x11, sizeof(mine));
        std::memset(other, 0x22, sizeof(other));
        writeAt(partition, second, entries, 2, mine);
        const ByteSpan bytes(partition.data(), partition.size());
        CHECK(RomImage::open(bytes, bmds::Span<const uint8_t>(mine, 32)).value().version().sequence == 2);
        CHECK(RomImage::open(bytes, bmds::Span<const uint8_t>(other, 32)).value().version().sequence == 1);
        CHECK(RomImage::open(bytes).value().version().sequence == 2); // no firmware given: the newest
        // Only versions for another firmware: the newest of them, rather than no ROM.
        std::vector<uint8_t> onlyOther = partition;
        writeAt(onlyOther, FirstVersion, entries, 1, other);
        writeAt(onlyOther, second, entries, 2, other);
        CHECK(RomImage::open(ByteSpan(onlyOther.data(), onlyOther.size()), bmds::Span<const uint8_t>(mine, 32))
                      .value()
                      .version()
                      .sequence == 2);
}

TEST_CASE("rom: the log read sector by sector: versions, wrong, damaged, erased and other") {
        const Loaded rom(BMDS_TEST_ROM);
        auto [image, err] = RomImage::open(rom.span());
        REQUIRE(err.isOk());
        std::vector<uint8_t> partition(rom.data, rom.data + rom.size);
        const auto           entries = newEntries(image);
        const uint32_t       bytes = image.version().bytes;
        const uint32_t       second = sectorAfter(FirstVersion, bytes);
        const uint32_t       third = sectorAfter(second, bytes);
        const uint32_t       fourth = sectorAfter(third, bytes);
        writeAt(partition, second, entries, 2);
        // The third whole but wrong (a file outside the partition), the fourth cut short.
        writeAt(partition, third, entries, 3);
        put32(partition.data() + third + RomImage::VersionHeaderBytes + 12, 0x7fffffff);
        fixVersionCrc(partition, third);
        writeAt(partition, fourth, entries, 4);
        std::fill(partition.begin() + fourth + 100, partition.begin() + fourth + bytes, 0xff);
        // Something in the sector after: neither erased nor a version.
        const uint32_t junk = sectorAfter(fourth, 1);
        partition[junk + 7] = 0x5a;

        auto [now, nerr] = RomImage::open(ByteSpan(partition.data(), partition.size()));
        REQUIRE(nerr.isOk());
        CHECK(now.version().sequence == 2); // 3 is wrong, 4 isn't whole
        using Kind = RomImage::LogRecord::Kind;
        std::vector<std::pair<Kind, uint32_t>> seen;
        uint32_t                               erased = 0;
        for (uint32_t at = now.logOffset(); at < now.logOffset() + now.logBytes();) {
                RomImage::LogRecord r;
                const uint32_t      next = now.logRecord(at, r);
                CHECK(next > at);
                CHECK(r.offset == at);
                if (r.kind == Kind::Erased) {
                        ++erased;
                } else {
                        seen.push_back({r.kind, r.sequence});
                }
                if (r.kind == Kind::Version) CHECK(r.count == image.count());
                at = next;
        }
        CHECK(seen ==
              std::vector<std::pair<Kind, uint32_t>>{
                      {Kind::Version, 1}, {Kind::Version, 2}, {Kind::Wrong, 3}, {Kind::Damaged, 4}, {Kind::Other, 0}});
        CHECK(erased == now.logBytes() / RomImage::SectorBytes - 5);
        // Outside the log, or not on a sector: the end.
        RomImage::LogRecord r;
        CHECK(now.logRecord(0, r) == now.logOffset() + now.logBytes());
        CHECK(now.logRecord(now.logOffset() + 1, r) == now.logOffset() + now.logBytes());
}

TEST_CASE("rom: the system files compared with the manifest, and trees with each other") {
        const Loaded rom(BMDS_TEST_ROM);
        auto [image, err] = RomImage::open(rom.span());
        REQUIRE(err.isOk());
        std::vector<uint8_t> partition(rom.data, rom.data + rom.size);
        auto                 entries = newEntries(image);
        const uint32_t       second = sectorAfter(FirstVersion, image.version().bytes);
        // B.txt changed (another file's bytes), a-b.txt the user's, a.txt gone, a user's file
        // added, and hello.txt moved (same bytes, another place: the user's x.txt's).
        uint8_t otherHash[RomImage::HashBytes];
        std::memset(otherHash, 0x33, sizeof(otherHash));
        auto find = [&](const char *path) -> RomImage::NewEntry & {
                for (auto &e : entries)
                        if (e.path == path) return e;
                FAIL("no " << path);
                return entries[0];
        };
        find("B.txt").hash = bmds::Span<const uint8_t>(otherHash, sizeof(otherHash));
        find("a-b.txt").flags = RomImage::User;
        const uint32_t helloAt = find("hello.txt").offset;
        find("hello.txt").offset = find("docs.txt").offset;
        entries.erase(entries.begin() + 2); // a.txt
        RomImage::NewEntry mine = find("hello.txt");
        mine.path = "x.txt";
        mine.flags = RomImage::User;
        mine.offset = helloAt;
        entries.push_back(mine);
        writeAt(partition, second, entries, 2);
        auto [changed, cerr] = RomImage::open(ByteSpan(partition.data(), partition.size()));
        REQUIRE(cerr.isOk());
        REQUIRE(changed.version().sequence == 2);

        std::vector<std::string> found;
        const auto               counts = bmds::RomManifest::compare(
                changed, TestManifest,
                [](void *c, const bmds::RomManifest::Found &f) {
                        static_cast<std::vector<std::string> *>(c)->push_back(
                                std::string(bmds::RomManifest::text(f.what)) + " " +
                                std::string(f.path.data(), f.path.size()));
                },
                &found);
        CHECK(found == std::vector<std::string>{"changed B.txt", "replaced a-b.txt", "missing a.txt"});
        CHECK(counts.changed == 1);
        CHECK(counts.replaced == 1);
        CHECK(counts.missing == 1);
        CHECK(counts.extra == 0);
        CHECK(counts.user == 1); // x.txt
        CHECK(counts.good == image.count() - 3);
        CHECK_FALSE(counts.matches());
        // A system file the manifest doesn't list: extra (the manifest without its last).
        const auto fewer = bmds::RomManifest::compare(
                image, bmds::Span<const bmds::RomManifestEntry>(RomManifestFiles, RomManifestCount - 1), nullptr,
                nullptr);
        CHECK(fewer.extra == 1);
        CHECK_FALSE(fewer.matches());

        // The booted tree against the changed one.
        found.clear();
        const size_t n = bmds::RomManifest::compareTrees(
                image, changed,
                [](void *c, const bmds::RomManifest::Changed &ch) {
                        static_cast<std::vector<std::string> *>(c)->push_back(
                                std::string(bmds::RomManifest::text(ch.what)) + " " +
                                std::string(ch.path.data(), ch.path.size()));
                },
                &found);
        CHECK(found == std::vector<std::string>{"replaced B.txt", "replaced a-b.txt", "removed a.txt",
                                                "moved hello.txt", "added x.txt"});
        CHECK(n == 5);
        CHECK(bmds::RomManifest::compareTrees(image, image, nullptr, nullptr) == 0);
}

TEST_CASE("rom: the partition's space: regions in order, sectors, what is free now and after the next boot") {
        const Loaded rom(BMDS_TEST_ROM);
        // In a partition larger than the image, as on the board: 256 KB more, on whole pages.
        const size_t imageEnd = (rom.size + RomImage::PageBytes - 1) / RomImage::PageBytes * RomImage::PageBytes;
        std::vector<uint8_t> partition(rom.data, rom.data + rom.size);
        partition.resize(imageEnd + 256 * 1024, 0xff);
        auto [image, err] = RomImage::open(ByteSpan(partition.data(), partition.size()));
        REQUIRE(err.isOk());
        {
                bmds::RomSpace space;
                REQUIRE(space.build(image, nullptr).isOk());
                const auto &sum = space.summary();
                CHECK(sum.partitionBytes == partition.size());
                CHECK(sum.reservedBytes == image.dataOffset());
                CHECK(sum.files == image.count());
                CHECK(sum.overlaps == 0);
                CHECK(sum.filesBytes == image.filesBytes());
                CHECK(sum.freeNow == sum.freeAfterBoot); // nothing pending
                CHECK(sum.freeNow % RomImage::SectorBytes == 0);
                // The regions cover the partition once, in order.
                uint64_t at = 0;
                for (size_t i = 0; i < space.regionCount(); ++i) {
                        CHECK(space.region(i).offset == at);
                        at += space.region(i).bytes;
                }
                CHECK(at == partition.size());
                CHECK(space.region(0).kind == bmds::RomSpace::Kind::Header);
                CHECK(space.region(1).kind == bmds::RomSpace::Kind::Log);
                CHECK(space.region(space.regionCount() - 1).kind == bmds::RomSpace::Kind::Free);
                // a.txt on its own 64 KB page: the rest of the page free but not counted as a run of pages.
                CHECK(sum.largestPagesNow > 0);
                CHECK(sum.largestPagesNow % RomImage::PageBytes == 0);
                CHECK(sum.largestNow >= sum.largestPagesNow);
                CHECK(sum.holeBytes == sum.freeNow - sum.largestNow);
                CHECK(sum.paddingBytes > 0); // small files share sectors
                // Every free byte is a whole free sector or padding.
                uint64_t free = 0;
                for (size_t i = 0; i < space.regionCount(); ++i)
                        if (space.region(i).kind == bmds::RomSpace::Kind::Free) free += space.region(i).bytes;
                CHECK(free == sum.freeNow + sum.paddingBytes);
                CHECK(space.sectorCount() == partition.size() / RomImage::SectorBytes);
                CHECK(space.sectorFlags(0) == (bmds::RomSpace::UsedNow | bmds::RomSpace::UsedAfter));
                CHECK(space.sectorFlags(space.sectorCount() - 1) == 0);
        }
        // A pending tree without a.txt (64 KB page) and with B.txt somewhere new: a.txt's page
        // is free after the next boot, B.txt's new place used now.
        auto           entries = newEntries(image);
        const uint32_t second = sectorAfter(FirstVersion, image.version().bytes);
        const uint32_t newPlace = uint32_t(imageEnd) + 64 * 1024;
        for (auto &e : entries)
                if (e.path == "B.txt") e.offset = newPlace;
        entries.erase(entries.begin() + 2); // a.txt
        writeAt(partition, second, entries, 2);
        auto [pending, perr] = RomImage::open(ByteSpan(partition.data(), partition.size()));
        REQUIRE(perr.isOk());
        REQUIRE(pending.version().sequence == 2);
        bmds::RomSpace space;
        REQUIRE(space.build(image, &pending).isOk());
        const auto &sum = space.summary();
        CHECK(sum.freeAfterBoot > sum.freeNow);
        CHECK(sum.freeAfterBoot - sum.freeNow >= RomImage::SectorBytes);
        const uint32_t page = image.dataOffset() / RomImage::SectorBytes;
        CHECK(space.sectorFlags(page) == bmds::RomSpace::UsedNow); // a.txt: booted only
        CHECK(space.sectorFlags(newPlace / RomImage::SectorBytes) ==
              (bmds::RomSpace::UsedNow | bmds::RomSpace::UsedAfter));
        size_t both = 0, bootedOnly = 0, pendingOnly = 0;
        for (size_t i = 0; i < space.regionCount(); ++i) {
                const auto &r = space.region(i);
                if (r.kind != bmds::RomSpace::Kind::File) continue;
                both += r.trees == (bmds::RomSpace::Booted | bmds::RomSpace::Pending);
                bootedOnly += r.trees == bmds::RomSpace::Booted;
                pendingOnly += r.trees == bmds::RomSpace::Pending;
        }
        CHECK(bootedOnly == 2);  // a.txt, B.txt's old place
        CHECK(pendingOnly == 1); // B.txt's new place
        CHECK(both == image.count() - 2);

        // Booted and pending sharing bytes under two names (a rename that kept them): no overlap.
        {
                auto renamed = newEntries(image);
                for (auto &e : renamed)
                        if (e.path == "hello.txt") e.path = "hello2.txt";
                std::sort(renamed.begin(), renamed.end(),
                          [](const RomImage::NewEntry &a, const RomImage::NewEntry &b) { return a.path < b.path; });
                std::vector<uint8_t> p2 = partition;
                writeAt(p2, second, renamed, 2);
                auto [r, rerr] = RomImage::open(ByteSpan(p2.data(), p2.size()));
                REQUIRE(rerr.isOk());
                bmds::RomSpace s2;
                REQUIRE(s2.build(image, &r).isOk());
                CHECK(s2.summary().overlaps == 0);
                CHECK(s2.summary().files == image.count() + 1); // hello.txt and hello2.txt, at one place
                CHECK(s2.summary().freeNow == s2.summary().freeAfterBoot);
        }

        // Two files at one place: an overlap, said.
        auto overlapping = newEntries(image);
        overlapping[1].offset = overlapping[0].offset; // a-b.txt over B.txt (both at 64 bytes)
        writeAt(partition, sectorAfter(second, image.version().bytes), overlapping, 3);
        auto [bad, berr] = RomImage::open(ByteSpan(partition.data(), partition.size()));
        REQUIRE(berr.isOk());
        bmds::RomSpace o;
        REQUIRE(o.build(bad, nullptr).isOk());
        CHECK(o.summary().overlaps == 1);
}

TEST_CASE("rom: random damage to a whole tree version: refused or read, never past the partition") {
        const Loaded               rom(BMDS_TEST_ROM);
        const std::vector<uint8_t> good(rom.data, rom.data + rom.size);
        const uint32_t             bytes = get32(good.data() + FirstVersion + 8);
        std::mt19937               random(7);
        size_t                     opened = 0;
        for (int i = 0; i < 3000; ++i) {
                std::vector<uint8_t> bad = good;
                // Bytes of the entries and paths, the CRC made right so the checks see them.
                for (int k = 0; k < 1 + i % 4; ++k)
                        bad[FirstVersion + RomImage::VersionHeaderBytes +
                            random() % (bytes - RomImage::VersionHeaderBytes - 4)] = uint8_t(random());
                fixVersionCrc(bad, FirstVersion);
                auto [damaged, derr] = RomImage::open(ByteSpan(bad.data(), bad.size()));
                if (derr.isError()) continue;
                ++opened;
                for (uint32_t n = 0; n < damaged.count(); ++n) {
                        const RomImage::Entry e = damaged.entry(n);
                        CHECK(e.data.data() >= bad.data());
                        CHECK(e.data.data() + e.data.size() <= bad.data() + bad.size());
                }
                CHECK(walkAll(damaged) == damaged.count());
                bool noPath = false;
                (void)damaged.find("/docs/deep/nested/leaf.txt", noPath);
        }
        CHECK(opened > 0); // some damage (a time, a hash, a size in bounds) still makes a good tree
}

TEST_CASE("rom: a version's entries are the size it says: older, shorter ones read, their missing fields 0") {
        const Loaded         rom(BMDS_TEST_ROM);
        std::vector<uint8_t> bytes(rom.data, rom.data + rom.size);
        auto [image, err] = RomImage::open(ByteSpan(bytes.data(), bytes.size()));
        REQUIRE(err.isOk());
        // The same tree written with 64-byte entries (format 2's first), as sequence 2.
        const uint32_t       count = image.count();
        const uint32_t       pathsAt = get32(bytes.data() + FirstVersion + 20);
        const uint32_t       pathsBytes = get32(bytes.data() + FirstVersion + 24);
        std::vector<uint8_t> v(RomImage::VersionHeaderBytes);
        std::memcpy(v.data(), bytes.data() + FirstVersion, RomImage::VersionHeaderBytes);
        for (uint32_t i = 0; i < count; ++i) {
                const uint8_t *e =
                        bytes.data() + FirstVersion + RomImage::VersionHeaderBytes + i * RomImage::EntryBytes;
                v.insert(v.end(), e, e + 64);
        }
        const uint32_t newPathsAt = uint32_t(v.size());
        v.insert(v.end(), bytes.begin() + FirstVersion + pathsAt, bytes.begin() + FirstVersion + pathsAt + pathsBytes);
        v.resize(v.size() + 4);
        v[6] = 64;
        v[7] = 0;
        put32(v.data() + 8, uint32_t(v.size()));
        put32(v.data() + 12, 2);
        put32(v.data() + 20, newPathsAt);
        const uint32_t second = FirstVersion + (image.version().bytes + 4095) / 4096 * 4096;
        std::memcpy(bytes.data() + second, v.data(), v.size());
        fixVersionCrc(bytes, second);
        auto [old, oerr] = RomImage::open(ByteSpan(bytes.data(), bytes.size()));
        REQUIRE(oerr.isOk());
        CHECK(old.version().sequence == 2);
        REQUIRE(old.count() == count);
        CHECK(bmds::RomManifest::compareTrees(image, old, nullptr, nullptr) == 0);
        CHECK(old.verify().isOk());
        for (uint32_t i = 0; i < count; ++i) {
                CHECK(old.entry(i).compression == RomImage::None);
                CHECK(old.entry(i).realSize == 0);
        }
        // An entry size that isn't a format 2 one: refused (the first version stands).
        v[6] = 60;
        std::memcpy(bytes.data() + second, v.data(), v.size());
        fixVersionCrc(bytes, second);
        CHECK(RomImage::open(ByteSpan(bytes.data(), bytes.size())).value().version().sequence == 1);
}

TEST_CASE("rom: a compressed file (a method this firmware doesn't know) leaves the tree good, the file unread") {
        REQUIRE(Vfs::init().isOk());
        const Loaded         rom(BMDS_TEST_ROM);
        std::vector<uint8_t> bytes(rom.data, rom.data + rom.size);
        // hello.txt said to be compressed by method 7, 1000 bytes when read.
        auto [image, err] = RomImage::open(ByteSpan(bytes.data(), bytes.size()));
        REQUIRE(err.isOk());
        const uint32_t at =
                FirstVersion + RomImage::VersionHeaderBytes + entryOf(image, "/hello.txt") * RomImage::EntryBytes;
        put32(bytes.data() + at + 56, 1000);
        bytes[at + 60] = 7;
        fixVersionCrc(bytes, FirstVersion);
        auto [tree, terr] = RomImage::open(ByteSpan(bytes.data(), bytes.size()));
        REQUIRE(terr.isOk());
        const RomImage::Entry e = tree.entry(entryOf(tree, "/hello.txt"));
        CHECK(e.compression == 7);
        CHECK(e.size() == 1000);
        CHECK(e.data.size() == 11);                                 // its stored bytes
        CHECK(tree.verifyFile(entryOf(tree, "/hello.txt")).isOk()); // the hash is of what is stored

        // Mounted: its size as read, but neither read nor mapped; the others as ever. (A copy
        // aligned as a mapped partition is.)
        const Loaded aligned(BMDS_TEST_ROM);
        std::memcpy(aligned.data, bytes.data(), bytes.size());
        bmds::MemoryBlockDevice device(aligned.span());
        char                    name[bmds::BlockDevices::MaxName + 1];
        REQUIRE(bmds::BlockDevices::addNumbered(device, "rom", name).isOk());
        REQUIRE(Vfs::mount("/rom", device).isOk());
        auto [info, serr] = Vfs::stat("/rom/hello.txt", Root);
        REQUIRE(serr.isOk());
        CHECK(info.size == 1000);
        CHECK(Vfs::open("/rom/hello.txt", Vfs::Read, Root).error() == Error::NotSupported);
        CHECK(Vfs::map("/rom/hello.txt", Root).error() == Error::NotSupported);
        CHECK(readVfs("/rom/a.txt") == readHostFile(std::string(BMDS_ROOT_DIR) + "/tests/host/rom/a.txt"));
        Vfs::eject(device);
        bmds::BlockDevices::remove(device);
}

TEST_CASE("rom: mounted: read-only files, directories, a link, Vfs::map()") {
        REQUIRE(Vfs::init().isOk());
        const Loaded            rom(BMDS_TEST_ROM);
        bmds::MemoryBlockDevice device(rom.span());
        char                    name[bmds::BlockDevices::MaxName + 1];
        REQUIRE(bmds::BlockDevices::addNumbered(device, "rom", name).isOk());
        REQUIRE(Vfs::mount("/rom", device).isOk());

        const std::string dir = std::string(BMDS_ROOT_DIR) + "/tests/host/rom/";
        CHECK(readVfs("/rom/hello.txt") == readHostFile(dir + "hello.txt"));
        CHECK(readVfs("/rom/docs/deep/nested/leaf.txt") == "deep\n");
        // Through the link (absolute: /rom/hello.txt).
        CHECK(readVfs("/rom/docs/hello-link") == "Hello, ROM\n");
        auto [target, lerr] = Vfs::readLink("/rom/docs/hello-link", Root);
        REQUIRE(lerr.isOk());
        CHECK(target.view() == "/rom/hello.txt");

        auto [info, serr] = Vfs::stat("/rom/hello.txt", Root);
        REQUIRE(serr.isOk());
        CHECK(info.type == FileType::File);
        CHECK(info.size == 11);
        CHECK((info.attributes & FileInfo::ReadOnly) != 0);
        auto [image, ierr] = RomImage::open(rom.span());
        REQUIRE(ierr.isOk());
        CHECK(info.modified.ns == image.entry(entryOf(image, "/hello.txt")).modified.ns);
        // A directory: implied, named, as new as what is under it.
        auto [docs, derr] = Vfs::stat("/rom/docs", Root);
        REQUIRE(derr.isOk());
        CHECK(docs.type == FileType::Directory);
        CHECK(docs.name.view() == "docs");
        CHECK(docs.modified.ns == image.version().written.ns);
        CHECK(Vfs::stat("/rom/nothing", Root).error() == Error::NotExist);

        // Seeking and the size by seek, as fstat() and mmap() of a copy do.
        {
                auto [file, err] = Vfs::open("/rom/hello.txt", Vfs::Read, Root);
                REQUIRE(err.isOk());
                auto [end, eerr] = file->stream().seek(0, bmds::Stream::SeekFrom::End);
                CHECK(eerr.isOk());
                CHECK(end == 11);
                (void)file->stream().seek(7, bmds::Stream::SeekFrom::Start);
                uint8_t buf[16];
                auto [n, rerr] = file->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                CHECK(rerr.isOk());
                CHECK(std::string(reinterpret_cast<char *>(buf), n) == "ROM\n");
        }

        // Nothing changes.
        CHECK(Vfs::open("/rom/hello.txt", Vfs::Read | Vfs::Write, Root).error() == Error::ReadOnly);
        CHECK(Vfs::open("/rom/new.txt", Vfs::Write | Vfs::Create, Root).error() == Error::ReadOnly);
        CHECK(Vfs::mkdir("/rom/newdir", Root) == Error::ReadOnly);
        CHECK(Vfs::mkdir("/rom/docs", Root) == Error::Exists);
        CHECK(Vfs::remove("/rom/hello.txt", Root) == Error::ReadOnly);
        CHECK(Vfs::rename("/rom/a.txt", "/rom/c.txt", Vfs::RenameMode::NoReplace, Root) == Error::ReadOnly);
        CHECK(Vfs::symlink("/rom/x", "/rom/new-link", Root) == Error::ReadOnly);

        // A directory, in the image's order.
        {
                auto [d, err] = Vfs::openDirectory("/rom/docs", Root);
                REQUIRE(err.isOk());
                std::vector<std::string> seen;
                for (;;) {
                        auto [e, nerr] = d.next();
                        if (nerr.isError()) break;
                        seen.push_back(std::string(e.name.view().data(), e.name.size()));
                }
                CHECK(seen == std::vector<std::string>{"deep", "hello-link", "readme.txt"});
        }

        // In place: the bytes in the image itself.
        auto [bytes, merr] = Vfs::map("/rom/docs/readme.txt", Root);
        REQUIRE(merr.isOk());
        CHECK(text(bytes) == "Read me.\n");
        CHECK(bytes.data() >= rom.data);
        CHECK(bytes.data() + bytes.size() <= rom.data + rom.size);
        CHECK(Vfs::map("/rom/docs/hello-link", Root).value().data() == Vfs::map("/rom/hello.txt", Root).value().data());
        CHECK(Vfs::map("/rom/docs", Root).error() == Error::IsDir);
        CHECK(Vfs::map("/rom/nothing", Root).error() == Error::NotExist);
        CHECK(Vfs::map("/dev/null", Root).error() == Error::NotSupported);

        auto [fs, ferr] = Vfs::statfs("/rom", Root);
        REQUIRE(ferr.isOk());
        CHECK(StringView(fs.type) == "rom");
        CHECK(fs.totalBytes == image.dataOffset() + image.filesBytes());
        CHECK(fs.freeBytes == 0);
        Vfs::MountInfo mounts[Vfs::MaxMounts];
        const size_t   n = Vfs::listMounts(mounts);
        bool           listed = false;
        for (size_t i = 0; i < n; ++i)
                listed = listed || (StringView(mounts[i].path) == "/rom" && StringView(mounts[i].type) == "rom");
        CHECK(listed);

        // Unmounted: a file still open goes stale.
        auto [open, oerr] = Vfs::open("/rom/a.txt", Vfs::Read, Root);
        REQUIRE(oerr.isOk());
        Vfs::eject(device);
        uint8_t c;
        CHECK(open->read(WritableByteSpan(&c, 1), WaitForever).error() == Error::NoMedia);
        bmds::BlockDevices::remove(device);
}

#ifdef BMDS_TEST_ZONEINFO
TEST_CASE("rom: its time zone database stands in while none is at the path set") {
        REQUIRE(Vfs::init().isOk());
        const Loaded            rom(BMDS_TEST_ROM);
        bmds::MemoryBlockDevice device(rom.span());
        char                    name[bmds::BlockDevices::MaxName + 1];
        REQUIRE(bmds::BlockDevices::addNumbered(device, "rom", name).isOk());
        REQUIRE(Vfs::mount("/rom", device).isOk());
        REQUIRE(Vfs::setZoneDatabase("/nothere/share/zoneinfo.bin").isOk());
        auto [info, err] = Vfs::zoneDatabaseInfo();
        REQUIRE(err.isOk());
        CHECK(info.fromRom);
        CHECK(info.count == 597);
        CHECK(Vfs::zoneRules("Europe/Berlin").error().isOk());
        auto [zone, serr] = Vfs::stat("/share/zoneinfo/Europe/Berlin", Root);
        CHECK(serr.isOk());
        CHECK(zone.type == FileType::File);
        // None set: not the ROM's either.
        REQUIRE(Vfs::setZoneDatabase("").isOk());
        CHECK(Vfs::zoneRules("Europe/Berlin").error() == Error::NotReady);
        // The ROM's itself, named.
        REQUIRE(Vfs::setZoneDatabase(Vfs::RomZoneDatabase).isOk());
        auto [named, nerr] = Vfs::zoneDatabaseInfo();
        REQUIRE(nerr.isOk());
        CHECK_FALSE(named.fromRom);
        REQUIRE(Vfs::setZoneDatabase("").isOk());
        Vfs::eject(device);
        bmds::BlockDevices::remove(device);
}
#endif

TEST_CASE("rom: a damaged image isn't mounted") {
        REQUIRE(Vfs::init().isOk());
        const Loaded rom(BMDS_TEST_ROM);
        rom.data[30] ^= 0xff; // in the header, under its CRC
        bmds::MemoryBlockDevice device(rom.span());
        char                    name[bmds::BlockDevices::MaxName + 1];
        REQUIRE(bmds::BlockDevices::addNumbered(device, "rom", name).isOk());
        CHECK(Vfs::mount("/rom", device) == Error::CorruptData);
        bmds::BlockDevices::remove(device);
}

TEST_CASE("rom: the bmds icon theme: every icon as its PNG decodes, by id and by name") {
        const Loaded pack(BMDS_TEST_ICONS);
        auto [icons, err] = IconPack::open(pack.span());
        REQUIRE(err.isOk());
        CHECK(icons.count() == bmds::IconIdCount);
        CHECK(icons.side(0) == 16);
        CHECK(icons.side(1) == 32);
        CHECK(icons.side(2) == 0);

        // rom/icons.list: NAME THEME SOURCE [mirror], an id a line.
        std::ifstream list(BMDS_ICONS_LIST);
        std::string   line;
        uint32_t      id = 0;
        while (std::getline(list, line)) {
                line = line.substr(0, line.find('#'));
                std::istringstream fields(line);
                std::string        name, theme, source, option;
                if (!(fields >> name >> theme >> source)) continue;
                const bool mirror = static_cast<bool>(fields >> option) && option == "mirror";
                CAPTURE(name);
                CHECK(icons.name(id) == StringView(name.data(), name.size()));
                CHECK(icons.idOf(StringView(name.data(), name.size())) == id);
                const std::string dir = theme == "glyphs" ? "/thirdparty/glyphs/png/"
                                        : theme == "bmds" ? "/rom/icons/png/"
                                                          : "/thirdparty/flat-color-icons/png/";
                for (uint16_t side : {uint16_t(16), uint16_t(32)}) {
                        const std::string file = readHostFile(std::string(BMDS_ROOT_DIR) + dir + std::to_string(side) +
                                                              "x" + std::to_string(side) + "/" + source + ".png");
                        std::vector<uint32_t> want(size_t(side) * side);
                        const auto            decoded = bmds::Png::decode(
                                ByteSpan(reinterpret_cast<const uint8_t *>(file.data()), file.size()),
                                bmds::Span<uint32_t>(want.data(), want.size()), bmds::MemSpace::Psram);
                        REQUIRE(decoded.error().isOk());
                        const uint32_t *got = icons.pixels(id, side);
                        REQUIRE(got != nullptr);
                        CHECK(reinterpret_cast<uintptr_t>(got) % 64 == 0);
                        bool same = true;
                        for (int y = 0; y < side; ++y)
                                for (int x = 0; x < side; ++x)
                                        same = same &&
                                               got[y * side + x] == want[y * side + (mirror ? side - 1 - x : x)];
                        CHECK(same);
                }
                ++id;
        }
        CHECK(id == icons.count());
        CHECK(icons.pixels(0, 24) == nullptr);
        CHECK(icons.pixels(icons.count(), 16) == nullptr);
        CHECK(icons.idOf("no-such-icon") == IconPack::NoIcon);
        CHECK(icons.idOf("") == IconPack::NoIcon);

        // The generated ids are the file's.
        CHECK(icons.idOf("folder") == uint32_t(IconId::Folder));
        CHECK(icons.idOf("media-playback-start") == uint32_t(IconId::MediaPlaybackStart));
        CHECK(icons.idOf("application-x-executable") == uint32_t(IconId::ApplicationXExecutable));

        // Every generic icon FileFormat names is in the theme (devplan/rom.md §3i).
        for (const char *type :
             {"inode/directory", "application/x-executable", "application/zip", "image/png", "audio/mpeg", "video/mp4",
              "text/plain", "application/x-shellscript", "application/octet-stream"}) {
                CAPTURE(type);
                CHECK(icons.idOf(bmds::FileFormat::genericIcon(type)) != IconPack::NoIcon);
        }
        CHECK(StringView(bmds::FileFormat::Directory) == "inode/directory");
}

TEST_CASE("rom: damaged icon packs are refused") {
        const Loaded               pack(BMDS_TEST_ICONS);
        const std::vector<uint8_t> good(pack.data, pack.data + pack.size);
        auto                       opens = [](std::vector<uint8_t> &bytes) {
                return IconPack::open(ByteSpan(bytes.data(), bytes.size())).error();
        };
        std::vector<uint8_t> bad = good;
        CHECK(opens(bad) == Error::Ok);
        bad[1] = 'X';
        CHECK(opens(bad) == Error::CorruptData);
        bad = good;
        bad[4] = 2;
        CHECK(opens(bad) == Error::NotSupported);
        bad = std::vector<uint8_t>(good.begin(), good.end() - 4);
        CHECK(opens(bad) == Error::CorruptData);
        bad = good;
        put32(bad.data() + IconPack::HeaderBytes + 8, uint32_t(good.size())); // icon 0's 16-pixel image past the end
        CHECK(opens(bad) == Error::CorruptData);
        bad = good;
        put32(bad.data() + IconPack::HeaderBytes + 8, 3); // not aligned
        CHECK(opens(bad) == Error::CorruptData);
        // Two index entries swapped: the names no longer in order.
        bad = good;
        const uint32_t index = good[28] | good[29] << 8 | good[30] << 16 | uint32_t(good[31]) << 24;
        std::swap(bad[index], bad[index + 2]);
        std::swap(bad[index + 1], bad[index + 3]);
        CHECK(opens(bad) == Error::CorruptData);
        // Not aligned in memory: its pixels couldn't be read where they are.
        CHECK(IconPack::open(ByteSpan(pack.data + 1, pack.size - 1)).error() == Error::Invalid);
}

#endif // BMDS_TEST_ROM
