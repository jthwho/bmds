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
#include <chrono>
#include <thread>
#include <bmds/blockdevices.h>
#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include <bmds/configtypes.h>
#include <bmds/password.h>
#include <bmds/shacrypt.h>
#include <bmds/ramflashdevice.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::FileType;
using bmds::Process;
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
        CHECK(SysConf::declare(clash).error() == Error::Exists);
        const Config::Param invalid{"App", Config::Type::Bool, "false"};
        CHECK(SysConf::declare(invalid).error() == Error::Invalid);
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
        SysConf::Id id;
        REQUIRE(palette.next(id));
        CHECK(SysConf::keyOf(id) == "wm/palette/text");
        CHECK_FALSE(palette.next(id));
        std::vector<std::string> keys;
        while (all.next(id)) keys.emplace_back(SysConf::keyOf(id).data(), SysConf::keyOf(id).size());
        CHECK(keys == std::vector<std::string>{"wm/palette/text", "wm/palette2", "panel/edge"});
        // A repeat isn't queued twice, so no overflow.
        for (int k = 0; k < 40; ++k) (void)SysConf::set("panel/size", k % 2 != 0 ? "10" : "12");
        CHECK(all.next(id)); // the key, once
        CHECK_FALSE(all.next(id));
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

TEST_CASE("SysConf: ids") {
        Flash f("/cfg");
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());

        // A key's id is the same each time, and names the key.
        const bmds::Result<SysConf::Id> edge = SysConf::find("panel/edge");
        REQUIRE(edge.isOk());
        CHECK(SysConf::find("panel/edge").value() == edge.value());
        CHECK(SysConf::keyOf(edge.value()) == "panel/edge");
        CHECK(SysConf::find("panel/nope").error() == Error::NotExist);
        CHECK(SysConf::keyOf(SysConf::NoId).isEmpty());
        String value;
        CHECK(SysConf::get(SysConf::NoId, value) == Error::NotExist);
        CHECK(SysConf::set(SysConf::NoId, "x") == Error::NotExist);

        // By id as by key: checked, saved, reverted, typed.
        CHECK(SysConf::set(edge.value(), "left") == Error::Invalid);
        CHECK(SysConf::set(edge.value(), "bottom").isOk());
        CHECK(get("panel/edge") == "bottom");
        CHECK(SysConf::save(edge.value()).isOk());
        CHECK(SysConf::setDefault(edge.value()).isOk());
        CHECK(get("panel/edge") == "top");
        CHECK(SysConf::revert(edge.value()).isOk());
        REQUIRE(SysConf::get(edge.value(), value).isOk());
        CHECK(std::string(value.cStr()) == "bottom");
        SysConf::Info info;
        REQUIRE(SysConf::info(edge.value(), info).isOk());
        CHECK(info.hasSaved);
        const SysConf::Id size = SysConf::find("panel/size").value();
        CHECK(SysConf::getInt(size).value() == 24);
        CHECK(SysConf::getInt(edge.value()).error() == Error::Invalid);
        CHECK(SysConf::getColor(SysConf::find("wm/palette/text").value()).value() == 0xffffffu);

        // A watcher started before a key is declared is told of it if the key is under its prefix;
        // the declared key gets an id, the same when declared again, and the others keep theirs.
        SysConf::Watcher app, panel;
        REQUIRE(app.start("app", nullptr, nullptr).isOk());
        REQUIRE(panel.start("panel", nullptr, nullptr).isOk());
        const Config::Param             first{"aaa/first", Config::Type::Int, "1", "", 0, 9, nullptr, nullptr};
        const Config::Param             volume{"app/volume", Config::Type::Int, "5", "", 0, 10, nullptr, nullptr};
        const bmds::Result<SysConf::Id> a = SysConf::declare(first); // sorts before every key
        const bmds::Result<SysConf::Id> v = SysConf::declare(volume);
        REQUIRE(a.isOk());
        REQUIRE(v.isOk());
        CHECK(SysConf::declare(volume).value() == v.value());
        CHECK(SysConf::find("app/volume").value() == v.value());
        CHECK(SysConf::find("panel/edge").value() == edge.value());
        CHECK(SysConf::set(v.value(), "8").isOk());
        CHECK(SysConf::set(a.value(), "2").isOk());
        CHECK(SysConf::set(edge.value(), "top").isOk());
        SysConf::Id id;
        REQUIRE(app.next(id));
        CHECK(id == v.value());
        CHECK_FALSE(app.next(id)); // aaa/first isn't under app
        REQUIRE(panel.next(id));
        CHECK(id == edge.value());
        CHECK_FALSE(panel.next(id)); // nor app/volume under panel
        app.stop();
        panel.stop();
}

