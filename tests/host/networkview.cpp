/**
 * @file      networkview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tray's network icon (devplan/network.md §17, §20): an interface's state, the best of
 * several, the picture's lights, dimming and cross, the lines of each state, the tooltip, spans.
 */

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "networkview.h"

using bmds::NetworkView;
using bmds::Span;
namespace NetStatus = bmds::NetStatus;
using State = NetworkView::State;

namespace {

        using Picture = uint32_t[NetworkView::Side * NetworkView::Side];
        constexpr int Side = NetworkView::Side;

        // a.b.c.d in network order, as NetStatus has addresses.
        uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
                const uint8_t bytes[4] = {a, b, c, d};
                uint32_t      v;
                std::memcpy(&v, bytes, sizeof(v));
                return v;
        }

        NetStatus::Info eth(const char *name = "eth0") {
                NetStatus::Info i;
                std::strncpy(i.name, name, sizeof(i.name) - 1);
                i.up = true;
                i.link = true;
                i.fullDuplex = true;
                i.speedMbps = 100;
                const uint8_t mac[6] = {0xe8, 0xf6, 0x0a, 0xe2, 0xe2, 0x22};
                std::memcpy(i.mac, mac, sizeof(mac));
                i.mode = NetStatus::Mode::Dhcp;
                i.dhcp = NetStatus::Dhcp::Bound;
                i.address = ip(172, 20, 200, 221);
                i.netmask = ip(255, 255, 0, 0);
                i.gateway = ip(172, 20, 0, 1);
                i.dhcpServer = ip(172, 20, 0, 1);
                i.leaseSeconds = 7200;
                i.leaseLeftSeconds = 6720;
                i.dns[0] = ip(172, 20, 0, 2);
                return i;
        }

        NetStatus::Info lo() {
                NetStatus::Info i;
                std::strncpy(i.name, "lo", sizeof(i.name) - 1);
                i.up = true;
                i.link = true;
                i.loopback = true;
                i.mode = NetStatus::Mode::Static;
                i.address = ip(127, 0, 0, 1);
                return i;
        }

        std::string all(const NetStatus::Info &info, bool leaseLeft) {
                char         l[NetworkView::MaxLines][NetworkView::LineBytes];
                const size_t n = NetworkView::lines(info, leaseLeft, l);
                std::string  s;
                for (size_t i = 0; i < n; ++i) s += std::string(l[i]) + "|";
                return s;
        }

        uint32_t at(const Picture &p, int x, int y) {
                return p[y * Side + x];
        }

} // namespace

TEST_CASE("NetworkView: states, and the best of several") {
        NetStatus::Info i = eth();
        CHECK(NetworkView::stateOf(i) == State::Connected);
        i.address = 0;
        CHECK(NetworkView::stateOf(i) == State::Acquiring);
        i.link = false;
        CHECK(NetworkView::stateOf(i) == State::NoLink);
        i.mode = NetStatus::Mode::Off;
        CHECK(NetworkView::stateOf(i) == State::Off);
        i = eth();
        i.up = false;
        CHECK(NetworkView::stateOf(i) == State::Off);

        // lo is never counted; the best interface wins; none is off.
        NetStatus::Info list[3] = {lo(), eth("eth0"), eth("eth1")};
        list[1].link = false;
        list[2].address = 0;
        CHECK(NetworkView::best(Span<const NetStatus::Info>(list, 3)) == State::Acquiring);
        CHECK(NetworkView::best(Span<const NetStatus::Info>(list, 2)) == State::NoLink);
        CHECK(NetworkView::best(Span<const NetStatus::Info>(list, 1)) == State::Off);
        CHECK(NetworkView::best(Span<const NetStatus::Info>()) == State::Off);
}

TEST_CASE("NetworkView: the picture's lights, dimming and cross") {
        Picture   p;
        const int lx = NetworkView::LeftLightX, rx = NetworkView::RightLightX, ly = NetworkView::LightY;

        NetworkView::draw(State::Connected, p);
        CHECK(at(p, 0, 0) == 0); // transparent round it
        CHECK(at(p, lx, ly) == NetworkView::GreenLight);
        CHECK(at(p, rx, ly) == NetworkView::GreenLight);
        CHECK(at(p, 4, 4) == NetworkView::PinColor);
        CHECK(at(p, 1, 2) == NetworkView::EdgeColor);

        NetworkView::draw(State::Acquiring, p);
        CHECK(at(p, lx, ly) == NetworkView::AmberLight);
        CHECK(at(p, rx, ly) == NetworkView::DarkLight);
        CHECK((at(p, 1, 2) >> 24) == 0xff);

        NetworkView::draw(State::Off, p);
        CHECK((at(p, lx, ly) & 0x00ffffff) == (NetworkView::DarkLight & 0x00ffffff));
        CHECK((at(p, 1, 2) >> 24) < 0xff); // dimmed
        CHECK(at(p, 2, 2) != NetworkView::CrossColor);

        NetworkView::draw(State::NoLink, p);
        CHECK((at(p, 1, 2) >> 24) < 0xff);
        CHECK(at(p, 2, 2) == NetworkView::CrossColor); // the cross, at full strength
        CHECK(at(p, Side - 3, 2) == NetworkView::CrossColor);
        CHECK(at(p, Side / 2, Side / 2) == NetworkView::CrossColor);
}

