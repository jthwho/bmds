/**
 * @file      rom.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ROM (devplan/rom.md): images as tools/mkrom.py writes them read by
 * RomImage (and refused when damaged), mounted in the VFS (read-only, links,
 * Vfs::map()), and the bmds icon theme read by IconPack, its every icon
 * compared with the PNG it came from as bmds_core decodes it.
 */

#include <doctest.h>

#ifdef BMDS_TEST_ROM

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
#include <romhash.h>

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
                return found.value();
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

        // The image's header CRC made right again after a change to it.
        void fixCrc(std::vector<uint8_t> &image) {
                put32(image.data() + RomImage::HeaderBytes - 4,
                      bmds::Png::crc32(ByteSpan(image.data(), RomImage::HeaderBytes - 4)));
        }

} // namespace

TEST_CASE("rom: mkrom's image read: the tree, names in order, contents, the hash") {
        const Loaded rom(BMDS_TEST_ROM);
        auto [image, err] = RomImage::open(rom.span());
        REQUIRE(err.isOk());
        CHECK(image.verify().isOk());
        CHECK(std::memcmp(image.hash().data(), RomContentHash, sizeof(RomContentHash)) == 0);
        CHECK(image.buildTime().seconds() > 0);

        // The root's children, sorted by their bytes ("B" before "a", "a-b" before "a.").
        const RomImage::Entry root = image.entry(0);
        REQUIRE(root.directory);
        std::vector<std::string> names;
        for (uint32_t i = root.first; i < root.first + root.count; ++i)
                names.push_back(std::string(image.entry(i).name.data(), image.entry(i).name.size()));
        CHECK(names == std::vector<std::string>{"B.txt", "a-b.txt", "a.txt", "docs", "hello.txt", "images", "share"});

        // Files' bytes as on the host; paths with and without the leading slash, and doubled ones.
        const std::string dir = std::string(BMDS_ROOT_DIR) + "/tests/host/rom/";
        CHECK(text(image.entry(entryOf(image, "/hello.txt")).data) == readHostFile(dir + "hello.txt"));
        CHECK(text(image.entry(entryOf(image, "docs//deep/nested/leaf.txt")).data) ==
              readHostFile(dir + "docs/deep/nested/leaf.txt"));
        CHECK(entryOf(image, "") == 0);
        CHECK(entryOf(image, "/") == 0);
        CHECK(image.entry(entryOf(image, "/docs/deep")).directory);

        // Missing: the last component (noPath false), or one along the way, or a file in the way.
        bool noPath = true;
        CHECK(image.find("/docs/nothing", noPath).error() == Error::NotExist);
        CHECK_FALSE(noPath);
        CHECK(image.find("/nothing/readme.txt", noPath).error() == Error::NotExist);
        CHECK(noPath);
        CHECK(image.find("/hello.txt/x", noPath).error() == Error::NotExist);
        CHECK(noPath);
        CHECK(image.findChild(entryOf(image, "/hello.txt"), "x") == RomImage::NoEntry);

        // A mappable file starts on a page; the others on a cache line.
        const RomImage::Entry png = image.entry(entryOf(image, "/images/grey.png"));
        CHECK(png.flags == RomImage::Mappable);
        CHECK((png.data.data() - rom.data) % RomImage::MapAlign == 0);
        CHECK((image.entry(entryOf(image, "/a.txt")).data.data() - rom.data) % 64 == 0);
        CHECK(text(png.data) == readHostFile(std::string(BMDS_ROOT_DIR) + "/tests/host/png/grey.png"));

        // The link: a System file holding Cygwin's link.
        const RomImage::Entry link = image.entry(entryOf(image, "/docs/hello-link"));
        CHECK(link.flags == RomImage::System);
        CHECK(text(link.data).rfind("!<symlink>", 0) == 0);
}

TEST_CASE("rom: damaged images are refused, never read past") {
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
        bad[10] ^= 1; // the size, under the header's CRC
        CHECK(opens(bad) == Error::CorruptData);
        bad = good;
        bad[4] = 2; // a later format
        CHECK(opens(bad) == Error::NotSupported);
        bad = std::vector<uint8_t>(good.begin(), good.end() - 1); // cut short
        CHECK(opens(bad) == Error::CorruptData);
        CHECK(opens(std::vector<uint8_t>(good.begin(), good.begin() + 40)) == Error::CorruptData);
        // More bytes after it (a partition larger than its image) are fine.
        bad = good;
        bad.resize(good.size() + 8192, 0xff);
        CHECK(opens(bad) == Error::Ok);

        // The content changed: it opens, its hash says so.
        bad = good;
        bad.back() ^= 0x80;
        auto [image, err] = RomImage::open(ByteSpan(bad.data(), bad.size()));
        REQUIRE(err.isOk());
        CHECK(image.verify() == Error::CorruptData);

        // An entry pointing past the image, the header's CRC made right: refused.
        const uint32_t entries = good[16] | good[17] << 8 | good[18] << 16 | uint32_t(good[19]) << 24;
        bad = good;
        put32(bad.data() + entries + RomImage::EntryBytes + 12, 0x7fffffff); // entry 1's size
        CHECK(opens(bad) == Error::CorruptData);
        // The root's children out of order (two names swapped).
        bad = good;
        uint8_t *e1 = bad.data() + entries + RomImage::EntryBytes;
        uint8_t  first[6];
        std::memcpy(first, e1, 6);
        std::memcpy(e1, e1 + RomImage::EntryBytes, 6);
        std::memcpy(e1 + RomImage::EntryBytes, first, 6);
        CHECK(opens(bad) == Error::CorruptData);
        // An entry count that leaves a child out of every directory.
        bad = good;
        put32(bad.data() + 12, (good[12] | good[13] << 8) + 1u);
        fixCrc(bad);
        CHECK(opens(bad) == Error::CorruptData);

        // Random damage to the entries and names: refused or read, never a crash (ASan would say).
        std::mt19937 random(7);
        const size_t tables = entries + 64 * RomImage::EntryBytes;
        for (int i = 0; i < 3000; ++i) {
                bad = good;
                for (int k = 0; k < 4; ++k) bad[entries + random() % (tables - entries)] = uint8_t(random());
                auto [damaged, derr] = RomImage::open(ByteSpan(bad.data(), bad.size()));
                if (derr.isError()) continue;
                for (uint32_t n = 0; n < damaged.count(); ++n) {
                        const RomImage::Entry e = damaged.entry(n);
                        CHECK(e.data.data() + e.data.size() <= bad.data() + bad.size());
                }
                bool noPath = false;
                (void)damaged.find("/docs/deep/nested/leaf.txt", noPath);
        }
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
        CHECK(info.modified.ns == image.buildTime().ns);
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
        CHECK(Vfs::rename("/rom/a.txt", "/rom/c.txt", Root) == Error::ReadOnly);
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
        CHECK(fs.totalBytes == image.bytes().size());
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
                const std::string dir =
                        theme == "glyphs" ? "/thirdparty/glyphs/png/" : "/thirdparty/flat-color-icons/png/";
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