TEST_CASE("SysConf: a kernel declaration keeps its check, a program's doesn't") {
        Flash f("/cfg");
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());
        constexpr Config::Check evenOnly = [](bmds::StringView v) -> Error {
                return (Config::toInt(v) % 2) == 0 ? Error::Ok : Error::Invalid;
        };
        const Config::Param kernel{"net/eth9/even", Config::Type::Int, "0", "", 0, 100, nullptr, evenOnly};
        const Config::Param program{"app/even", Config::Type::Int, "0", "", 0, 100, nullptr, evenOnly};
        REQUIRE(SysConf::declareKernel(kernel).isOk());
        REQUIRE(SysConf::declare(program).isOk());
        CHECK(SysConf::set("net/eth9/even", "4").isOk());
        CHECK(SysConf::set("net/eth9/even", "5") == Error::Invalid);
        CHECK(get("net/eth9/even") == "4");
        CHECK(SysConf::set("app/even", "5").isOk()); // the program's code may be gone: no check
        CHECK(SysConf::set("net/eth9/even", "102") == Error::OutOfRange);
}

TEST_CASE("SysConf: the SDK's calls (sysconf_*)") {
        Flash f("/cfg");
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());
        bmds::FdTable fds;
        Process       p(fds);
        char          text[BMDS_SYSCONF_VALUE_MAX + 1];

        // find, get: each field; errors.
        const int32_t edge = p.sysconfFind("panel/edge");
        REQUIRE(edge >= 0);
        CHECK(p.sysconfFind("panel/nope") == -BMDS_ENOENT);
        CHECK(p.sysconfFind(std::string(200, 'a').c_str()) == -BMDS_ENOENT);
        CHECK(p.sysconfFind(nullptr) == -BMDS_EFAULT);
        CHECK(p.sysconfGet(edge, BMDS_SYSCONF_FIELD_VALUE, text, sizeof(text)) == 3);
        CHECK(std::string(text) == "top");
        CHECK(p.sysconfGet(edge, BMDS_SYSCONF_FIELD_KEY, text, sizeof(text)) == 10);
        CHECK(std::string(text) == "panel/edge");
        CHECK(p.sysconfGet(edge, BMDS_SYSCONF_FIELD_CHOICES, text, sizeof(text)) > 0);
        CHECK(std::string(text) == "top|bottom");
        CHECK(p.sysconfGet(edge, BMDS_SYSCONF_FIELD_HELP, text, sizeof(text)) > 0);
        CHECK(p.sysconfGet(edge, BMDS_SYSCONF_FIELD_SAVED, text, sizeof(text)) == -BMDS_ENOENT);
        CHECK(p.sysconfGet(edge, BMDS_SYSCONF_FIELD_VALUE, text, 3) == -BMDS_ERANGE); // "top" and its NUL
        CHECK(p.sysconfGet(edge, 9, text, sizeof(text)) == -BMDS_EINVAL);
        CHECK(p.sysconfGet(9999, BMDS_SYSCONF_FIELD_VALUE, text, sizeof(text)) == -BMDS_ENOENT);
        CHECK(p.sysconfGet(-5, BMDS_SYSCONF_FIELD_VALUE, text, sizeof(text)) == -BMDS_ENOENT);

        // set: each op; info's flags follow.
        bmds_sysconf_info_t info = {};
        info.size = sizeof(info);
        REQUIRE(p.sysconfInfo(edge, &info) == 0);
        CHECK(info.type == BMDS_SYSCONF_TYPE_ENUM);
        CHECK(info.flags == BMDS_SYSCONF_IS_DEFAULT);
        CHECK(p.sysconfSet(edge, BMDS_SYSCONF_OP_SET, "left") == -BMDS_EINVAL);
        CHECK(p.sysconfSet(edge, 7, "top") == -BMDS_EINVAL);
        CHECK(p.sysconfSet(edge, BMDS_SYSCONF_OP_SET, nullptr) == -BMDS_EFAULT);
        CHECK(p.sysconfSet(edge, BMDS_SYSCONF_OP_SET, "bottom") == 0);
        REQUIRE(p.sysconfInfo(edge, &info) == 0);
        CHECK(info.flags == BMDS_SYSCONF_CHANGED);
        CHECK(p.sysconfSet(edge, BMDS_SYSCONF_OP_SAVE, nullptr) == 0);
        REQUIRE(p.sysconfInfo(edge, &info) == 0);
        CHECK(info.flags == BMDS_SYSCONF_IS_SAVED);
        CHECK(p.sysconfSet(edge, BMDS_SYSCONF_OP_SETDEF, nullptr) == 0);
        CHECK(get("panel/edge") == "top");
        CHECK(p.sysconfSet(edge, BMDS_SYSCONF_OP_REVERT, nullptr) == 0);
        CHECK(get("panel/edge") == "bottom");
        CHECK(p.sysconfGet(edge, BMDS_SYSCONF_FIELD_SAVED, text, sizeof(text)) == 6);
        const int32_t size = p.sysconfFind("panel/size");
        CHECK(p.sysconfSet(size, BMDS_SYSCONF_OP_SET, "100") == -BMDS_ERANGE);
        REQUIRE(p.sysconfInfo(size, &info) == 0);
        CHECK(info.min == 8);
        CHECK(info.max == 64);
        CHECK(p.sysconfSet(size, BMDS_SYSCONF_OP_SET, "30") == 0);
        CHECK(p.sysconfSet(BMDS_SYSCONF_ALL, BMDS_SYSCONF_OP_SAVE, nullptr) == 0);
        CHECK(readFile("/cfg/sysconf").find("panel/size=30\n") != std::string::npos);
        // A shorter struct gets what fits.
        bmds_sysconf_info_t small = {};
        small.size = 8;
        small.min = 77;
        REQUIRE(p.sysconfInfo(size, &small) == 0);
        CHECK(small.type == BMDS_SYSCONF_TYPE_INT);
        CHECK(small.min == 77);

        // next: in key order, under the prefix.
        std::vector<std::string> keys;
        for (int32_t id = p.sysconfNext("wm", -1); id >= 0; id = p.sysconfNext("wm", id)) {
                REQUIRE(p.sysconfGet(id, BMDS_SYSCONF_FIELD_KEY, text, sizeof(text)) > 0);
                keys.emplace_back(text);
        }
        CHECK(keys == std::vector<std::string>{"wm/even", "wm/palette/desktop", "wm/palette/text", "wm/palette2"});
        CHECK(p.sysconfNext("wm/palette", p.sysconfFind("wm/palette/text")) == -BMDS_ENOENT); // not palette2
        CHECK(p.sysconfNext("nothing", -1) == -BMDS_ENOENT);

        // declare: a program's own key, again, and refusals.
        bmds_sysconf_param_t param = {};
        param.size = sizeof(param);
        param.type = BMDS_SYSCONF_TYPE_INT;
        param.key = "game/level";
        param.default_value = "3";
        param.help = "The level";
        param.min = 1;
        param.max = 9;
        const int32_t level = p.sysconfDeclare(&param);
        REQUIRE(level >= 0);
        CHECK(p.sysconfDeclare(&param) == level);
        CHECK(p.sysconfFind("game/level") == level);
        REQUIRE(p.sysconfInfo(level, &info) == 0);
        CHECK(info.flags == (BMDS_SYSCONF_IS_DEFAULT | BMDS_SYSCONF_RUNTIME));
        param.type = BMDS_SYSCONF_TYPE_BOOL;
        CHECK(p.sysconfDeclare(&param) == -BMDS_EEXIST);
        param.type = 8;
        CHECK(p.sysconfDeclare(&param) == -BMDS_EINVAL);
        param.type = BMDS_SYSCONF_TYPE_INT;
        param.key = "Game";
        CHECK(p.sysconfDeclare(&param) == -BMDS_EINVAL);
        param.key = "game/other";
        param.default_value = "99";
        CHECK(p.sysconfDeclare(&param) == -BMDS_ERANGE); // the default outside its limits
        param.size = 8;
        CHECK(p.sysconfDeclare(&param) == -BMDS_EINVAL);

        // watch: records of ids, io_poll, close.
        const int32_t fd = p.sysconfWatch("panel", BMDS_O_CLOEXEC);
        REQUIRE(fd >= 0);
        CHECK(p.sysconfWatch("Bad Prefix", 0) == -BMDS_EINVAL);
        CHECK(p.sysconfWatch("", 0x80) == -BMDS_EINVAL);
        bmds_pollfd_t poll = {};
        poll.size = sizeof(poll);
        poll.fd = fd;
        poll.events = BMDS_POLL_IN;
        CHECK(p.poll(&poll, 1, 0) == 0);
        CHECK(p.sysconfSet(edge, BMDS_SYSCONF_OP_SET, "top") == 0);
        CHECK(p.sysconfSet(size, BMDS_SYSCONF_OP_SET, "40") == 0);
        CHECK(p.sysconfSet(level, BMDS_SYSCONF_OP_SET, "5") == 0); // not under panel
        CHECK(p.poll(&poll, 1, 0) == 1);
        uint32_t records[8] = {};
        CHECK(p.read(fd, records, 6) == -BMDS_EINVAL);
        CHECK(p.read(fd, records, 4) == 4); // one at a time
        CHECK(records[0] == uint32_t(edge));
        CHECK(p.read(fd, records, sizeof(records)) == 4);
        CHECK(records[0] == uint32_t(size));
        CHECK(p.poll(&poll, 1, 0) == 0);
        // A read waits for the next change, from another task.
        std::thread setter([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                CHECK(SysConf::set("panel/edge", "bottom").isOk());
        });
        CHECK(p.read(fd, records, sizeof(records)) == 4);
        CHECK(records[0] == uint32_t(edge));
        setter.join();
        // More than it keeps: the overflow record first.
        for (int i = 0; i < 20; ++i) {
                char key[32];
                std::snprintf(key, sizeof(key), "panel/x%d", i);
                bmds_sysconf_param_t x = {};
                x.size = sizeof(x);
                x.type = BMDS_SYSCONF_TYPE_BOOL;
                x.key = key;
                x.default_value = "false";
                const int32_t id = p.sysconfDeclare(&x);
                REQUIRE(id >= 0);
                CHECK(p.sysconfSet(id, BMDS_SYSCONF_OP_SET, "true") == 0);
        }
        REQUIRE(p.poll(&poll, 1, 0) == 1); // (a read would wait otherwise)
        CHECK(p.read(fd, records, sizeof(records)) == int32_t(sizeof(records)));
        CHECK(records[0] == BMDS_SYSCONF_OVERFLOW);
        CHECK(p.close(fd) == 0);
        CHECK(SysConf::set("panel/edge", "top").isOk()); // the watch is gone
}