TEST_CASE("NetworkView: each state's lines") {
        NetStatus::Info i = eth();
        CHECK(all(i, false) == "eth0: connected, 100 Mbit/s full duplex|172.20.200.221/16, gateway 172.20.0.1|"
                               "DHCP from 172.20.0.1, lease 2 h|DNS 172.20.0.2|MAC e8:f6:0a:e2:e2:22|");
        // The menu's: with the lease's time left.
        CHECK(all(i, true) == "eth0: connected, 100 Mbit/s full duplex|172.20.200.221/16, gateway 172.20.0.1|"
                              "DHCP from 172.20.0.1, lease 2 h|Lease: 1 h 52 min left|DNS 172.20.0.2|"
                              "MAC e8:f6:0a:e2:e2:22|");
        // Static, half duplex, two DNS servers, no gateway.
        i.mode = NetStatus::Mode::Static;
        i.dhcp = NetStatus::Dhcp::None;
        i.fullDuplex = false;
        i.speedMbps = 10;
        i.gateway = 0;
        i.netmask = ip(255, 255, 255, 0);
        i.dns[1] = ip(8, 8, 8, 8);
        CHECK(all(i, true) == "eth0: connected, 10 Mbit/s half duplex|172.20.200.221/24, no gateway|static|"
                              "DNS 172.20.0.2, 8.8.8.8|MAC e8:f6:0a:e2:e2:22|");
        i.address = 0;
        CHECK(all(i, false) == "eth0: no address yet, 10 Mbit/s half duplex|static, no address set|"
                               "MAC e8:f6:0a:e2:e2:22|");
        i = eth();
        i.address = 0;
        i.dhcp = NetStatus::Dhcp::Asking;
        CHECK(all(i, false) == "eth0: no address yet, 100 Mbit/s full duplex|DHCP: asking for an address|"
                               "MAC e8:f6:0a:e2:e2:22|");
        i.link = false;
        CHECK(all(i, false) == "eth0: cable unplugged|MAC e8:f6:0a:e2:e2:22|");
        i.up = false;
        CHECK(all(i, false) == "eth0: off|MAC e8:f6:0a:e2:e2:22|");

        // A line too long for a menu item is cut, not overrun.
        i = eth();
        std::strcpy(i.name, "a-very-long-nam");
        char         l[NetworkView::MaxLines][NetworkView::LineBytes];
        const size_t n = NetworkView::lines(i, true, l);
        CHECK(n == NetworkView::MaxLines);
        CHECK(std::strlen(l[0]) == NetworkView::LineBytes - 1);
}

TEST_CASE("NetworkView: the tooltip and spans") {
        NetStatus::Info list[3] = {lo(), eth("eth0"), eth("eth1")};
        list[2].link = false;
        char              tip[384];
        const size_t      n = NetworkView::toolTip(Span<const NetStatus::Info>(list, 3), Span<char>(tip, sizeof(tip)));
        const std::string want = "eth0: connected, 100 Mbit/s full duplex\n  172.20.200.221/16, gateway 172.20.0.1\n"
                                 "  DHCP from 172.20.0.1, lease 2 h\n  DNS 172.20.0.2\n  MAC e8:f6:0a:e2:e2:22\n"
                                 "eth1: cable unplugged\n  MAC e8:f6:0a:e2:e2:22";
        CHECK(std::string(tip) == want);
        CHECK(n == want.size());
        CHECK(NetworkView::toolTip(Span<const NetStatus::Info>(list, 1), Span<char>(tip, sizeof(tip))) > 0);
        CHECK(std::string(tip) == "No network interfaces");
        char small[10];
        CHECK(NetworkView::toolTip(Span<const NetStatus::Info>(list, 3), Span<char>(small, sizeof(small))) == 9);
        CHECK(std::string(small) == "eth0: con");

        char                                    s[24];
        const std::pair<uint32_t, const char *> spans[] = {
                {0, "0 s"},           {59, "59 s"},   {60, "1 min"},      {3599, "59 min"},   {3600, "1 h"},
                {6720, "1 h 52 min"}, {86400, "1 d"}, {90000, "1 d 1 h"}, {90059, "1 d 1 h"},
        };
        for (const auto &[seconds, text] : spans) {
                NetworkView::formatSpan(seconds, s);
                CHECK(std::string(s) == text);
        }
}
