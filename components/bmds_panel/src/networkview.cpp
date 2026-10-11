/**
 * @file      networkview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tray's network icon's picture and text (devplan/network.md §17, §20).
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <bmds/ethernet.h>
#include <bmds/icon.h>

#include "networkview.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t Side = NetworkView::Side;
        using Lines = char[NetworkView::MaxLines][NetworkView::LineBytes];

        void fill(uint32_t (&out)[Side * Side], uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t argb) {
                for (uint16_t r = y; r < y + h && r < Side; ++r)
                        for (uint16_t c = x; c < x + w && c < Side; ++c) out[r * Side + c] = argb;
                return;
        }

        // An address in network order (as NetStatus has it) in host order (as Ipv4 takes it).
        uint32_t hostOrder(uint32_t network) {
                uint8_t b[4];
                std::memcpy(b, &network, sizeof(b));
                return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
        }

        void formatIp(uint32_t network, char (&out)[Ipv4::TextBytes + 1]) {
                Ipv4::format(hostOrder(network), out);
                return;
        }

        // A line, cut to fit.
        void put(Lines &out, size_t &n, const char *format, ...) __attribute__((format(printf, 3, 4)));
        void put(Lines &out, size_t &n, const char *format, ...) {
                if (n >= NetworkView::MaxLines) return;
                va_list args;
                va_start(args, format);
                std::vsnprintf(out[n++], NetworkView::LineBytes, format, args);
                va_end(args);
                return;
        }

} // namespace

NetworkView::State NetworkView::stateOf(const NetStatus::Info &info) {
        if (!info.up || info.mode == NetStatus::Mode::Off) return State::Off;
        if (!info.link) return State::NoLink;
        return info.address == 0 ? State::Acquiring : State::Connected;
}

NetworkView::State NetworkView::best(Span<const NetStatus::Info> interfaces) {
        State state = State::Off;
        for (const NetStatus::Info &info : interfaces) {
                if (info.loopback) continue;
                const State s = stateOf(info);
                if (uint8_t(s) > uint8_t(state)) state = s;
        }
        return state;
}

void NetworkView::draw(State state, uint32_t (&out)[Side * Side]) {
        fill(out, 0, 0, Side, Side, 0); // transparent round it
        // The socket: its body and edge, the hole with four pins at its top, the latch's notch.
        fill(out, 1, 2, 14, 12, EdgeColor);
        fill(out, 2, 3, 12, 10, BodyColor);
        fill(out, 3, 4, 10, 6, HoleColor);
        fill(out, 6, 10, 4, 2, HoleColor);
        for (uint16_t x : {4, 6, 9, 11}) fill(out, x, 4, 1, 2, PinColor);
        // Its lights.
        const uint32_t left = state == State::Connected   ? GreenLight
                              : state == State::Acquiring ? AmberLight
                                                          : DarkLight;
        const uint32_t right = state == State::Connected ? GreenLight : DarkLight;
        fill(out, LeftLightX, LightY, 2, 1, left);
        fill(out, RightLightX, LightY, 2, 1, right);
        if (state == State::Off || state == State::NoLink)
                for (uint32_t &p : out) p = halfAlpha(p);
        if (state == State::NoLink) {
                for (uint16_t i = 2; i < Side - 2; ++i) {
                        fill(out, i, i, 1, 1, CrossColor);
                        fill(out, uint16_t(Side - 1 - i), i, 1, 1, CrossColor);
                }
        }
        return;
}

void NetworkView::formatSpan(uint32_t seconds, char (&out)[24]) {
        const uint32_t days = seconds / 86400, hours = seconds / 3600 % 24, minutes = seconds / 60 % 60;
        if (days > 0) {
                if (hours > 0)
                        std::snprintf(out, sizeof(out), "%u d %u h", unsigned(days), unsigned(hours));
                else
                        std::snprintf(out, sizeof(out), "%u d", unsigned(days));
        } else if (hours > 0) {
                if (minutes > 0)
                        std::snprintf(out, sizeof(out), "%u h %u min", unsigned(hours), unsigned(minutes));
                else
                        std::snprintf(out, sizeof(out), "%u h", unsigned(hours));
        } else if (minutes > 0) {
                std::snprintf(out, sizeof(out), "%u min", unsigned(minutes));
        } else {
                std::snprintf(out, sizeof(out), "%u s", unsigned(seconds));
        }
        return;
}

size_t NetworkView::lines(const NetStatus::Info &info, bool leaseLeft, char (&out)[MaxLines][LineBytes]) {
        size_t      n = 0;
        const State state = stateOf(info);
        const bool  dhcp = info.mode == NetStatus::Mode::Dhcp;
        char        speed[32] = {};
        if (info.link)
                std::snprintf(speed, sizeof(speed), ", %u Mbit/s %s duplex", unsigned(info.speedMbps),
                              info.fullDuplex ? "full" : "half");
        switch (state) {
                case State::Off: put(out, n, "%s: off", info.name); break;
                case State::NoLink: put(out, n, "%s: cable unplugged", info.name); break;
                case State::Acquiring:
                        put(out, n, "%s: no address yet%s", info.name, speed);
                        put(out, n, "%s", dhcp ? "DHCP: asking for an address" : "static, no address set");
                        break;
                case State::Connected: {
                        char address[Ipv4::TextBytes + 1], gateway[Ipv4::TextBytes + 1];
                        formatIp(info.address, address);
                        formatIp(info.gateway, gateway);
                        const int prefix = Ipv4::prefixOf(hostOrder(info.netmask));
                        put(out, n, "%s: connected%s", info.name, speed);
                        if (info.gateway != 0)
                                put(out, n, "%s/%d, gateway %s", address, prefix, gateway);
                        else
                                put(out, n, "%s/%d, no gateway", address, prefix);
                        if (dhcp) {
                                char server[Ipv4::TextBytes + 1], length[24];
                                formatIp(info.dhcpServer, server);
                                formatSpan(info.leaseSeconds, length);
                                put(out, n, "DHCP from %s, lease %s", server, length);
                                if (leaseLeft) {
                                        char left[24];
                                        formatSpan(info.leaseLeftSeconds, left);
                                        put(out, n, "Lease: %s left", left);
                                }
                        } else {
                                put(out, n, "static");
                        }
                        if (info.dns[0] != 0) {
                                char first[Ipv4::TextBytes + 1], second[Ipv4::TextBytes + 1];
                                formatIp(info.dns[0], first);
                                formatIp(info.dns[1], second);
                                if (info.dns[1] != 0)
                                        put(out, n, "DNS %s, %s", first, second);
                                else
                                        put(out, n, "DNS %s", first);
                        }
                        break;
                }
        }
        MacAddress mac;
        std::memcpy(mac.bytes, info.mac, sizeof(mac.bytes));
        char text[MacAddress::TextBytes + 1];
        mac.toString(text);
        put(out, n, "MAC %s", text);
        return n;
}

size_t NetworkView::toolTip(Span<const NetStatus::Info> interfaces, Span<char> out) {
        if (out.size() == 0) return 0;
        size_t used = 0;
        auto   append = [&](const char *text) {
                const size_t len = std::strlen(text);
                const size_t room = out.size() - 1 - used;
                const size_t n = len < room ? len : room;
                std::memcpy(out.data() + used, text, n);
                used += n;
        };
        for (const NetStatus::Info &info : interfaces) {
                if (info.loopback) continue;
                Lines        l;
                const size_t count = lines(info, false, l);
                for (size_t i = 0; i < count; ++i) {
                        if (used > 0) append("\n");
                        if (i > 0) append("  ");
                        append(l[i]);
                }
        }
        if (used == 0) append("No network interfaces");
        out[used] = '\0';
        return used;
}

BMDS_NAMESPACE_END