TEST_CASE("SysConf: a secret is never shown; the password, hashed, in /etc/shadow") {
        namespace Password = bmds::Password;
        Flash f("/cfg");
        REQUIRE(SysConf::initForHost(Params, "/cfg/sysconf").isOk());
        REQUIRE(SysConf::declareKernel(Password::declaration()).isOk());
        CHECK(std::string(Config::typeName(Config::Type::Secret)) == "secret");
        CHECK(get("system/password") == "");
        CHECK_FALSE(Password::isSet());
        // Only a hash goes in.
        CHECK(SysConf::set("system/password", "plain text") == Error::Invalid);
        REQUIRE(Password::set("open sesame").isOk());
        CHECK(Password::isSet());
        // Readers see that it is set, never the hash.
        CHECK(get("system/password") == "(set)");
        SysConf::Info info;
        REQUIRE(SysConf::info("system/password", info).isOk());
        CHECK(std::string(info.value.cStr()) == "(set)");
        CHECK(std::string(info.saved.cStr()) == "(set)");
        CHECK(info.hasSaved);
        listed.clear();
        REQUIRE(SysConf::list("system", collect, nullptr).isOk());
        CHECK(listed == std::vector<std::string>{"system/password saved (set)"});
        // The kernel's own read, and the store, hold the hash.
        String hash;
        REQUIRE(SysConf::getSecret("system/password", hash).isOk());
        CHECK(bmds::ShaCrypt::isHash(hash.view()));
        CHECK(hash.view().startsWith("$6$rounds=2000$"));
        CHECK(readFile("/cfg/sysconf").find(std::string("system/password=") + hash.cStr()) != std::string::npos);
        // /etc/shadow's line has it, for crypt() in programs.
        String shadow;
        REQUIRE(Password::shadowFile(shadow).isOk());
        CHECK(std::string(shadow.cStr()) == std::string("root:") + hash.cStr() + ":::::::\n");
        // Cleared.
        REQUIRE(Password::set("").isOk());
        CHECK_FALSE(Password::isSet());
        CHECK(get("system/password") == "");
        shadow.clear();
        REQUIRE(Password::shadowFile(shadow).isOk());
        CHECK(std::string(shadow.cStr()) == "root:!:::::::\n"); // no password: logins refused
}
