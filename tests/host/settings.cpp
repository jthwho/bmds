/**
 * @file      settings.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Settings' pages as data (devplan/controlpanel.md §5): rows well formed, every key placed once,
 * labels and words.
 */

#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <bmds/configtypes.h>
#include <bmds/terminalview.h>
#include <bmds/windowpalette.h>

#include "settingstable.h"
#include "themes.h"

using bmds::Span;
using bmds::StringView;
namespace T = bmds::SettingsTable;

namespace {

        std::string text(size_t (*fn)(StringView, Span<char>), StringView in) {
                char         out[64];
                const size_t n = fn(in, Span<char>(out, sizeof(out)));
                return std::string(out, n);
        }

        // A pattern as a key: each `*` made a component.
        std::string asKey(const char *pattern) {
                std::string k(pattern);
                for (char &c : k)
                        if (c == '*') c = 'x';
                return k;
        }

} // namespace

TEST_CASE("SettingsTable: pages and rows well formed, each key or pattern placed once") {
        const Span<const T::Page> pages = T::pages();
        REQUIRE(pages.size() >= 2);
        CHECK(std::strcmp(pages[pages.size() - 1].id, "all") == 0);
        CHECK(pages[pages.size() - 1].rows.isEmpty());
        for (size_t i = 0; i < pages.size(); ++i) {
                CAPTURE(pages[i].id);
                CHECK(T::findPage(pages[i].id) == int(i));
                CHECK(pages[i].icon[0] != '\0');
                for (const T::Row &r : pages[i].rows) {
                        if (r.key == nullptr) {
                                CHECK(r.label != nullptr); // a heading has a title
                                continue;
                        }
                        CAPTURE(r.key);
                        CHECK(bmds::Config::isValidKey(StringView(asKey(r.key).c_str())));
                        // Placed here, by this row: no row before it (on any page) takes its keys.
                        const T::Place at = T::place(StringView(asKey(r.key).c_str()));
                        CHECK(at.page == int(i));
                        CHECK(at.row == &r);
                        if (r.choices != nullptr) {
                                // word=Label pairs
                                StringView rest(r.choices);
                                while (!rest.isEmpty()) {
                                        const size_t bar = rest.indexOf('|');
                                        CHECK(rest.left(bar).indexOf('=') != StringView::NPos);
                                        if (bar == StringView::NPos) break;
                                        rest = rest.mid(bar + 1);
                                }
                        }
                }
        }
        CHECK(T::findPage("nothing") == -1);
}

TEST_CASE("SettingsTable: patterns, places, labels and words") {
        StringView star;
        CHECK(T::matches("net/*/ipv4/mode", "net/eth0/ipv4/mode", star));
        CHECK(star == StringView("eth0"));
        CHECK_FALSE(T::matches("net/*/ipv4/mode", "net/eth0/ipv4", star));
        CHECK_FALSE(T::matches("net/*/ipv4/mode", "net/eth0/ipv4/mode/x", star));
        CHECK_FALSE(T::matches("wm/palette/*", "wm/palette2/face", star));
        CHECK(T::matches("home", "home", star));
        CHECK(star.isEmpty());

        const auto pageOf = [](const char *key) {
                const int p = T::place(StringView(key)).page;
                return p >= 0 ? std::string(T::pages()[size_t(p)].id) : std::string("-");
        };
        CHECK(pageOf("net/eth0/ipv4/address") == "network");
        CHECK(pageOf("net/ptp/domain") == "time");
        CHECK(pageOf("net/ssh/port") == "sharing");
        CHECK(pageOf("wm/palette/active-frame") == "appearance");
        CHECK(pageOf("terminal/colors/bright-red") == "terminal");
        CHECK(pageOf("audio/dsp0/volume") == "sound");
        CHECK(pageOf("example/greeting") == "-"); // a program's: All Settings only
        const T::Place ptp = T::place("net/ptp/profile");
        REQUIRE(ptp.section != nullptr);
        CHECK(std::string(ptp.section->label) == "Precision time (PTP)");

        char          out[64];
        const T::Row &ipv4 = *T::place("net/eth1/ipv4/mode").row;
        CHECK(std::string(out, T::label(ipv4, "net/eth1/ipv4/mode", Span<char>(out, sizeof(out)))) == "IPv4 (eth1)");
        const T::Row &role = *T::place("wm/palette/active-frame").row;
        CHECK(std::string(out, T::label(role, "wm/palette/active-frame", Span<char>(out, sizeof(out)))) ==
              "Active frame");
        const T::Row &later = *T::place("wm/palette/new-role").row; // a role added later: the pattern's
        CHECK(std::string(later.key) == "wm/palette/*");
        CHECK(std::string(out, T::label(later, "wm/palette/new-role", Span<char>(out, sizeof(out)))) == "New role");
        CHECK(text(T::labelOf, "cpu-graph") == "Cpu graph");
        CHECK(text(T::labelOf, "") == "");
        CHECK(std::string(out, T::choiceLabel(ipv4.choices, "dhcp", Span<char>(out, sizeof(out)))) ==
              "Automatic (DHCP)");
        CHECK(std::string(out, T::choiceLabel(ipv4.choices, "static", Span<char>(out, sizeof(out)))) == "Manual");
        CHECK(std::string(out, T::choiceLabel(nullptr, "double", Span<char>(out, sizeof(out)))) == "Double");
        CHECK(std::string(out, T::choiceLabel("auto=Automatic", "-3", Span<char>(out, sizeof(out)))) == "-3");
        char small[4];
        CHECK(T::labelOf("abcdef", Span<char>(small, sizeof(small))) == 3); // cut to fit, NUL kept

        CHECK(T::containsFolded("Time zone", "ZONE"));
        CHECK(T::containsFolded("net/ssh/port", "SSH/P"));
        CHECK_FALSE(T::containsFolded("Port", "ports"));
        CHECK(T::containsFolded("anything", ""));
}

