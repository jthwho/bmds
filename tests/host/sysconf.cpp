/**
 * @file      sysconf.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system configuration (devplan/sysconf.md): the types, the registry's
 * states and store, watchers, `sysconf`, and the VFS's /home link.
 */

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/configtypes.h>
#include <bmds/ramflashdevice.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::FileType;
using bmds::RamFlashDevice;
using bmds::String;
using bmds::StringView;
using bmds::WaitForever;
using bmds::WritableByteSpan;
namespace Config = bmds::Config;
namespace SysConf = bmds::SysConf;
namespace Vfs = bmds::Vfs;

namespace {

        const Vfs::Context Root{"/", nullptr};

        std::string canon(const Config::Param &p, const char *input) {
                String      out;
                const Error err = Config::canonical(p, input, out);
                return err.isOk() ? std::string(out.cStr()) : std::string("!") + err.name();
        }

        // A littlefs flash mounted at @p path for one test.
        struct Flash {
                        explicit Flash(const char *mountPath) : path(mountPath) {
                                REQUIRE(Vfs::init().isOk());
                                REQUIRE(device.allocate(4096, 64, bmds::MemSpace::Psram).isOk());
                                char name[bmds::BlockDevices::MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(device, "cflash", name).isOk());
                                REQUIRE(Vfs::format(device, bmds::FsFormat::Auto).isOk());
                                REQUIRE(Vfs::mount(path, device).isOk());
                        }
                        ~Flash() {
                                Vfs::eject(device);
                                bmds::BlockDevices::remove(device);
                        }
                        RamFlashDevice device;
                        const char    *path;
        };

        std::string readFile(const char *path) {
                auto [h, err] = Vfs::open(path, Vfs::Read, Root);
                if (err.isError()) return std::string("!") + err.name();
                std::string text;
                uint8_t     buf[256];
                for (;;) {
                        auto [n, rerr] = h->read(WritableByteSpan(buf), WaitForever);
                        if (rerr.isError()) break;
                        text.append(reinterpret_cast<char *>(buf), n);
                }
                return text;
        }

        std::string get(const char *key) {
                String      v;
                const Error err = SysConf::get(key, v);
                return err.isOk() ? std::string(v.cStr()) : std::string("!") + err.name();
        }

        Error refuseOdd(StringView value) {
                return Config::toInt(value) % 2 != 0 ? Error(Error::Invalid) : Error();
        }

        // The built-in declarations of these tests.
        constexpr Config::Param Params[] = {
                {"home", Config::Type::Path, "/cfg/home", "Home", 0, 0, nullptr, nullptr},
                {"panel/edge", Config::Type::Enum, "top", "Edge", 0, 0, "top|bottom", nullptr},
                {"panel/size", Config::Type::Int, "24", "Size", 8, 64, nullptr, nullptr},
                {"wm/palette/desktop", Config::Type::Color, "404040", "Desktop", 0, 0, nullptr, nullptr},
                {"wm/palette/text", Config::Type::Color, "ffffff", "Text", 0, 0, nullptr, nullptr},
                {"wm/palette2", Config::Type::Bool, "false", "Not under wm/palette", 0, 0, nullptr, nullptr},
                {"wm/even", Config::Type::Int, "2", "Even", 0, 100, nullptr, refuseOdd},
        };

        std::vector<std::string> listed;

        void collect(void *, const SysConf::Info &info) {
                listed.push_back(std::string(info.param->key) + " " + info.stateName() + " " + info.value.cStr());
                return;
        }

        int notified = 0;

        void countNotify(void *) {
                ++notified;
                return;
        }

} // namespace

