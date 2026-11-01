/**
 * @file      pkg.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Packages (devplan/packages.md): versions against dpkg's answers, the package and index heads
 * and the data blocks of files made by tools/mkpkg (tests/host/pkg, signed with keys/dev.key),
 * manifests, dependency lists and index stanzas, each fault refused; the dependency walk.
 */

#include <doctest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <bmds/blake2s.h>
#include <bmds/pkgfile.h>
#include <bmds/pkgindex.h>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgsolver.h>
#include <bmds/pkgversion.h>

using bmds::Error;
using bmds::Span;
using bmds::StringView;
namespace PkgFile = bmds::PkgFile;
namespace PkgVersion = bmds::PkgVersion;
namespace PkgManifest = bmds::PkgManifest;
namespace PkgDepends = bmds::PkgDepends;
namespace PkgIndex = bmds::PkgIndex;
namespace PkgSolver = bmds::PkgSolver;
using Verdict = PkgFile::Verdict;

namespace {

        std::vector<uint8_t> load(const std::string &name) {
                std::ifstream file(std::string(BMDS_TEST_PKG_DIR "/") + name, std::ios::binary);
                return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        PkgFile::Key devKey() {
                std::ifstream pub(BMDS_TEST_KEYS_DIR "/dev.pub");
                std::string   kind, hex;
                pub >> kind >> hex;
                PkgFile::Key key;
                REQUIRE(hex.size() == 64);
                for (size_t i = 0; i < 32; ++i) key.bytes[i] = uint8_t(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
                return key;
        }

        StringView view(const std::vector<uint8_t> &v, const PkgFile::Section *s) {
                REQUIRE(s != nullptr);
                return StringView(reinterpret_cast<const char *>(v.data() + s->offset), s->size);
        }

        uint32_t u32(const uint8_t *p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        // Every dependency in @p list as "name>=version;" (or "name;"), or "!" and the error.
        std::string depends(StringView list) {
                PkgDepends::Reader     r(list);
                PkgDepends::Dependency d;
                std::string            out;
                Error                  err;
                while ((err = r.next(d)).isOk()) {
                        out += std::string(d.name.data(), d.name.size());
                        if (!d.minVersion.isEmpty())
                                out += ">=" + std::string(d.minVersion.data(), d.minVersion.size());
                        out += ";";
                }
                return err.code() == Error::EndOfFile ? out : "!" + out;
        }

        StringView sv(const std::string &s) {
                return StringView(s.data(), s.size());
        }

        // The first manifest line of @p text, read: its error.
        Error firstLine(const std::string &text, PkgManifest::Entry &e) {
                PkgManifest::Reader r(sv(text));
                return r.next(e);
        }

        Error firstStanza(const std::string &text, PkgIndex::Stanza &s) {
                PkgIndex::Reader r(sv(text));
                return r.next(s);
        }

        const std::string Hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

} // namespace

TEST_CASE("Package versions: compared as dpkg --compare-versions does") {
        struct Case {
                        const char *a;
                        const char *b;
                        int         expect;
        };
        // dpkg 1.22's answers (Ubuntu 24.04), each pair run through dpkg --compare-versions.
        const Case cases[] = {
                {"1.0", "1.0", 0},
                {"1.0", "1.0-0", 0},
                {"1.0", "1.0-1", -1},
                {"1.0-1", "1.0-2", -1},
                {"1.0", "1.1", -1},
                {"1.2", "1.10", -1},
                {"1.10", "1.9", 1},
                {"1.0~rc1", "1.0", -1},
                {"1.0~rc1", "1.0~rc2", -1},
                {"1.0~~", "1.0~", -1},
                {"1.0~", "1.0", -1},
                {"1.0", "1.0+1", -1},
                {"1.0+1", "1.0.1", -1},
                {"1.0a", "1.0", 1},
                {"1.0a", "1.0b", -1},
                {"1.0a", "1.0+", -1},
                {"1.0.", "1.0+", 1},
                {"1:1.0", "2.0", 1},
                {"1:1.0", "0:9.9", 1},
                {"0:1.0", "1.0", 0},
                {"2:0.1", "1:9.9", 1},
                {"1.37.0-1", "1.37.0-1", 0},
                {"1.37.0-1", "1.37.0-10", -1},
                {"1.37.0-9", "1.37.0-10", -1},
                {"001.0", "1.0", 0},
                {"1.00", "1.0", 0},
                {"1.0-001", "1.0-1", 0},
                {"2.0~rc1-3", "2.0-1", -1},
                {"2.0~rc1-3", "2.0~rc1-3", 0},
                {"1.0-1~bpo1", "1.0-1", -1},
                {"1.0-1+b1", "1.0-1", 1},
                {"1.0a~", "1.0a", -1},
                {"1.2.3", "1.2.3.0", -1},
                {"1.2.3.0", "1.2.3.0.0", -1},
                {"9", "10", -1},
                {"1.0-a", "1.0-B", 1},
                {"1.0-Z", "1.0-a", -1},
                {"2025.88", "2024.86", 1},
                {"0.1.0-1", "0.1.0", 1},
        };
        const auto sign = [](int x) {
                return x < 0 ? -1 : x > 0 ? 1 : 0;
        };
        for (const Case &c : cases) {
                CAPTURE(c.a);
                CAPTURE(c.b);
                REQUIRE(PkgVersion::isValid(c.a));
                REQUIRE(PkgVersion::isValid(c.b));
                CHECK(sign(PkgVersion::compare(c.a, c.b)) == c.expect);
                CHECK(sign(PkgVersion::compare(c.b, c.a)) == -c.expect);
        }
}

TEST_CASE("Package versions: what is one") {
        for (const char *good : {"1", "0.1.0", "1.37.0-1", "2025.88", "1:2.0~rc1-3", "1.0+git20251030-2", "1.0-1-2",
                                 "12345678:1", "1.0-a.b+c~d"}) {
                CAPTURE(good);
                CHECK(PkgVersion::isValid(good));
        }
        for (const char *bad : {"", "a1", "-1", "1.0-", ":1.0", "x:1.0", "123456789:1", "1:", "1.0 2", "1.0_1",
                                "1.0-1:2", "1.0/1", "1.0-1-", "1.0\t", "123456789012345678901234567890123"}) {
                CAPTURE(bad);
                CHECK_FALSE(PkgVersion::isValid(bad));
        }
}

TEST_CASE("Packages: one made by tools/mkpkg, its every file decoded") {
        const std::vector<uint8_t> v = load("hello_2.0~rc1-3.bmpk");
        REQUIRE(v.size() > PkgFile::PackageHeaderBytes);
        PkgFile::Package p;
        REQUIRE(PkgFile::parsePackage(Span<const uint8_t>(v.data(), v.size()), v.size(), p) == Verdict::Ok);
        CHECK(std::string(p.name) == "hello");
        CHECK(std::string(p.version) == "1:2.0~rc1-3");
        CHECK(std::string(p.arch) == "esp32p4");
        CHECK(p.abiVersion == 1);
        CHECK(p.installedBytes == 21 + 85536);
        CHECK(p.sectionCount == 4);
        const PkgFile::Key key = devKey();
        size_t             index = 9;
        CHECK(bmds::SignedContainer::verify(p, Span<const uint8_t>(v.data(), p.headBytes),
                                            Span<const PkgFile::Key>(&key, 1), index) == Verdict::Ok);
        CHECK(index == 0);
        CHECK(view(v, p.find(PkgFile::SectionType::Description)) == StringView("Says hello: the host tests' package."));

        // The manifest, line by line; each file's bytes decoded from the data blocks and hashed.
        PkgManifest::Reader     manifest(view(v, p.find(PkgFile::SectionType::Manifest)));
        const PkgFile::Section *data = p.find(PkgFile::SectionType::Data);
        REQUIRE(data != nullptr);
        size_t                   at = data->offset;
        std::vector<std::string> seen;
        PkgManifest::Entry       e;
        Error                    err;
        size_t                   stored = 0, compressed = 0;
        std::vector<uint8_t>     out(PkgFile::BlockBytes);
        while ((err = manifest.next(e)).isOk()) {
                switch (e.kind) {
                        case PkgManifest::Kind::Field:
                                seen.push_back(std::string(e.key.data(), e.key.size()) + "=" +
                                               std::string(e.value.data(), e.value.size()));
                                break;
                        case PkgManifest::Kind::Directory:
                                seen.push_back("d " + std::string(e.path.data(), e.path.size()));
                                break;
                        case PkgManifest::Kind::Link:
                                seen.push_back("l " + std::string(e.path.data(), e.path.size()) + " " +
                                               std::string(e.target.data(), e.target.size()));
                                break;
                        case PkgManifest::Kind::File: {
                                seen.push_back("f " + std::string(e.path.data(), e.path.size()) + " " +
                                               std::to_string(e.mode) + " " + std::to_string(e.size));
                                bmds::Blake2s h;
                                uint64_t      left = e.size;
                                while (left > 0) {
                                        const size_t want =
                                                left < PkgFile::BlockBytes ? size_t(left) : PkgFile::BlockBytes;
                                        PkgFile::Block b;
                                        REQUIRE(at + 4 <= data->offset + data->size);
                                        REQUIRE(PkgFile::readBlock(u32(v.data() + at), want, b).isOk());
                                        (b.stored ? stored : compressed) += 1;
                                        REQUIRE(at + 4 + b.bytes <= data->offset + data->size);
                                        REQUIRE(PkgFile::decodeBlock(b, bmds::ByteSpan(v.data() + at + 4, b.bytes),
                                                                     bmds::WritableByteSpan(out.data(), want))
                                                        .isOk());
                                        h.update(out.data(), want);
                                        at += 4 + b.bytes;
                                        left -= want;
                                }
                                uint8_t digest[32];
                                h.final(digest);
                                CHECK(std::memcmp(digest, e.hash, 32) == 0);
                                break;
                        }
                }
        }
        CHECK(err.code() == Error::EndOfFile);
        CHECK(at == data->offset + data->size);
        CHECK(stored == 2);     // bin/hello (too small to shrink) and big.bin's random first 64 KiB
        CHECK(compressed == 1); // big.bin's repeated rest
        const std::vector<std::string> expect = {"depends=busybox (>= 1.37.0-1), tzdata",
                                                 "title=Hello, World",
                                                 "summary=Says hello",
                                                 "license=MIT",
                                                 "category=Utility",
                                                 "homepage=https://example.org/hello",
                                                 "d bin",
                                                 "d share",
                                                 "f bin/hello 493 21",
                                                 "l bin/hi hello",
                                                 "d share/hello",
                                                 "f share/hello/big.bin 420 85536",
                                                 "f share/hello/empty 420 0"};
        CHECK(seen == expect);
}

TEST_CASE("Packages: a meta package, and an index made by tools/mkpkg") {
        const std::vector<uint8_t> meta = load("bmds-standard_0.1.0-1.bmpk");
        PkgFile::Package           p;
        REQUIRE(PkgFile::parsePackage(Span<const uint8_t>(meta.data(), meta.size()), meta.size(), p) == Verdict::Ok);
        CHECK(std::string(p.arch) == PkgFile::ArchAll);
        CHECK(p.installedBytes == 0);
        CHECK(view(meta, p.find(PkgFile::SectionType::Manifest)) ==
              StringView("depends\thello\ntitle\tBMDS standard set\n"));
        CHECK(p.find(PkgFile::SectionType::Icons) == nullptr);
        CHECK(p.find(PkgFile::SectionType::Data)->size == 0);

        const std::vector<uint8_t> v = load("index.bmpi");
        PkgFile::Index             index;
        REQUIRE(PkgFile::parseIndex(Span<const uint8_t>(v.data(), v.size()), v.size(), index) == Verdict::Ok);
        CHECK(index.packageCount == 2);
        CHECK(std::string(index.label) == "host tests");
        const PkgFile::Key key = devKey();
        size_t             which = 9;
        CHECK(bmds::SignedContainer::verify(index, Span<const uint8_t>(v.data(), index.headBytes),
                                            Span<const PkgFile::Key>(&key, 1), which) == Verdict::Ok);

        // The stanzas name the packages beside it, with their sizes and hashes.
        PkgIndex::Reader r(view(v, index.find(PkgFile::IndexSectionType::Stanzas)));
        PkgIndex::Stanza s;
        REQUIRE(r.next(s).isOk());
        CHECK(s.package == StringView("bmds-standard"));
        CHECK(s.architecture == StringView("all"));
        CHECK(s.title == StringView("BMDS standard set"));
        CHECK(s.category.isEmpty());
        CHECK(s.size == meta.size());
        CHECK(s.file == StringView("bmds-standard_0.1.0-1.bmpk"));
        REQUIRE(r.next(s).isOk());
        CHECK(s.package == StringView("hello"));
        CHECK(s.version == StringView("1:2.0~rc1-3"));
        CHECK(s.abi == 1);
        CHECK(s.depends == StringView("busybox (>= 1.37.0-1), tzdata"));
        CHECK(s.installedSize == 21 + 85536);
        CHECK(s.description == StringView("Says hello: the host tests' package."));
        CHECK(s.title == StringView("Hello, World"));
        CHECK(s.summary == StringView("Says hello"));
        CHECK(s.category == StringView("Utility"));
        CHECK(s.license == StringView("MIT"));
        CHECK(s.homepage == StringView("https://example.org/hello"));
        const std::vector<uint8_t> hello = load("hello_2.0~rc1-3.bmpk");
        CHECK(s.size == hello.size());
        bmds::Blake2s h;
        h.update(hello.data(), hello.size());
        uint8_t digest[32];
        h.final(digest);
        CHECK(std::memcmp(digest, s.hash, 32) == 0);
        CHECK(r.next(s).code() == Error::EndOfFile);

        // The index's icons: hello's two, named.
        const PkgFile::Section *icons = index.find(PkgFile::IndexSectionType::Icons);
        REQUIRE(icons != nullptr);
        PkgFile::IconReader icon(bmds::ByteSpan(v.data() + icons->offset, icons->size), true);
        PkgFile::Icon       i;
        REQUIRE(icon.next(i).isOk());
        CHECK(i.name == StringView("hello"));
        CHECK(i.side == 16);
        REQUIRE(icon.next(i).isOk());
        CHECK(i.side == 32);
        CHECK(icon.next(i).code() == Error::EndOfFile);
}

TEST_CASE("Packages: icons read, and what is refused") {
        const std::vector<uint8_t> v = load("hello_2.0~rc1-3.bmpk");
        PkgFile::Package           p;
        REQUIRE(PkgFile::parsePackage(Span<const uint8_t>(v.data(), v.size()), v.size(), p) == Verdict::Ok);
        const PkgFile::Section *s = p.find(PkgFile::SectionType::Icons);
        REQUIRE(s != nullptr);
        const std::vector<uint8_t> section(v.begin() + s->offset, v.begin() + s->offset + s->size);
        PkgFile::IconReader        own(bmds::ByteSpan(section.data(), section.size()), false);
        PkgFile::Icon              i;
        REQUIRE(own.next(i).isOk());
        CHECK(i.name.isEmpty());
        CHECK(i.side == 16);
        CHECK(i.png.size() > 24);
        CHECK(i.png[1] == 'P');
        REQUIRE(own.next(i).isOk());
        CHECK(i.side == 32);
        CHECK(own.next(i).code() == Error::EndOfFile);

        // A package's icons have no names; an index's do.
        PkgFile::IconReader named(bmds::ByteSpan(section.data(), section.size()), true);
        CHECK(named.next(i).code() == Error::CorruptData);

        const auto first = [&](const std::vector<uint8_t> &w, bool isNamed) {
                PkgFile::IconReader r(bmds::ByteSpan(w.data(), w.size()), isNamed);
                return r.next(i).code();
        };
        std::vector<uint8_t> w = section;
        w[1] = 17; // the width isn't the PNG's
        w[3] = 17;
        CHECK(first(w, false) == Error::CorruptData);
        w = section;
        w[3] = 32; // not square
        CHECK(first(w, false) == Error::CorruptData);
        w = section;
        w[1] = 65; // too big
        w[3] = 65;
        CHECK(first(w, false) == Error::CorruptData);
        w = section;
        w[9 + 1] = 'X'; // not a PNG
        CHECK(first(w, false) == Error::CorruptData);
        w = section;
        w[5] = uint8_t(section.size()); // longer than what is left of the section (and under 16 KB)
        w[6] = uint8_t(section.size() >> 8);
        CHECK(first(w, false) == Error::CorruptData);
        w.assign(section.begin(), section.begin() + 5);
        CHECK(first(w, false) == Error::CorruptData);
        w = section;
        w.insert(w.begin() + 1, {'H', 'i'}); // a name that isn't a package's
        w[0] = 2;
        CHECK(first(w, true) == Error::CorruptData);
        w[1] = 'h';
        CHECK(first(w, true) == Error::Ok);
        CHECK(i.name == StringView("hi"));
}

TEST_CASE("Packages: an unsigned index written, and read back") {
        // The test package's icons, named, in an index of two stanzas.
        const std::vector<uint8_t> v = load("hello_2.0~rc1-3.bmpk");
        PkgFile::Package           p;
        REQUIRE(PkgFile::parsePackage(Span<const uint8_t>(v.data(), v.size()), v.size(), p) == Verdict::Ok);
        const PkgFile::Section *s = p.find(PkgFile::SectionType::Icons);
        REQUIRE(s != nullptr);
        PkgFile::IconReader own(bmds::ByteSpan(v.data() + s->offset, s->size), false);
        PkgFile::Icon       i;
        bmds::List<uint8_t> icons;
        while (own.next(i).isOk()) REQUIRE(PkgFile::appendIcon(icons, "hello", i.side, i.png).isOk());
        const std::string stanzas =
                "Package: hello\nVersion: 1\nArchitecture: all\nABI: 1\nSize: 5\nBLAKE2s: " + std::string(64, '0') +
                "\nFile: hello_1.bmpk\n\nPackage: other\nVersion: 2\n"
                "Architecture: esp32p4\nABI: 1\nSize: 7\nBLAKE2s: " +
                std::string(64, 'a') + "\nFile: other_2.bmpk\n";
        bmds::List<uint8_t> out;
        REQUIRE(PkgFile::buildIndex(StringView(stanzas.data(), stanzas.size()), icons.span(), 2, 1700000000, "a card",
                                    out)
                        .isOk());

        PkgFile::Index index;
        REQUIRE(PkgFile::parseIndex(out.span(), out.size(), index) == Verdict::Ok);
        CHECK(bmds::SignedContainer::isUnsigned(index));
        CHECK(index.packageCount == 2);
        CHECK(index.madeAt == 1700000000);
        CHECK(StringView(index.label) == StringView("a card"));
        size_t                     which = 0;
        bmds::SignedContainer::Key none;
        CHECK(bmds::SignedContainer::verify(index, out.span(), Span<const bmds::SignedContainer::Key>(&none, 1),
                                            which) == Verdict::BadSignature);
        for (size_t k = 0; k < index.sectionCount; ++k) {
                const auto   &sec = index.sections[k];
                bmds::Blake2s h;
                h.update(out.data() + sec.offset, sec.size);
                uint8_t d[32];
                h.final(d);
                CHECK(std::memcmp(d, sec.hash, 32) == 0);
        }
        const PkgFile::Section *text = index.find(PkgFile::IndexSectionType::Stanzas);
        REQUIRE(text != nullptr);
        PkgIndex::Reader r(StringView(reinterpret_cast<const char *>(out.data() + text->offset), text->size));
        PkgIndex::Stanza st;
        REQUIRE(r.next(st).isOk());
        CHECK(st.package == StringView("hello"));
        REQUIRE(r.next(st).isOk());
        CHECK(st.package == StringView("other"));
        CHECK(r.next(st).code() == Error::EndOfFile);
        const PkgFile::Section *named = index.find(PkgFile::IndexSectionType::Icons);
        REQUIRE(named != nullptr);
        PkgFile::IconReader ir(bmds::ByteSpan(out.data() + named->offset, named->size), true);
        REQUIRE(ir.next(i).isOk());
        CHECK(i.name == StringView("hello"));
        CHECK(i.side == 16);
        REQUIRE(ir.next(i).isOk());
        CHECK(i.side == 32);
        CHECK(ir.next(i).code() == Error::EndOfFile);

        // A signed file isn't unsigned; no icons, no section; what is refused.
        CHECK_FALSE(bmds::SignedContainer::isUnsigned(p));
        REQUIRE(PkgFile::buildIndex("", bmds::ByteSpan(), 0, 0, "", out).isOk());
        REQUIRE(PkgFile::parseIndex(out.span(), out.size(), index) == Verdict::Ok);
        CHECK(index.sectionCount == 1);
        CHECK(PkgFile::buildIndex("", bmds::ByteSpan(), 0, 0, "tab\there", out).code() == Error::Invalid);
        CHECK(PkgFile::buildIndex("", bmds::ByteSpan(), 0, 0, std::string(33, 'x').c_str(), out).code() ==
              Error::Invalid);
        CHECK(PkgFile::appendIcon(icons, "x", 65, i.png).code() == Error::Invalid);
        CHECK(PkgFile::appendIcon(icons, std::string(256, 'x').c_str(), 16, i.png).code() == Error::Invalid);
}

TEST_CASE("Packages: a head's faults refused") {
        const std::vector<uint8_t> v = load("hello_2.0~rc1-3.bmpk");
        PkgFile::Package           p;
        const auto                 parse = [&](const std::vector<uint8_t> &w) {
                return PkgFile::parsePackage(Span<const uint8_t>(w.data(), w.size()), w.size(), p);
        };
        REQUIRE(parse(v) == Verdict::Ok);
        std::vector<uint8_t> w = v;
        std::memcpy(w.data(), "BMUP", 4); // an update package isn't a package
        CHECK(parse(w) == Verdict::NotAPackage);
        w = v;
        w[24] = 'H'; // the name: uppercase
        CHECK(parse(w) == Verdict::Malformed);
        w = v;
        w[56] = 'x'; // the version: not starting with a digit
        CHECK(parse(w) == Verdict::Malformed);
        w = v;
        w[88] = '-'; // the architecture
        CHECK(parse(w) == Verdict::Malformed);
        w = v;
        w[108] = 1; // the reserved u32
        CHECK(parse(w) == Verdict::Malformed);
        w = v;
        w[24 + 5] = 0; // NUL inside the name, text after it
        w[24 + 6] = 'x';
        CHECK(parse(w) == Verdict::Malformed);
        w.assign(v.begin(), v.end() - 1);
        CHECK(parse(w) == Verdict::Truncated);
        for (int i = 0; i <= int(Verdict::BadHash); ++i) CHECK(std::strlen(PkgFile::verdictText(Verdict(i))) > 0);

        PkgFile::Index index;
        CHECK(PkgFile::parseIndex(Span<const uint8_t>(v.data(), v.size()), v.size(), index) == Verdict::NotAPackage);
}

TEST_CASE("Packages: names, architectures and data blocks") {
        for (const char *good :
             {"busybox", "soundfont-timgm6mb", "g++", "lib.x", "0ad", "abcdefghijklmnopqrstuvwxyz012345"})
                CHECK(PkgFile::isValidName(good));
        for (const char *bad : {"", "-x", ".x", "Busybox", "a_b", "a b", "a/b", "abcdefghijklmnopqrstuvwxyz0123456"})
                CHECK_FALSE(PkgFile::isValidName(bad));
        CHECK(PkgFile::isValidArch("esp32p4"));
        CHECK(PkgFile::isValidArch("all"));
        CHECK_FALSE(PkgFile::isValidArch("esp32-p4"));
        CHECK_FALSE(PkgFile::isValidArch(""));

        PkgFile::Block b;
        CHECK(PkgFile::readBlock(PkgFile::BlockStored | 100, 100, b).isOk());
        CHECK(b.stored);
        CHECK(b.bytes == 100);
        CHECK(PkgFile::readBlock(PkgFile::BlockStored | 99, 100, b).code() == Error::CorruptData);
        CHECK(PkgFile::readBlock(10, 100, b).isOk());
        CHECK_FALSE(b.stored);
        CHECK(PkgFile::readBlock(0, 100, b).code() == Error::CorruptData);
        CHECK(PkgFile::readBlock(PkgFile::BlockBytes + 1, 100, b).code() == Error::CorruptData);
        CHECK(PkgFile::readBlock(10, 0, b).code() == Error::Invalid);
        CHECK(PkgFile::readBlock(10, PkgFile::BlockBytes + 1, b).code() == Error::Invalid);

        // Raw DEFLATE of "hello hello hello\n" (zlib, level 9, no header).
        const uint8_t deflated[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x5c, 0x00};
        uint8_t       out[18];
        b = PkgFile::Block{sizeof(deflated), false};
        REQUIRE(PkgFile::decodeBlock(b, bmds::ByteSpan(deflated, sizeof(deflated)), bmds::WritableByteSpan(out, 18))
                        .isOk());
        CHECK(std::memcmp(out, "hello hello hello\n", 18) == 0);
        uint8_t shortOut[17];
        CHECK(PkgFile::decodeBlock(b, bmds::ByteSpan(deflated, sizeof(deflated)), bmds::WritableByteSpan(shortOut, 17))
                      .code() == Error::CorruptData); // more than the block's size
        uint8_t longOut[19];
        CHECK(PkgFile::decodeBlock(b, bmds::ByteSpan(deflated, sizeof(deflated)), bmds::WritableByteSpan(longOut, 19))
                      .code() == Error::CorruptData); // less
        const uint8_t junk[] = {0xff, 0xff, 0xff};
        b = PkgFile::Block{sizeof(junk), false};
        CHECK(PkgFile::decodeBlock(b, bmds::ByteSpan(junk, sizeof(junk)), bmds::WritableByteSpan(out, 18)).code() ==
              Error::CorruptData);
        b = PkgFile::Block{3, true};
        CHECK(PkgFile::decodeBlock(b, bmds::ByteSpan(junk, 3), bmds::WritableByteSpan(out, 3)).isOk());
        CHECK(PkgFile::decodeBlock(b, bmds::ByteSpan(junk, 3), bmds::WritableByteSpan(out, 4)).code() ==
              Error::CorruptData);
}

TEST_CASE("Package manifests: paths, lines, and what is refused") {
        for (const char *good : {"bin", "bin/busybox", "share/licenses/busybox/LICENSE", "Grüße aus Zürich.txt",
                                 "a b/c d", ".hidden", "a..b", "x\xef\xbf\xbdy"}) {
                CAPTURE(good);
                CHECK(PkgManifest::isValidPath(good));
        }
        for (const char *bad : {"",
                                "/bin",
                                "bin/",
                                "bin//x",
                                ".",
                                "..",
                                "a/../b",
                                "a/./b",
                                "a\\b",
                                "a:b",
                                "a*b",
                                "a?b",
                                "a\"b",
                                "a<b",
                                "a>b",
                                "a|b",
                                "a\tb",
                                "a\nb",
                                "a\x7f"
                                "b",
                                "a\xc2\x85"
                                "b",
                                "a\xff"
                                "b",
                                "a\xc0\xaf"
                                "b"}) {
                CAPTURE(bad);
                CHECK_FALSE(PkgManifest::isValidPath(bad));
        }
        CHECK(PkgManifest::isValidPath(sv(std::string(255, 'n'))));
        CHECK_FALSE(PkgManifest::isValidPath(sv(std::string(256, 'n'))));
        CHECK_FALSE(PkgManifest::isValidPath(sv(std::string(255, 'n') + "/" + std::string(256, 'n'))));
        CHECK_FALSE(PkgManifest::isValidPath(sv(std::string(255, 'n') + "/" + std::string(255, 'n') + "/x"))); // > 512

        PkgManifest::Entry e;
        REQUIRE(firstLine("f\tbin/x\t755\t12\t" + Hash + "\n", e).isOk());
        CHECK(e.kind == PkgManifest::Kind::File);
        CHECK(e.mode == 0755);
        CHECK(e.size == 12);
        CHECK(e.hash[0] == 0x01);
        CHECK(e.hash[31] == 0xef);
        const std::string link = "l\tbin/awk\tbusybox"; // no newline at the end
        REQUIRE(firstLine(link, e).isOk());
        CHECK(e.kind == PkgManifest::Kind::Link);
        CHECK(e.target == StringView("busybox"));
        REQUIRE(firstLine("d\tshare\n", e).isOk());
        CHECK(e.kind == PkgManifest::Kind::Directory);
        REQUIRE(firstLine("depends\t\n", e).isOk());
        CHECK(e.kind == PkgManifest::Kind::Field);
        CHECK(e.value.isEmpty());
        REQUIRE(firstLine("some-new-field\twhatever it says\n", e).isOk()); // skipped by its user
        CHECK(firstLine("x\tsomething new\n", e).code() == Error::NotSupported);

        for (const std::string &bad : {
                     std::string("\n"),
                     std::string("f\tbin/x\t755\t12\n"),             // no hash
                     "f\tbin/x\t755\t12\t" + Hash + "\textra\n",     // a column too many
                     "f\tbin/x\t758\t12\t" + Hash + "\n",            // not octal
                     "f\tbin/x\t17777\t12\t" + Hash + "\n",          // more than 7777
                     "f\tbin/x\t755\t012\t" + Hash + "\n",           // a leading zero
                     "f\tbin/x\t755\t-1\t" + Hash + "\n",            // a sign
                     "f\tbin/x\t755\t12\t" + Hash.substr(1) + "\n",  // a short hash
                     "f\tbin/x\t755\t12\t" + Hash.substr(1) + "A\n", // uppercase hex
                     "f\t../x\t755\t12\t" + Hash + "\n",             // outside the root
                     std::string("d\tshare\textra\n"),
                     std::string("d\t/share\n"),
                     std::string("l\tbin/awk\n"),    // no target
                     std::string("l\tbin/awk\t\n"),  // an empty target
                     std::string("depends\n"),       // a field without a value
                     std::string("depends\ta\tb\n"), // a tab in a value
                     std::string("D\tshare\n"),      // not lowercase
                     std::string("dir\x01\tx\n"),
                     std::string("f\tbin/x\t755\t12\t") + Hash + "\r\n", // a CR
             }) {
                CAPTURE(bad);
                CHECK(firstLine(bad, e).code() == Error::CorruptData);
        }

        // Lines counted; the end after the last.
        PkgManifest::Reader r("d\ta\nd\tb\nd\t/c\n");
        CHECK(r.next(e).isOk());
        CHECK(r.next(e).isOk());
        CHECK(r.next(e).code() == Error::CorruptData);
        CHECK(r.line() == 3);
        PkgManifest::Reader end("d\ta\n");
        CHECK(end.next(e).isOk());
        CHECK(end.next(e).code() == Error::EndOfFile);
        CHECK(end.next(e).code() == Error::EndOfFile);
}

TEST_CASE("Package dependencies: lists read, and what is refused") {
        CHECK(depends("") == "");
        CHECK(depends("   ") == "");
        CHECK(depends("busybox") == "busybox;");
        CHECK(depends("busybox, tzdata") == "busybox;tzdata;");
        CHECK(depends("busybox (>= 1.37.0-1), tzdata") == "busybox>=1.37.0-1;tzdata;");
        CHECK(depends(" a(>=1) ,b ( >= 2:1.0~rc1 ) ") == "a>=1;b>=2:1.0~rc1;");
        for (const char *bad : {",", "a,", "a,,b", "a b", "A", "a (> 1)", "a (<= 1)", "a (= 1)", "a (>= )", "a (>= x1)",
                                "a (>= 1", "a >= 1", "a (>= 1) b", "(>= 1)"}) {
                CAPTURE(bad);
                CHECK(depends(bad)[0] == '!');
        }
}

TEST_CASE("Package indexes: stanzas read, and what is refused") {
        const std::string stanza = "Package: busybox\n"
                                   "Version: 1.37.0-1\n"
                                   "Architecture: esp32p4\n"
                                   "ABI: 1\n"
                                   "Size: 764844\n"
                                   "BLAKE2s: " +
                                   Hash + "\nFile: https://example.org/busybox_1.37.0-1.bmpk\n";
        PkgIndex::Stanza s;
        REQUIRE(firstStanza(stanza, s).isOk());
        CHECK(s.package == StringView("busybox"));
        CHECK(s.size == 764844);
        CHECK(s.installedSize == 0);
        CHECK(s.depends.isEmpty());
        CHECK(s.file == StringView("https://example.org/busybox_1.37.0-1.bmpk"));

        // Two stanzas with extra empty lines, an unknown field, and spaces around values.
        const std::string two =
                "\n\n" + stanza + "X-New-Field: skipped\n\n\n" + stanza + "Description:   Unix tools  \n";
        PkgIndex::Reader r(sv(two));
        REQUIRE(r.next(s).isOk());
        REQUIRE(r.next(s).isOk());
        CHECK(s.description == StringView("Unix tools"));
        CHECK(r.next(s).code() == Error::EndOfFile);

        // Each required field missing; a field twice; bad values.
        for (const char *field : {"Package", "Version", "Architecture", "ABI", "Size", "BLAKE2s", "File"}) {
                CAPTURE(field);
                std::string  without = stanza;
                const size_t at = without.find(std::string(field) + ":");
                REQUIRE(at != std::string::npos);
                without.erase(at, without.find('\n', at) - at + 1);
                CHECK(firstStanza(without, s).code() == Error::CorruptData);
        }
        for (const std::string &extra :
             {std::string("Package: again\n"), std::string("Depends: a,\n"), std::string("Depends: A\n"),
              std::string("Installed-Size: x\n"), std::string("Category: Not A Name\n"), std::string("Homepage: a b\n"),
              std::string("Title: \n"), std::string("no colon here\n"), std::string("Bad Key: x\n"),
              std::string("Description: a\x01"
                          "b\n")}) {
                CAPTURE(extra);
                CHECK(firstStanza(stanza + extra, s).code() == Error::CorruptData);
        }
        std::string bad = stanza;
        bad.replace(bad.find("ABI: 1"), 6, "ABI: 4294967296");
        CHECK(firstStanza(bad, s).code() == Error::CorruptData);
        bad = stanza;
        bad.replace(bad.find("File: "), 6, "File: a b");
        CHECK(firstStanza(bad, s).code() == Error::CorruptData);
        bad = stanza;
        bad.replace(bad.find("Version: 1.37.0-1"), 17, "Version: one");
        CHECK(firstStanza(bad, s).code() == Error::CorruptData);
}

namespace {

        using Pkgs = std::vector<PkgSolver::Package>;

        // The plan as "a b* c" (* automatic), or "error: WHY".
        std::string planOf(const Pkgs &installed, const Pkgs &candidates, const std::vector<size_t> &requested,
                           std::string *wanted = nullptr) {
                bmds::List<PkgSolver::Step> steps;
                bmds::String                why;
                StringView                  need;
                const Error                 err =
                        PkgSolver::plan(Span<const PkgSolver::Package>(installed.data(), installed.size()),
                                        Span<const PkgSolver::Package>(candidates.data(), candidates.size()),
                                        Span<const size_t>(requested.data(), requested.size()), steps, why, &need);
                if (wanted != nullptr) *wanted = std::string(need.data(), need.size());
                if (err.isError()) return std::string(err.name()) + ": " + why.cStr();
                std::string out;
                for (const PkgSolver::Step &s : steps) {
                        const StringView n = candidates[s.candidate].name;
                        out += (out.empty() ? "" : " ") + std::string(n.data(), n.size()) + (s.automatic ? "*" : "");
                }
                return out;
        }

        std::string unneededOf(const Pkgs &installed, const std::vector<StringView> &removing) {
                bmds::List<size_t> out;
                REQUIRE(PkgSolver::unneeded(Span<const PkgSolver::Package>(installed.data(), installed.size()),
                                            Span<const StringView>(removing.data(), removing.size()), out)
                                .isOk());
                std::string s;
                for (size_t i : out)
                        s += (s.empty() ? "" : " ") + std::string(installed[i].name.data(), installed[i].name.size());
                return s;
        }

} // namespace

TEST_CASE("Package dependencies: the walk, in order, automatic marked") {
        const Pkgs none;
        // A meta package: its members after their own dependencies, each automatic.
        const Pkgs set = {{"bmds-standard", "1-1", "doom, busybox"},
                          {"doom", "1-1", "freedoom (>= 0.13-1)"},
                          {"freedoom", "0.12-1", ""},
                          {"freedoom", "0.13-1", ""},
                          {"busybox", "1.37.0-1", ""}};
        CHECK(planOf(none, set, {0}) == "freedoom* doom* busybox* bmds-standard");
        // Asked for, not automatic, though also needed.
        CHECK(planOf(none, set, {0, 4}) == "freedoom* doom* busybox bmds-standard");
        CHECK(planOf(none, set, {4, 0}) == "busybox freedoom* doom* bmds-standard");

        // What is installed and suits stays: nothing is upgraded unasked.
        const Pkgs have = {{"freedoom", "0.13-1", "", true}, {"busybox", "1.36-1", "", false}};
        CHECK(planOf(have, set, {0}) == "doom* bmds-standard");
        // Too old installed: the newest candidate that suits.
        const Pkgs old = {{"freedoom", "0.12-1", "", false}};
        CHECK(planOf(old, set, {1}) == "freedoom* doom");

        // Nothing provides it: said with the chain, and named for the caller to look further.
        std::string wanted;
        const Pkgs  noData = {{"bmds-standard", "1-1", "doom"}, {"doom", "1-1", "freedoom (>= 0.13-1)"}};
        CHECK(planOf(none, noData, {0}, &wanted) ==
              "NotFound: freedoom (>= 0.13-1), needed by bmds-standard -> doom, isn't installed or given");
        CHECK(wanted == "freedoom");
        CHECK(planOf(old, noData, {1}) ==
              "NotFound: freedoom (>= 0.13-1), needed by doom, is installed in an older version and not given");
}

TEST_CASE("Package dependencies: what the walk refuses") {
        const Pkgs none;
        const Pkgs cycle = {{"a", "1", "b"}, {"b", "1", "c"}, {"c", "1", "a"}};
        CHECK(planOf(none, cycle, {0}) == "InvalidState: a dependency cycle: a -> b -> c -> a");
        const Pkgs self = {{"a", "1", "a"}};
        CHECK(planOf(none, self, {0}) == "InvalidState: a dependency cycle: a -> a");
        // A cycle through what is installed is no cycle: nothing is walked there.
        const Pkgs haveA = {{"a", "1", "b"}};
        CHECK(planOf(haveA, {{"b", "1", "a"}}, {0}) == "b");

        const Pkgs twice = {{"a", "1", ""}, {"a", "2", ""}};
        CHECK(planOf(none, twice, {0, 1}) == "InvalidState: a is given twice (1 and 2)");
        CHECK(planOf(none, twice, {1, 1}) == "a"); // the same file twice is once

        // A given version too old for what needs it.
        const Pkgs given = {{"doom", "1", "freedoom (>= 2)"}, {"freedoom", "1", ""}};
        CHECK(planOf(none, given, {0, 1}) == "InvalidState: doom needs freedoom (>= 2); 1 is given");

        // A downgrade below what an installed package needs.
        const Pkgs installed = {{"doom", "1", "freedoom (>= 2)"}, {"freedoom", "2", "", true}};
        CHECK(planOf(installed, {{"freedoom", "1", ""}}, {0}) ==
              "InvalidState: installing freedoom 1 would break doom, which needs freedoom (>= 2)");
        CHECK(planOf(installed, {{"freedoom", "3", ""}}, {0}) == "freedoom");

        const Pkgs damaged = {{"a", "1", "b (= 1)"}};
        CHECK(planOf(none, damaged, {0}) == "CorruptData: a: its dependencies are damaged");

        // A long chain walks on the walk's own stack.
        std::vector<std::string> names(5000), deps(5000);
        Pkgs                     chain;
        for (size_t i = 0; i < names.size(); ++i) names[i] = "p" + std::to_string(i);
        for (size_t i = 0; i < names.size(); ++i) {
                deps[i] = i + 1 < names.size() ? names[i + 1] : "";
                chain.push_back({StringView(names[i].data(), names[i].size()), "1",
                                 StringView(deps[i].data(), deps[i].size())});
        }
        const std::string plan = planOf(none, chain, {0});
        CHECK(plan.rfind("p4999* p4998*", 0) == 0);
        CHECK(plan.size() > 5000);
}

TEST_CASE("Package dependencies: what nothing needs any more") {
        const Pkgs installed = {{"bmds-standard", "1", "doom, busybox", false},
                                {"doom", "1", "freedoom", true},
                                {"freedoom", "1", "", true},
                                {"busybox", "1", "", false},
                                {"orphan", "1", "", true}};
        CHECK(unneededOf(installed, {}) == "orphan");
        CHECK(unneededOf(installed, {"bmds-standard"}) == "doom freedoom orphan"); // busybox was asked for
        CHECK(unneededOf(installed, {"doom"}) == "freedoom orphan");
        CHECK(unneededOf(installed, {"bmds-standard", "doom", "freedoom", "orphan"}).empty());
}

TEST_CASE("Package keys: key lines read, and what is refused") {
        using bmds::SignedContainer::Key;
        const std::string hex = "df057b819666fb2822e3153af76dbf72d9fbd4a9087eb9ff02251bc2708ab09b";
        Key               k;
        StringView        comment;
        const std::string line = "ed25519 " + hex + "  dev key: anyone can sign with it  \n";
        REQUIRE(bmds::SignedContainer::parseKey(StringView(line.data(), line.size()), k, comment).isOk());
        CHECK(k.bytes[0] == 0xdf);
        CHECK(k.bytes[31] == 0x9b);
        CHECK(comment == StringView("dev key: anyone can sign with it"));
        std::string upper = "ed25519 " + hex;
        for (char &c : upper) c = char(c >= 'a' && c <= 'f' ? c - 32 : c);
        upper.replace(0, 7, "ed25519");
        REQUIRE(bmds::SignedContainer::parseKey(StringView(upper.data(), upper.size()), k, comment).isOk());
        CHECK(k.bytes[1] == 0x05);
        CHECK(comment.isEmpty());

        const auto refused = [&](const std::string &text) {
                return bmds::SignedContainer::parseKey(StringView(text.data(), text.size()), k, comment).code() ==
                       Error::CorruptData;
        };
        CHECK(refused("rsa " + hex));
        CHECK(refused("ed25519 " + hex.substr(2)));
        CHECK(refused("ed25519 " + hex.substr(0, 63) + "g"));
        CHECK(refused("ed25519 " + hex + "x comment"));
        CHECK(refused("ed25519 " + hex + " a\ttab"));
        CHECK(refused("ed25519 " + hex + " " + std::string(256, 'c')));
        CHECK(refused("ed25519  " + hex));
        CHECK(refused(""));
}