TEST_CASE("SettingsTable: a row naming a key wins over a pattern matching it") {
        const T::Place mdns = T::place("net/mdns/enabled");
        REQUIRE(mdns.row != nullptr);
        CHECK(std::string(mdns.row->key) == "net/mdns/enabled");
        CHECK(std::string(T::place("net/ntp/enabled").row->key) == "net/ntp/enabled");
        CHECK(std::string(T::place("net/eth0/enabled").row->key) == "net/*/enabled");
}

TEST_CASE("SettingsTable: conditions on rows") {
        struct Values {
                        std::vector<std::pair<std::string, std::string>> kv;
        } values;
        values.kv = {{"net/eth0/enabled", "true"},      {"net/eth0/ipv4/mode", "dhcp"},
                     {"panel/items", "launcher;clock"}, {"log/dir", ""},
                     {"net/ptp/enabled", "true"},       {"net/ptp/slave-only", "false"}};
        const T::ValueOf get = [](void *arg, StringView key, char *out, size_t size) {
                for (const auto &p : static_cast<Values *>(arg)->kv) {
                        if (p.first != std::string(key.data(), key.size())) continue;
                        std::snprintf(out, size, "%s", p.second.c_str());
                        return true;
                }
                return false;
        };
        CHECK(T::applies(nullptr, "", get, &values));
        CHECK(T::applies("", "", get, &values));
        CHECK(T::applies("net/*/enabled=true", "eth0", get, &values));
        CHECK_FALSE(T::applies("net/*/enabled=true,net/*/ipv4/mode=static", "eth0", get, &values));
        CHECK(T::applies("net/*/ipv4/mode=static|dhcp", "eth0", get, &values));
        CHECK(T::applies("net/*/ipv4/mode!=static", "eth0", get, &values));
        CHECK_FALSE(T::applies("net/*/enabled=true", "eth1", get, &values)); // no such key
        CHECK(T::applies("panel/items~clock", "", get, &values));
        CHECK_FALSE(T::applies("panel/items~spacer", "", get, &values));
        CHECK_FALSE(T::applies("panel/items~launch", "", get, &values)); // whole items only
        CHECK_FALSE(T::applies("log/dir!=", "", get, &values));
        CHECK(T::applies("log/dir=", "", get, &values));
        CHECK(T::applies("net/ptp/enabled=true,net/ptp/slave-only=false", "", get, &values));
        CHECK_FALSE(T::applies("net/ptp/enabled", "", get, &values)); // no operator
        CHECK_FALSE(T::applies("net/ptp/enabled!true", "", get, &values));
        // Every row's condition is well formed: each part has an operator.
        for (const T::Page &p : T::pages())
                for (const T::Row &r : p.rows) {
                        if (r.when == nullptr) continue;
                        CAPTURE(r.key);
                        StringView rest(r.when);
                        while (!rest.isEmpty()) {
                                const size_t     comma = rest.indexOf(',');
                                const StringView c = rest.left(comma);
                                CHECK((c.indexOf('=') != StringView::NPos || c.indexOf('~') != StringView::NPos));
                                rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                        }
                }
}

TEST_CASE("Themes: complete sets, the standard ones the defaults, matched exactly") {
        const bmds::Themes::Set &sys = bmds::Themes::system();
        REQUIRE(sys.names.size() == bmds::WindowPalette::RoleCount);
        const bmds::WindowPalette standard;
        for (size_t i = 0; i < sys.names.size(); ++i) {
                const auto role = static_cast<bmds::WindowPalette::Role>(i);
                CHECK(std::string(sys.names[i]) == bmds::WindowPalette::name(role));
                CHECK(sys.themes[0].colors[i] == standard.color(role));
        }
        const bmds::Themes::Set &term = bmds::Themes::terminal();
        REQUIRE(term.names.size() == 19);
        const bmds::TerminalColors tc;
        CHECK(term.themes[0].colors[0] == tc.text);
        CHECK(term.themes[0].colors[1] == tc.background);
        CHECK(term.themes[0].colors[2] == tc.cursor);
        for (size_t i = 0; i < 16; ++i) CHECK(term.themes[0].colors[3 + i] == tc.ansi[i]);
        for (const bmds::Themes::Set *set : {&sys, &term}) {
                for (size_t t = 0; t < set->themes.size(); ++t) {
                        const Span<const uint32_t> colors(set->themes[t].colors, set->names.size());
                        CHECK(bmds::Themes::match(*set, colors) == int(t)); // and no two alike
                }
                std::vector<uint32_t> custom(set->themes[0].colors, set->themes[0].colors + set->names.size());
                custom[1] ^= 1;
                CHECK(bmds::Themes::match(*set, Span<const uint32_t>(custom.data(), custom.size())) == -1);
        }
}
