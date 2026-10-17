/**
 * @file      pagestatus.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pages' status (devplan/controlpanel.md §7): the system, the clock, the screen, the network,
 * the SSH server's address, the gamepads.
 */

#include <cstdio>
#include <bmds/audiodevice.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/memory.h>
#include <bmds/netstatus.h>
#include <bmds/pads.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/timesync.h>

#include "pagestatus.h"

BMDS_NAMESPACE_BEGIN

namespace {

        void ipv4(uint32_t address, char (&out)[16]) {
                const auto *b = reinterpret_cast<const uint8_t *>(&address); // network order
                std::snprintf(out, sizeof(out), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                return;
        }

        int prefixOf(uint32_t netmask) {
                const auto    *b = reinterpret_cast<const uint8_t *>(&netmask);
                const uint32_t m = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
                int            n = 0;
                for (uint32_t bit = 0x80000000u; bit != 0 && (m & bit) != 0; bit >>= 1) ++n;
                return n;
        }

        void sizeText(size_t bytes, char (&out)[24]) {
                if (bytes >= 10u * 1024 * 1024)
                        std::snprintf(out, sizeof(out), "%zu MB", bytes / (1024 * 1024));
                else
                        std::snprintf(out, sizeof(out), "%zu KB", bytes / 1024);
                return;
        }

        void general(String &out) {
                const SystemInfo info = System::info();
                (void)out.appendf("BMDS %s (%s) on the %s, %u cores at %u MHz\n", info.bmdsVersion, info.buildVersion,
                                  info.boardName, info.cores, info.cpuMhz);
                const unsigned long s = static_cast<unsigned long>(System::uptimeUs() / 1'000'000);
                char                up[40];
                if (s >= 86400)
                        std::snprintf(up, sizeof(up), "%lu d %lu h", s / 86400, s / 3600 % 24);
                else if (s >= 3600)
                        std::snprintf(up, sizeof(up), "%lu h %02lu min", s / 3600, s / 60 % 60);
                else
                        std::snprintf(up, sizeof(up), "%lu min", s / 60);
                (void)out.appendf("Serial number %s. Up %s\n",
                                  System::serial()[0] != '\0' ? System::serial() : "(none)", up);
                const MemStats in = Memory::stats(MemSpace::Internal);
                const MemStats ps = Memory::stats(MemSpace::Psram);
                char           a[24], b[24], c[24], d[24];
                sizeText(in.freeBytes, a);
                sizeText(in.totalBytes, b);
                sizeText(ps.freeBytes, c);
                sizeText(ps.totalBytes, d);
                (void)out.appendf("Memory free: %s of %s internal, %s of %s PSRAM", a, b, c, d);
                return;
        }

        // @p ns signed, in the unit that suits it ("+1.23 ms", "-480 ns").
        void duration(int64_t ns, bool sign, char (&out)[24]) {
                const char    *s = !sign ? "" : ns < 0 ? "-" : "+";
                const uint64_t a = uint64_t(ns < 0 ? -ns : ns);
                if (a < 1000)
                        std::snprintf(out, sizeof(out), "%s%llu ns", s, static_cast<unsigned long long>(a));
                else if (a < 1'000'000)
                        std::snprintf(out, sizeof(out), "%s%.1f us", s, double(a) / 1e3);
                else if (a < 1'000'000'000)
                        std::snprintf(out, sizeof(out), "%s%.2f ms", s, double(a) / 1e6);
                else
                        std::snprintf(out, sizeof(out), "%s%.1f s", s, double(a) / 1e9);
                return;
        }

        void ntp(String &out) {
                const TimeSync::Ntp n = TimeSync::ntp();
                if (!n.enabled) {
                        (void)out.append("\nNTP: off");
                        return;
                }
                if (!n.synced || n.server[0] == '\0') {
                        (void)out.appendf("\nNTP: not synchronized yet (%u of %u servers answering)", n.reachable,
                                          n.servers);
                        return;
                }
                char offset[24], delay[24];
                duration(n.offsetNs, true, offset);
                duration(n.delayNs, false, delay);
                (void)out.appendf("\nNTP: synchronized with %s (stratum %u), offset %s, round trip %s", n.server,
                                  n.stratum, offset, delay);
                if (n.sinceAnswerNs >= 0)
                        (void)out.appendf(", %lld s ago", static_cast<long long>(n.sinceAnswerNs / 1'000'000'000));
                return;
        }

        void ptp(String &out) {
                const TimeSync::Ptp p = TimeSync::ptp();
                if (!p.enabled) return; // off: nothing to say (its settings are hidden too)
                if (!p.running) {
                        (void)out.appendf("\nPTP on %s: not running (%s)", p.interface, p.error.desc());
                        return;
                }
                (void)out.appendf("\nPTP on %s: %s for %lld s", p.interface, p.state,
                                  static_cast<long long>(p.stateForNs / 1'000'000'000));
                if (p.master[0] != '\0') (void)out.appendf(", master %s", p.master);
                if (p.haveOffset) {
                        char offset[24], path[24];
                        duration(p.offsetNs, true, offset);
                        duration(p.pathDelayNs, false, path);
                        (void)out.appendf(", offset %s, path %s", offset, path);
                }
                if (p.systemFollows) (void)out.append("; the system clock follows it");
                if (p.ptpFollows) (void)out.append("; it follows the system clock");
                return;
        }

        void sound(String &out) {
                bool any = false;
                for (size_t i = 0; i < AudioDevice::MaxDevices; ++i) {
                        const Result<AudioDevice *> d = AudioDevice::device(i);
                        if (d.isError() || d.value() == nullptr) continue;
                        const AudioDevice::Status s = d.value()->status();
                        if (any) (void)out.append('\n');
                        any = true;
                        (void)out.appendf("dsp%u: ", unsigned(i));
                        if (s.state == AudioDevice::State::Running) {
                                (void)out.appendf("playing at %lu Hz, %lu-frame periods; %u stream%s",
                                                  static_cast<unsigned long>(s.rateHz),
                                                  static_cast<unsigned long>(s.periodFrames), unsigned(s.streams),
                                                  s.streams == 1 ? "" : "s");
                        } else if (s.state == AudioDevice::State::Faulted) {
                                (void)out.append("faulted (audio status says why)");
                        } else {
                                (void)out.append("idle (it starts when something plays)");
                        }
                        (void)out.appendf(" on the %s", d.value()->codec().name());
                        if (s.underruns != 0 || s.late != 0)
                                (void)out.appendf("\n  %lu underruns, %lu late periods since boot",
                                                  static_cast<unsigned long>(s.underruns),
                                                  static_cast<unsigned long>(s.late));
                }
                if (!any) (void)out.append("No audio device");
                return;
        }

        void time(String &out) {
                const LocalTime local = Clock::localTime(Clock::now());
                if (formatTime(out, StringView("%A, %B %-d, %Y   %H:%M:%S %Z"), local).isError()) out.clear();
                String zone(MemSpace::Psram);
                (void)SysConf::get(StringView("clock/zone"), zone);
                (void)out.appendf("\nTime zone %s", zone.cStr());
                ntp(out);
                ptp(out);
                return;
        }

        void network(String &out) {
                NetStatus::Info info;
                bool            any = false;
                for (size_t i = 0; NetStatus::info(i, info).isOk(); ++i) {
                        if (info.loopback) continue;
                        if (any) (void)out.append('\n');
                        any = true;
                        if (!info.up) {
                                (void)out.appendf("%s: off", info.name);
                                continue;
                        }
                        if (!info.link) {
                                (void)out.appendf("%s: no link (is the cable in?)", info.name);
                                continue;
                        }
                        (void)out.appendf("%s: connected, %u Mbps %s duplex", info.name, info.speedMbps,
                                          info.fullDuplex ? "full" : "half");
                        if (info.address == 0) {
                                (void)out.appendf("\n  No address yet%s",
                                                  info.mode == NetStatus::Mode::Dhcp ? " (asking DHCP)" : "");
                                continue;
                        }
                        char a[16], g[16];
                        ipv4(info.address, a);
                        ipv4(info.gateway, g);
                        (void)out.appendf("\n  Address %s/%d, router %s", a, prefixOf(info.netmask),
                                          info.gateway != 0 ? g : "none");
                        if (info.mode == NetStatus::Mode::Dhcp && info.leaseSeconds > 0)
                                (void)out.appendf(", DHCP lease %lu min left",
                                                  static_cast<unsigned long>(info.leaseLeftSeconds / 60));
                        if (info.dns[0] != 0) {
                                char d[16];
                                ipv4(info.dns[0], d);
                                (void)out.appendf("\n  DNS from DHCP %s", d);
                                if (info.dns[1] != 0) {
                                        ipv4(info.dns[1], d);
                                        (void)out.appendf(", %s", d);
                                }
                        }
                }
                if (!any) (void)out.append("No network interface");
                return;
        }

        void sharing(String &out) {
                String     on(MemSpace::Psram), port(MemSpace::Psram);
                const bool running =
                        SysConf::get(StringView("net/ssh/server"), on).isOk() && on.view() == StringView("true");
                if (!running) {
                        (void)out.append("The SSH server is off");
                        return;
                }
                (void)SysConf::get(StringView("net/ssh/port"), port);
                char host[HostName::MaxBytes + 1];
                (void)SysConf::hostname(host);
                const bool p22 = port.view() == StringView("22");
                (void)out.appendf("Connect with: ssh %s%s root@%s.local", p22 ? "" : "-p ", p22 ? "" : port.cStr(),
                                  host);
                NetStatus::Info info;
                for (size_t i = 0; NetStatus::info(i, info).isOk(); ++i) {
                        if (info.loopback || info.address == 0) continue;
                        char a[16];
                        ipv4(info.address, a);
                        (void)out.appendf("\n  or root@%s (%s)", a, info.name);
                }
                return;
        }

        void pads(String &out) {
                Pads::Info   list[4];
                const size_t n = Pads::list(Span<Pads::Info>(list));
                if (n == 0) {
                        (void)out.append("No gamepad plugged in");
                        return;
                }
                for (size_t i = 0; i < n; ++i)
                        (void)out.appendf("%spad%u: %s (%04x:%04x, %s mapping)", i > 0 ? "\n" : "", list[i].number,
                                          list[i].name[0] != '\0' ? list[i].name : "a gamepad", list[i].vendor,
                                          list[i].product, Pads::originName(list[i].origin));
                return;
        }

} // namespace

bool PageStatus::has(StringView id) {
        return id == StringView("general") || id == StringView("time") || id == StringView("display") ||
               id == StringView("network") || id == StringView("sharing") || id == StringView("input") ||
               id == StringView("sound");
}

bool PageStatus::ticks(StringView id) {
        return id == StringView("general") || id == StringView("time") || id == StringView("network") ||
               id == StringView("input") || id == StringView("sound");
}

void PageStatus::text(StringView id, const Rect &screen, String &out) {
        out.clear();
        if (id == StringView("general")) {
                general(out);
        } else if (id == StringView("time")) {
                time(out);
        } else if (id == StringView("display")) {
                (void)out.appendf("The desktop: %u x %u pixels", screen.width, screen.height);
        } else if (id == StringView("network")) {
                network(out);
        } else if (id == StringView("sharing")) {
                sharing(out);
        } else if (id == StringView("input")) {
                pads(out);
        } else if (id == StringView("sound")) {
                sound(out);
        }
        return;
}

int PageStatus::leasedInterface() {
        NetStatus::Info info;
        for (size_t i = 0; NetStatus::info(i, info).isOk(); ++i)
                if (!info.loopback && info.mode == NetStatus::Mode::Dhcp && info.leaseSeconds > 0) return int(i);
        return -1;
}

BMDS_NAMESPACE_END
