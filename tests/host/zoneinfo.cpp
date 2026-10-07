/**
 * @file      zoneinfo.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The time zone database (devplan/time.md §9): the bundle's reader, the
 * VFS's /share/zoneinfo, zone names in the kernel's zone and the shell's TZ.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/clock.h>
#include <bmds/ramblockdevice.h>
#include <bmds/shell.h>
#include <bmds/timecommands.h>
#include <bmds/vfs.h>
#include <bmds/zonebundle.h>

#include "fakeconsole.h"
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::TimeZone;
using bmds::WaitForever;
using bmds::WritableByteSpan;
using bmds::ZoneBundle;
namespace Clock = bmds::Clock;
namespace Vfs = bmds::Vfs;

namespace {

        // A bundle in memory.
        struct MemorySource final : ZoneBundle::Source {
                        explicit MemorySource(std::string b) : bytes(std::move(b)) {}

                        Error readAt(uint32_t offset, WritableByteSpan buf) override {
                                ++reads;
                                if (offset > bytes.size() || buf.size() > bytes.size() - offset)
                                        return Error::CorruptData;
                                std::memcpy(buf.data(), bytes.data() + offset, buf.size());
                                return Error();
                        }

                        std::string bytes;
                        int         reads = 0;
        };

        void putU16(std::string &s, size_t at, uint16_t v) {
                s[at] = static_cast<char>(v & 0xff);
                s[at + 1] = static_cast<char>(v >> 8);
        }

        void putU32(std::string &s, size_t at, uint32_t v) {
                for (int i = 0; i < 4; ++i) s[at + i] = static_cast<char>(v >> (8 * i) & 0xff);
        }

        // A TZif file as zic -b slim makes them, as far as the reader looks:
        // "TZif2", a header's worth of bytes, then the footer line.
        std::string tzif(const std::string &footer, char version = '2') {
                std::string data = std::string("TZif") + version;
                data.resize(44, '\0');
                if (version >= '2') data += "\n" + footer + "\n";
                return data;
        }

        // A bundle as tools/tzbundle writes it: @p zones (name, data), sorted
        // here; equal data stored once.
        std::string bundle(std::vector<std::pair<std::string, std::string>> zones, const char *version = "2026e") {
                std::sort(zones.begin(), zones.end());
                const uint32_t entryBytes = 48;
                const uint32_t dataStart = 32 + static_cast<uint32_t>(zones.size()) * entryBytes;
                std::string    index;
                std::string    data;
                for (const auto &[name, blob] : zones) {
                        size_t at = data.find(blob);
                        if (at == std::string::npos) {
                                at = data.size();
                                data += blob;
                        }
                        std::string entry(entryBytes, '\0');
                        entry.replace(0, name.size(), name);
                        putU32(entry, 40, dataStart + static_cast<uint32_t>(at));
                        putU32(entry, 44, static_cast<uint32_t>(blob.size()));
                        index += entry;
                }
                std::string header(32, '\0');
                header.replace(0, 4, "BMTZ");
                putU16(header, 4, 1);
                putU16(header, 6, 32);
                header.replace(8, std::strlen(version), version);
                putU32(header, 16, static_cast<uint32_t>(zones.size()));
                putU32(header, 20, 32);
                putU32(header, 24, entryBytes);
                putU32(header, 28, dataStart + static_cast<uint32_t>(data.size()));
                return header + index + data;
        }

        const std::string Berlin = tzif("CET-1CEST,M3.5.0,M10.5.0/3");
        const std::string NewYork = tzif("EST5EDT,M3.2.0,M11.1.0");
        const std::string Tokyo = tzif("JST-9");

        std::string sample() {
                return bundle({{"Europe/Berlin", Berlin},
                               {"America/New_York", NewYork},
                               {"US/Eastern", NewYork},
                               {"Asia/Tokyo", Tokyo},
                               {"Japan", Tokyo},
                               {"America/Argentina/Salta", tzif("<-03>3")},
                               {"America/Aruba", tzif("AST4")},
                               {"UTC", tzif("UTC0")}});
        }

        ZoneBundle openBundle(MemorySource &source) {
                auto [b, err] = ZoneBundle::open(source, source.bytes.size());
                REQUIRE_MESSAGE(err.isOk(), err.name());
                return b;
        }

} // namespace

TEST_CASE("TimeZone::isName: names and POSIX rules told apart as musl does") {
        for (const char *name : {"Europe/Berlin", "Japan", ":Europe/Berlin", ":UTC", "Etc/GMT+5", "bogus", "UTCX",
                                 "America/Port-au-Prince", "<+03", "5"})
                CHECK_MESSAGE(TimeZone::isName(name), name);
        for (const char *rules :
             {"", "UTC", "GMT", "UTC0", "EST5EDT", "CET-1CEST,M3.5.0,M10.5.0/3", "<+0530>-5:30", "GMT+0", "JST-9"})
                CHECK_FALSE_MESSAGE(TimeZone::isName(rules), rules);
}

TEST_CASE("Zone bundle: names, aliases and rules") {
        MemorySource      source(sample());
        const ZoneBundle  b = openBundle(source);
        ZoneBundle::Entry e;
        CHECK(b.count() == 8);
        CHECK(b.version() == "2026e");
        REQUIRE(b.find(source, "Europe/Berlin", e).isOk());
        CHECK(e.length == Berlin.size());
        auto [rules, err] = ZoneBundle::rules(source, e);
        REQUIRE(err.isOk());
        CHECK(rules.spec() == "CET-1CEST,M3.5.0,M10.5.0/3");
        // An alias shares its zone's data.
        ZoneBundle::Entry alias;
        REQUIRE(b.find(source, "US/Eastern", alias).isOk());
        REQUIRE(b.find(source, "America/New_York", e).isOk());
        CHECK(alias.offset == e.offset);
        CHECK(b.find(source, "Europe/Berln", e) == Error::NotExist);
        CHECK(b.find(source, "Europe", e) == Error::NotExist); // a directory is no zone
        CHECK(b.find(source, "", e) == Error::NotExist);
        CHECK(b.find(source, "Zulu", e) == Error::NotExist); // past the last
        CHECK(b.lowerBound(source, "").value() == 0);
        CHECK(b.lowerBound(source, "America/").value() == 0);
        CHECK(b.lowerBound(source, "Asia/").value() == 3);
        CHECK(b.lowerBound(source, "zzz").value() == b.count());
        // A lookup reads the index, not the bundle: a binary search.
        source.reads = 0;
        REQUIRE(b.find(source, "UTC", e).isOk());
        CHECK(source.reads <= 5);
}

TEST_CASE("Zone bundle: a damaged or foreign one is refused") {
        const std::string good = sample();
        auto              refused = [](std::string bytes, uint64_t size) {
                MemorySource source(std::move(bytes));
                return ZoneBundle::open(source, size).error();
        };
        CHECK(refused(good, good.size()).isOk());
        CHECK(refused(good, good.size() + 1) == Error::CorruptData); // its size says otherwise
        CHECK(refused(good.substr(0, 20), 20) == Error::CorruptData);
        std::string bad = good;
        bad[0] = 'X';
        CHECK(refused(bad, bad.size()) == Error::CorruptData);
        bad = good;
        putU16(bad, 4, 2);
        CHECK(refused(bad, bad.size()) == Error::NotSupported); // a later format
        bad = good;
        putU32(bad, 16, 100000);
        CHECK(refused(bad, bad.size()) == Error::CorruptData); // more entries than fit
        bad = good;
        putU32(bad, 24, 4);
        CHECK(refused(bad, bad.size()) == Error::CorruptData);
        bad = good;
        bad[9] = '\x01';
        CHECK(refused(bad, bad.size()) == Error::CorruptData); // the version isn't text

        // Entries: checked as they are read.
        ZoneBundle::Entry e;
        {
                std::string bytes = good;
                putU32(bytes, 32 + 40, static_cast<uint32_t>(bytes.size()) - 10); // data past the end
                MemorySource source(bytes);
                CHECK(openBundle(source).entry(source, 0, e) == Error::CorruptData);
                CHECK(openBundle(source).entry(source, 99, e) == Error::OutOfRange);
        }
        {
                std::string bytes = good;
                putU32(bytes, 32 + 40, 40); // data inside the index
                MemorySource source(bytes);
                CHECK(openBundle(source).entry(source, 0, e) == Error::CorruptData);
        }
        {
                std::string bytes = good;
                for (size_t i = 0; i < 40; ++i) bytes[32 + i] = 'A'; // a name without its NUL
                MemorySource source(bytes);
                CHECK(openBundle(source).entry(source, 0, e) == Error::CorruptData);
        }
        // Rules: a version 1 file has none; a footer must end its file.
        {
                MemorySource     source(bundle({{"Old", tzif("", '\0')}, {"Cut", tzif("EST5").substr(0, 49)}}));
                const ZoneBundle b = openBundle(source);
                REQUIRE(b.find(source, "Old", e).isOk());
                CHECK(ZoneBundle::rules(source, e).error() == Error::NotSupported);
                REQUIRE(b.find(source, "Cut", e).isOk());
                CHECK(ZoneBundle::rules(source, e).error() == Error::CorruptData);
        }
        {
                MemorySource     source(bundle({{"Empty", tzif("")}, {"Bad", tzif("not rules")}}));
                const ZoneBundle b = openBundle(source);
                REQUIRE(b.find(source, "Empty", e).isOk());
                CHECK(ZoneBundle::rules(source, e).error() == Error::NotSupported); // no POSIX form
                REQUIRE(b.find(source, "Bad", e).isOk());
                CHECK(ZoneBundle::rules(source, e).error() == Error::CorruptData);
        }
}

#ifdef BMDS_TEST_ZONEINFO
TEST_CASE("Zone bundle: the real database, every zone's rules") {
        std::ifstream in(BMDS_TEST_ZONEINFO, std::ios::binary);
        REQUIRE(in);
        MemorySource      source(std::string(std::istreambuf_iterator<char>(in), {}));
        const ZoneBundle  b = openBundle(source);
        ZoneBundle::Entry e;
        CHECK(b.count() > 500);
        CHECK(b.version().size() == 5); // "2026e"
        for (uint32_t i = 0; i < b.count(); ++i) {
                REQUIRE(b.entry(source, i, e).isOk());
                const auto [rules, err] = ZoneBundle::rules(source, e);
                CHECK_MESSAGE(err.isOk(), e.name, ": ", err.name());
                if (i > 0) {
                        ZoneBundle::Entry previous;
                        REQUIRE(b.entry(source, i - 1, previous).isOk());
                        CHECK(previous.nameView() < e.nameView()); // sorted, no repeats
                }
        }
        const auto rulesOf = [&](const char *name) {
                REQUIRE(b.find(source, name, e).isOk());
                return std::string(ZoneBundle::rules(source, e).value().spec().data(),
                                   ZoneBundle::rules(source, e).value().spec().size());
        };
        CHECK(rulesOf("Europe/Berlin") == "CET-1CEST,M3.5.0,M10.5.0/3");
        CHECK(rulesOf("America/New_York") == "EST5EDT,M3.2.0,M11.1.0");
        CHECK(rulesOf("Europe/Dublin") == "IST-1GMT0,M10.5.0,M3.5.0/1"); // negative DST
        CHECK(rulesOf("Etc/UTC") == "UTC0");
        // Negative DST: Dublin is on GMT in winter, IST (UTC+1) in summer.
        REQUIRE(b.find(source, "Europe/Dublin", e).isOk());
        const TimeZone dublin = ZoneBundle::rules(source, e).value();
        CHECK(dublin.localTime(bmds::Timestamp::fromSeconds(1767225600)).utcOffset == 0);    // 2026-01-01
        CHECK(dublin.localTime(bmds::Timestamp::fromSeconds(1782864000)).utcOffset == 3600); // 2026-07-01
}
#endif

// --- The VFS's /share/zoneinfo, the kernel's zone, the shell ---------------

namespace {

        // A RAM disk of its own, tz0, mounted for a test and ejected after
        // it (the VFS's mount slots are few, and shared by the test files).
        struct ZoneDisk {
                        ZoneDisk() : device(512) {
                                REQUIRE(Vfs::init().isOk());
                                REQUIRE(device.allocate(8 * 2048, bmds::MemSpace::Psram).isOk());
                                char name[bmds::BlockDevices::MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(device, "tz", name).isOk());
                                REQUIRE(std::string(name) == "tz0");
                                REQUIRE(Vfs::format(device, bmds::FsFormat::Auto).isOk());
                        }

                        bmds::RamBlockDevice device;
        };

        ZoneDisk &zoneDisk() {
                static ZoneDisk d;
                return d;
        }

        const Vfs::Context    Root{"/", nullptr};
        constexpr const char *Database = "/tz0/p0/share/zoneinfo.bin";

        constexpr bmds::Command ClockTable[] = {
                {"clock", "[zone TZ | tzdata [PATH] | adjust [+|-]SECONDS]", "clock", bmds::TimeCommands::clock},
                {"date", "[-u] [+FORMAT] | [-u] -s DATE", "date", bmds::TimeCommands::date},
        };

        // The disk mounted with the sample database on it; the kernel zone
        // UTC. Everything back as it was after.
        struct ZoneFixture {
                        ZoneFixture() {
                                ZoneDisk &d = zoneDisk();
                                REQUIRE(Vfs::format(d.device, bmds::FsFormat::Auto).isOk());
                                mount();
                                REQUIRE(Vfs::mkdir("/tz0/p0/share", Root).isOk());
                                install(sample());
                                REQUIRE(Vfs::setZoneDatabase(Database).isOk());
                                REQUIRE(Clock::setTimeZone("UTC0").isOk());
                                live = HostMemory::liveAllocations();
                        }

                        ~ZoneFixture() {
                                (void)Clock::setTimeZone("UTC0");
                                (void)Vfs::setZoneDatabase("");
                                Vfs::eject(zoneDisk().device);
                        }

                        void mount() { REQUIRE(Vfs::automount(zoneDisk().device).value() == 1); }

                        void install(const std::string &bytes, const char *path = Database) {
                                auto [h, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                                REQUIRE(err.isOk());
                                REQUIRE(h->write(ByteSpan(reinterpret_cast<const uint8_t *>(bytes.data()),
                                                          bytes.size()),
                                                 WaitForever)
                                                .error()
                                                .isOk());
                        }

                        std::vector<std::string> list(const char *path) {
                                auto [dir, err] = Vfs::openDirectory(path, Root);
                                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                                std::vector<std::string> names;
                                for (;;) {
                                        auto [info, nextErr] = dir.next();
                                        if (nextErr == Error::EndOfFile) break;
                                        REQUIRE(nextErr.isOk());
                                        names.push_back(std::string(info.name.cStr()) +
                                                        (info.isDirectory() ? "/" : ""));
                                }
                                return names;
                        }

                        std::string read(const char *path) {
                                auto [h, err] = Vfs::open(path, Vfs::Read, Root);
                                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                                std::string text;
                                uint8_t     buf[16];
                                for (;;) {
                                        auto [n, readErr] = h->read(WritableByteSpan(buf), WaitForever);
                                        if (readErr == Error::EndOfFile) break;
                                        REQUIRE(readErr.isOk());
                                        text.append(reinterpret_cast<char *>(buf), n);
                                }
                                return text;
                        }

                        size_t live = 0;
        };

        std::string specOf(const TimeZone &zone) {
                return std::string(zone.spec().data(), zone.spec().size());
        }

} // namespace

TEST_CASE("Vfs: /share/zoneinfo shows the time zone database, read-only") {
        ZoneFixture f;
        CHECK(f.list("/share") == std::vector<std::string>{"zoneinfo/"});
        CHECK(f.list("/share/zoneinfo") ==
              std::vector<std::string>{"America/", "Asia/", "Europe/", "Japan", "US/", "UTC"});
        CHECK(f.list("/share/zoneinfo/America") == std::vector<std::string>{"Argentina/", "Aruba", "New_York"});
        CHECK(f.list("/share/zoneinfo/America/Argentina") == std::vector<std::string>{"Salta"});
        CHECK(Vfs::openDirectory("/share/zoneinfo/Amer", Root).error() == Error::NotExist);
        CHECK(Vfs::openDirectory("/share/zoneinfo/Japan", Root).error() == Error::NotDir);

        CHECK(f.read("/share/zoneinfo/Europe/Berlin") == Berlin);
        CHECK(f.read("/share/zoneinfo/US/Eastern") == NewYork);
        {
                auto [info, err] = Vfs::stat("/share/zoneinfo/Asia/Tokyo", Root);
                REQUIRE(err.isOk());
                CHECK(info.type == bmds::FileType::File);
                CHECK(info.size == Tokyo.size());
                CHECK(info.modified.ns != 0); // the database's
                CHECK(Vfs::stat("/share/zoneinfo/Europe", Root).value().isDirectory());
                CHECK(Vfs::stat("/share/zoneinfo/Europe/Paris", Root).error() == Error::NotExist);
        }
        {
                // Seekable, so a program's fstat() tells the size and mmap() works.
                auto [h, err] = Vfs::open("/share/zoneinfo/Europe/Berlin", Vfs::Read, Root);
                REQUIRE(err.isOk());
                CHECK(h->stream().seek(0, bmds::Stream::SeekFrom::End).value() == Berlin.size());
                CHECK(h->stream().seek(-3, bmds::Stream::SeekFrom::End).value() == Berlin.size() - 3);
                uint8_t buf[8];
                CHECK(h->read(WritableByteSpan(buf), WaitForever).value() == 3);
                CHECK(h->read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);
                CHECK(h->id() == Vfs::stat("/share/zoneinfo/Europe/Berlin", Root).value().id);
                CHECK(h->device() == 0);
                CHECK(h->write(bmds::StringView("x"), WaitForever).error() == Error::BadDescriptor);
        }
        CHECK(Vfs::open("/share/zoneinfo/Japan", Vfs::Write, Root).error() == Error::ReadOnly);
        CHECK(Vfs::open("/share/zoneinfo/New", Vfs::Write | Vfs::Create, Root).error() == Error::ReadOnly);
        CHECK(Vfs::remove("/share/zoneinfo/Japan", Root) == Error::ReadOnly);
        CHECK(Vfs::mkdir("/share/zoneinfo/Mars", Root) == Error::ReadOnly);
        CHECK(Vfs::rename("/share/zoneinfo/Japan", "/tz0/p0/x", Root) == Error::ReadOnly);
        CHECK(Vfs::mount("/share/zoneinfo", zoneDisk().device) == Error::Invalid);
        CHECK(Vfs::mount("/share/zoneinfo/x", zoneDisk().device) == Error::Invalid);
        CHECK(HostMemory::liveAllocations() == f.live);

        // Without the database: an empty directory.
        CHECK(Vfs::setZoneDatabase("/tz0/p0/none.bin").isOk());
        CHECK(f.list("/share/zoneinfo").empty());
        CHECK(Vfs::stat("/share/zoneinfo/Japan", Root).error() == Error::NotExist);
        CHECK(Vfs::setZoneDatabase("").isOk());
        CHECK(f.list("/share/zoneinfo").empty());
        CHECK(Vfs::setZoneDatabase("relative.bin") == Error::Invalid);
        CHECK(Vfs::zoneDatabaseInfo().error() == Error::NotExist);
        // A damaged one: refused, the directory empty.
        f.install("not a database at all, but long enough to have a header");
        CHECK(Vfs::setZoneDatabase(Database).isOk());
        CHECK(Vfs::zoneDatabaseInfo().error() == Error::CorruptData);
        CHECK(f.list("/share/zoneinfo").empty());
        CHECK(HostMemory::liveAllocations() == f.live);
}

TEST_CASE("Vfs: zoneRules looks names up, and forgets when the database changes") {
        ZoneFixture f;
        auto [info, err] = Vfs::zoneDatabaseInfo();
        REQUIRE(err.isOk());
        CHECK(std::string(info.version) == "2026e");
        CHECK(info.count == 8);
        CHECK(specOf(Vfs::zoneRules("Asia/Tokyo").value()) == "JST-9");
        CHECK(specOf(Vfs::zoneRules("Asia/Tokyo").value()) == "JST-9"); // the kept answer
        CHECK(Vfs::zoneRules("Asia/Tokio").error() == Error::NotExist);
        // A new database copied over the old one is seen at once.
        f.install(bundle({{"Asia/Tokyo", tzif("JST-10")}}));
        CHECK(specOf(Vfs::zoneRules("Asia/Tokyo").value()) == "JST-10");
        CHECK(Vfs::remove(Database, Root).isOk());
        CHECK(Vfs::zoneRules("Asia/Tokyo").error() == Error::NotReady);
        f.install(sample());
        CHECK(specOf(Vfs::zoneRules("Asia/Tokyo").value()) == "JST-9");
        // The card pulled: nothing to ask.
        Vfs::eject(zoneDisk().device);
        CHECK(Vfs::zoneRules("Asia/Tokyo").error() == Error::NotReady);
        CHECK(HostMemory::liveAllocations() == f.live);
}

TEST_CASE("Clock: a zone name, pending until its database is found") {
        ZoneFixture f;
        // Found: the rules are the file's footer, the name kept.
        REQUIRE(Clock::setTimeZone("Europe/Berlin").isOk());
        CHECK(specOf(Clock::timeZone()) == "CET-1CEST,M3.5.0,M10.5.0/3");
        CHECK(Clock::zoneName().view() == "Europe/Berlin");
        CHECK(Clock::zoneName().pending == Clock::ZonePending::None);
        REQUIRE(Clock::setTimeZone(":Japan").isOk());
        CHECK(Clock::zoneName().view() == "Japan");
        CHECK(specOf(Clock::timeZone()) == "JST-9");
        // Unknown: refused, the zone unchanged.
        CHECK(Clock::setTimeZone("Europe/Berln") == Error::NotExist);
        CHECK(Clock::zoneName().view() == "Japan");
        CHECK(Clock::setTimeZone("../etc/passwd") == Error::Invalid);
        CHECK(Clock::setTimeZone("/share/zoneinfo/Japan") == Error::Invalid);
        CHECK(Clock::setTimeZone(std::string(40, 'x').c_str()) == Error::Invalid);
        // Rules again: the name goes.
        REQUIRE(Clock::setTimeZone("EST5EDT").isOk());
        CHECK(Clock::zoneName().view().isEmpty());

        // No card: pending, UTC meanwhile; found once its filesystem mounts.
        Vfs::eject(zoneDisk().device);
        REQUIRE(Clock::setTimeZone("Europe/Berlin").isOk());
        CHECK(Clock::zoneName().pending == Clock::ZonePending::NoDatabase);
        CHECK(Clock::zoneName().view() == "Europe/Berlin");
        CHECK(specOf(Clock::timeZone()).empty());
        f.mount();
        CHECK(Clock::zoneName().pending == Clock::ZonePending::None);
        CHECK(specOf(Clock::timeZone()) == "CET-1CEST,M3.5.0,M10.5.0/3");
        // The card going again: the kernel keeps the rules it has.
        Vfs::eject(zoneDisk().device);
        CHECK(specOf(Clock::timeZone()) == "CET-1CEST,M3.5.0,M10.5.0/3");
        f.mount();

        // Pending, then a database without it: still pending, and says why.
        REQUIRE(Vfs::setZoneDatabase("").isOk());
        REQUIRE(Clock::setTimeZone("Mars/Olympus").isOk());
        CHECK(Clock::zoneName().pending == Clock::ZonePending::NoDatabase);
        REQUIRE(Vfs::setZoneDatabase(Database).isOk());
        CHECK(Clock::zoneName().pending == Clock::ZonePending::Unknown);
        CHECK(specOf(Clock::timeZone()).empty());
        // Set again meanwhile: a retry of the old name doesn't undo it.
        REQUIRE(Clock::setTimeZone("UTC0").isOk());
        Clock::retryZoneName();
        CHECK(Clock::zoneName().view().isEmpty());
}

TEST_CASE("Shell: clock zone and TZ take names; clock tzdata") {
        ZoneFixture                  f;
        FakeConsole                  con;
        std::unique_ptr<bmds::Shell> shell =
                std::make_unique<bmds::Shell>(con, bmds::Span<const bmds::Command>(ClockTable), 1 << 20);
        REQUIRE(shell->init().isOk());
        auto out = [&](const char *line, int status = bmds::Shell::ExitSuccess) {
                CHECK(shell->execute(line) == status);
                return con.take();
        };
        CHECK(out("date -u -s '2026-07-01 10:00'") == "Wed Jul  1 10:00:00 UTC 2026\n");
        CHECK(out("TZ=Europe/Berlin; date '+%H:%M %Z'") == "12:00 CEST\n");
        CHECK(out("TZ=:Asia/Tokyo; date '+%H:%M %Z'") == "19:00 JST\n");
        CHECK(out("TZ=Europe/Nowhere; date '+%H:%M %Z'") == "10:00 UTC\n");
        CHECK(out("clock zone America/New_York") == "");
        CHECK(specOf(Clock::timeZone()) == "EST5EDT,M3.2.0,M11.1.0");
        std::string status = out("clock");
        CHECK(status.find("\nZone:      America/New_York (EST5EDT,M3.2.0,M11.1.0; EDT, UTC-0400)\n") !=
              std::string::npos);
        CHECK(out("clock zone Europe/Berln", bmds::Shell::ExitFailure) == "clock: unknown time zone 'Europe/Berln'\n");
        CHECK(out("clock zone 'CET-1CEST,M13'", bmds::Shell::ExitFailure) ==
              "clock: invalid time zone 'CET-1CEST,M13'\n");
        CHECK(out("clock tzdata") == "/tz0/p0/share/zoneinfo.bin: tzdata 2026e, 8 zones\n");

        // No database: the name is pending, and clock says so.
        Vfs::eject(zoneDisk().device);
        CHECK(out("clock zone Europe/Berlin") == "");
        status = out("clock");
        CHECK(status.find("\nZone:      Europe/Berlin (pending: no zone database; UTC until it is found)\n") !=
              std::string::npos);
        CHECK(out("clock tzdata") == "/tz0/p0/share/zoneinfo.bin: No such file or directory\n");
        f.mount();
        status = out("clock");
        CHECK(status.find("\nZone:      Europe/Berlin (CET-1CEST,M3.5.0,M10.5.0/3; CEST, UTC+0200)\n") !=
              std::string::npos);
        CHECK(out("clock tzdata /tz0/p0/share/../share/zoneinfo.bin") ==
              "/tz0/p0/share/zoneinfo.bin: tzdata 2026e, 8 zones\n");
        CHECK(out(("clock tzdata /" + std::string(130, 'x')).c_str(), bmds::Shell::ExitFailure) ==
              "clock: /" + std::string(130, 'x') + ": Invalid argument\n");
}