TEST_CASE("Config: keys, types and canonical forms") {
        CHECK(Config::isValidKey("home"));
        CHECK(Config::isValidKey("wm/palette/active-frame"));
        CHECK_FALSE(Config::isValidKey(""));
        CHECK_FALSE(Config::isValidKey("/wm"));
        CHECK_FALSE(Config::isValidKey("wm/"));
        CHECK_FALSE(Config::isValidKey("wm//a"));
        CHECK_FALSE(Config::isValidKey("Wm/a"));
        CHECK_FALSE(Config::isValidKey("wm/a b"));
        CHECK_FALSE(Config::isValidKey(std::string(64, 'a').c_str()));

        const Config::Param b{"b", Config::Type::Bool};
        CHECK(canon(b, "yes") == "true");
        CHECK(canon(b, "off") == "false");
        CHECK(canon(b, "maybe") == "!Invalid");
        const Config::Param i{"i", Config::Type::Int, "0", "", -5, 10};
        CHECK(canon(i, "+7") == "7");
        CHECK(canon(i, "-5") == "-5");
        CHECK(canon(i, "11") == "!OutOfRange");
        CHECK(canon(i, "1x") == "!Invalid");
        const Config::Param s{"s", Config::Type::String, "", "", 0, 4};
        CHECK(canon(s, "abcd") == "abcd");
        CHECK(canon(s, "abcde") == "!OutOfRange");
        const Config::Param c{"c", Config::Type::Color};
        CHECK(canon(c, "#A0B0C0") == "a0b0c0");
        CHECK(canon(c, "a0b0c") == "!Invalid");
        CHECK(Config::toColor("a0b0c0") == 0xa0b0c0u);
        const Config::Param e{"e", Config::Type::Enum, "top", "", 0, 0, "top|bottom"};
        CHECK(canon(e, "bottom") == "bottom");
        CHECK(canon(e, "bot") == "!Invalid");
        CHECK(canon(e, "") == "!Invalid");
        const Config::Param p{"p", Config::Type::Path};
        CHECK(canon(p, "/sd0//p0/./home/../home/") == "/sd0/p0/home");
        CHECK(canon(p, "/") == "/");
        CHECK(canon(p, "relative") == "!Invalid");
        const Config::Param ps{"ps", Config::Type::Paths};
        CHECK(canon(ps, "/a/;;/b//c") == "/a;/b/c");
        CHECK(canon(ps, "/a;b") == "!Invalid");
        CHECK(std::string(Config::typeName(Config::Type::Paths)) == "paths");

        // The store's lines, escapes kept.
        String text;
        REQUIRE(Config::appendStoreLine(text, "a/b", "one\ntwo\\three").isOk());
        CHECK(std::string(text.cStr()) == "a/b=one\\ntwo\\\\three\n");
        std::vector<std::pair<std::string, std::string>> lines;
        const size_t                                     bad = Config::parseStore(
                "# comment\n\na/b=one\\ntwo\\\\three\nBad Key=x\nnoequals\nc=\n",
                [](void *arg, StringView k, StringView v) {
                        static_cast<decltype(lines) *>(arg)->emplace_back(std::string(k.data(), k.size()),
                                                                                                              std::string(v.data(), v.size()));
                },
                &lines);
        CHECK(bad == 2);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0].second == "one\ntwo\\three");
        CHECK(lines[1].first == "c");
        CHECK(lines[1].second.empty());
}

TEST_CASE("SysConf: set, setdef, save, revert, and a reboot") {
        Flash f("/cfg");
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());
        CHECK(get("panel/edge") == "top");
        CHECK(get("nothing") == "!NotExist");
        CHECK(SysConf::set("panel/edge", "left") == Error::Invalid);
        CHECK(SysConf::set("panel/size", "100") == Error::OutOfRange);
        CHECK(SysConf::set("wm/even", "3") == Error::Invalid); // the declaration's check
        CHECK(SysConf::set("wm/even", "4").isOk());
        CHECK(SysConf::set("panel/edge", "bottom").isOk());
        CHECK(SysConf::set("wm/palette/desktop", "#102030").isOk());
        CHECK(get("wm/palette/desktop") == "102030");
        CHECK(SysConf::getColor("wm/palette/desktop").value() == 0x102030u);
        CHECK(SysConf::getInt("panel/size").value() == 24);
        CHECK(SysConf::getInt("panel/edge").error() == Error::Invalid);
        CHECK(readFile("/cfg/sysconf") == "!NotExist"); // nothing saved yet

        // Saved: one key, then the rest.
        CHECK(SysConf::save("panel/edge").isOk());
        CHECK(readFile("/cfg/sysconf").find("panel/edge=bottom\n") != std::string::npos);
        CHECK(readFile("/cfg/sysconf").find("wm/palette/desktop") == std::string::npos);
        SysConf::Info info;
        REQUIRE(SysConf::info("wm/palette/desktop", info).isOk());
        CHECK(std::string(info.stateName()) == "changed");
        CHECK(SysConf::saveAll().isOk());
        REQUIRE(SysConf::info("wm/palette/desktop", info).isOk());
        CHECK(std::string(info.stateName()) == "saved");
        // Changed again but not saved: a reboot forgets it.
        CHECK(SysConf::set("panel/size", "32").isOk());
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());
        CHECK(get("panel/edge") == "bottom");
        CHECK(get("wm/palette/desktop") == "102030");
        CHECK(get("wm/even") == "4");
        CHECK(get("panel/size") == "24");

        // setdef and revert change only the current value; saving the default takes the line out.
        CHECK(SysConf::setDefault("panel/edge").isOk());
        CHECK(get("panel/edge") == "top");
        CHECK(SysConf::revert("panel/edge").isOk());
        CHECK(get("panel/edge") == "bottom");
        CHECK(SysConf::setDefault("panel/edge").isOk());
        CHECK(SysConf::save("panel/edge").isOk());
        CHECK(readFile("/cfg/sysconf").find("panel/edge") == std::string::npos);
        REQUIRE(SysConf::info("panel/edge", info).isOk());
        CHECK(std::string(info.stateName()) == "default");
        CHECK_FALSE(info.hasSaved);

        // A saved line for a key nobody declared survives saves, and applies once declared.
        {
                auto [h, err] = Vfs::open("/cfg/sysconf", Vfs::Write | Vfs::Append, Root);
                REQUIRE(err.isOk());
                REQUIRE(h->write(StringView("app/volume=7\npanel/size=999\n"), WaitForever).error().isOk());
        }
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());
        CHECK(get("panel/size") == "24"); // out of range: ignored
        CHECK(SysConf::set("panel/size", "40").isOk());
        CHECK(SysConf::save("panel/size").isOk());
        const std::string store = readFile("/cfg/sysconf");
        CHECK(store.find("app/volume=7\n") != std::string::npos);
        CHECK(store.find("panel/size=40\n") != std::string::npos);
        CHECK(store.find("panel/size=999") == std::string::npos);
        CHECK(store.find("app/volume") < store.find("panel/size")); // by key
        const Config::Param volume{"app/volume", Config::Type::Int, "5", "Volume", 0, 10, nullptr, nullptr};
        CHECK(get("app/volume") == "!NotExist");
        CHECK(SysConf::declare(volume).isOk());
        CHECK(get("app/volume") == "7");
        CHECK(SysConf::declare(volume).isOk()); // again: fine
        const Config::Param clash{"app/volume", Config::Type::Bool, "false"};
        CHECK(SysConf::declare(clash) == Error::Exists);
        const Config::Param invalid{"App", Config::Type::Bool, "false"};
        CHECK(SysConf::declare(invalid) == Error::Invalid);
}

TEST_CASE("SysConf: list by prefix, watchers") {
        Flash f("/cfg");
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());
        listed.clear();
        CHECK(SysConf::list("wm/palette", collect, nullptr).isOk());
        CHECK(listed ==
              std::vector<std::string>{"wm/palette/desktop default 404040", "wm/palette/text default ffffff"});
        listed.clear();
        CHECK(SysConf::list("", collect, nullptr).isOk());
        CHECK(listed.size() == sizeof(Params) / sizeof(Params[0]));
        CHECK(listed.front().rfind("home ", 0) == 0); // by key

        SysConf::Watcher palette, all;
        notified = 0;
        REQUIRE(palette.start("wm/palette", countNotify, nullptr).isOk());
        REQUIRE(all.start("", nullptr, nullptr).isOk());
        CHECK(palette.start("wm", nullptr, nullptr) == Error::InvalidState);
        CHECK(SysConf::set("wm/palette/text", "000000").isOk());
        CHECK(SysConf::set("wm/palette/text", "000000").isOk()); // unchanged: nothing told
        CHECK(SysConf::set("wm/palette2", "true").isOk());
        CHECK(SysConf::set("panel/edge", "bottom").isOk());
        CHECK(notified == 1);
        String key;
        REQUIRE(palette.next(key));
        CHECK(std::string(key.cStr()) == "wm/palette/text");
        CHECK_FALSE(palette.next(key));
        std::vector<std::string> keys;
        while (all.next(key)) keys.push_back(key.cStr());
        CHECK(keys == std::vector<std::string>{"wm/palette/text", "wm/palette2", "panel/edge"});
        // More than the ring holds: overflowed, once.
        for (int k = 0; k < 40; ++k) (void)SysConf::set("panel/size", k % 2 != 0 ? "10" : "12");
        CHECK(all.next(key)); // the key, once (repeats aren't queued twice)
        CHECK_FALSE(all.next(key));
        CHECK_FALSE(all.overflowed());
        palette.stop();
        CHECK(SysConf::set("wm/palette/text", "111111").isOk());
        CHECK(notified == 1);
        all.stop();
}

namespace {

        std::vector<std::pair<std::string, bool>> reports;

        void recordReport(StringView leads, bool configured) {
                reports.emplace_back(std::string(leads.data(), leads.size()), configured);
                return;
        }

        std::vector<std::string> names(const char *path) {
                auto [dir, err] = Vfs::openDirectory(path, Root);
                REQUIRE(err.isOk());
                std::vector<std::string> out;
                for (auto r = dir.next(); r.isOk(); r = dir.next())
                        out.push_back(std::string(r.value().name.cStr()) +
                                      (r.value().type == FileType::Link ? "@" : ""));
                return out;
        }

} // namespace

TEST_CASE("Vfs: /home, a link to the home that falls back while it isn't mounted") {
        Flash boot("/cfg");
        auto  card = std::make_unique<Flash>("/card");
        CHECK(Vfs::mkdir("/cfg/home", Root).isOk());
        CHECK(Vfs::mkdir("/card/home", Root).isOk());
        reports.clear();
        Vfs::setHomeReport(recordReport);
        CHECK(Vfs::stat("/home", Root).error() == Error::NotExist); // no /home until set
        REQUIRE(Vfs::setHome("/card/home", "/cfg/home").isOk());
        REQUIRE(reports.size() == 1);
        CHECK(reports[0] == std::make_pair(std::string("/card/home"), true));
        CHECK(Vfs::setHome("/home/x", "/cfg/home") == Error::Invalid);

        // Below it, and through it, as the target; itself, a link.
        auto [h, err] = Vfs::open("/home/notes", Vfs::Write | Vfs::Create, Root);
        REQUIRE(err.isOk());
        h.reset();
        CHECK(Vfs::stat("/card/home/notes", Root).isOk());
        CHECK(Vfs::stat("notes", Vfs::Context{"/home", nullptr}).isOk());
        CHECK(Vfs::stat("/home", Root).value().isDirectory());
        CHECK(Vfs::lstat("/home", Root).value().type == FileType::Link);
        CHECK(std::string(Vfs::readLink("/home", Root).value().cStr()) == "/card/home");
        CHECK(std::string(Vfs::realPath("/home/notes", Root).value().cStr()) == "/card/home/notes");
        const std::vector<std::string> root = names("/");
        CHECK(std::find(root.begin(), root.end(), "home@") != root.end());
        CHECK(names("/home") == std::vector<std::string>{"notes"});
        // The link itself can't be changed.
        CHECK(Vfs::remove("/home", Root) == Error::PermissionDenied);
        CHECK(Vfs::rmdir("/home", Root) == Error::PermissionDenied);
        CHECK(Vfs::mkdir("/home", Root) == Error::Exists);
        CHECK(Vfs::symlink("/x", "/home", Root) == Error::Exists);
        CHECK(Vfs::mount("/home", card->device) == Error::Invalid);
        // A link on the card that leads into /home.
        CHECK(Vfs::symlink("/home/notes", "/card/toNotes", Root).isOk());
        CHECK(Vfs::stat("/card/toNotes", Root).isOk());

        // The card goes: /home leads to the fallback, then back when it returns.
        card.reset();
        REQUIRE(reports.size() == 2);
        CHECK(reports[1] == std::make_pair(std::string("/cfg/home"), false));
        CHECK(std::string(Vfs::readLink("/home", Root).value().cStr()) == "/cfg/home");
        CHECK(Vfs::stat("/home/notes", Root).error() == Error::NotExist);
        card = std::make_unique<Flash>("/card"); // a new, empty card
        CHECK(reports.size() == 3);
        CHECK(reports[2] == std::make_pair(std::string("/card/home"), true));

        // Taken away.
        CHECK(Vfs::setHome("", "").isOk());
        CHECK(Vfs::lstat("/home", Root).error() == Error::NotExist);
        Vfs::setHomeReport(nullptr);
}
