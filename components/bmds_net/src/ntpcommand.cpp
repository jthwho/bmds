/**
 * @file      ntpcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `ntp` command (devplan/network.md §22): the NTP client's state and its
 * servers, and `ntp poll` to ask them all now.
 */

#include <cstdio>
#include <cstring>
#include <lwip/def.h>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/ethernet.h>
#include <bmds/shell.h>

#include "ntpclient.h"
#include "ptpservice.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // "+1.234 ms", or "-12.345 s" from 10 s up.
        void formatMs(int64_t ns, char (&out)[24]) {
                const uint64_t m = ns < 0 ? 0 - static_cast<uint64_t>(ns) : static_cast<uint64_t>(ns);
                const char     sign = ns < 0 ? '-' : '+';
                if (m >= 10 * uint64_t(Timestamp::NsPerSecond)) {
                        std::snprintf(out, sizeof(out), "%c%llu.%03llu s", sign,
                                      static_cast<unsigned long long>(m / Timestamp::NsPerSecond),
                                      static_cast<unsigned long long>(m % Timestamp::NsPerSecond / 1'000'000));
                } else {
                        std::snprintf(out, sizeof(out), "%c%llu.%03llu ms", sign,
                                      static_cast<unsigned long long>(m / 1'000'000),
                                      static_cast<unsigned long long>(m % 1'000'000 / 1000));
                }
                return;
        }

        // As formatMs(), without the sign (delays and jitters).
        void formatSpan(int64_t ns, char (&out)[24]) {
                formatMs(ns, out);
                std::memmove(out, out + 1, std::strlen(out));
                return;
        }

        int show(CommandContext &ctx) {
                NtpClient::Status s; // about 800 bytes
                NtpClient::status(s);
                if (!s.enabled) {
                        ctx.print("NTP: off (net/ntp/enabled)\n");
                        return Shell::ExitSuccess;
                }
                const NtpClient::ServerStatus *chosen = nullptr;
                for (size_t i = 0; i < s.count; ++i)
                        if (s.servers[i].chosen) chosen = &s.servers[i];
                if (s.count == 0) {
                        ctx.print("NTP: no servers (net/ntp/servers, or DHCP's)\n");
                        return Shell::ExitSuccess;
                }
                const char *ptp = PtpService::ownsSystemClock() ? " (the clock follows PTP)" : "";
                if (s.synced && chosen != nullptr)
                        ctx.printf("NTP: synced to %s, stratum %u, polling every %d s%s\n", chosen->name, s.stratum,
                                   1 << s.poll, ptp);
                else
                        ctx.printf("NTP: not synced, polling every %d s%s\n", 1 << s.poll, ptp);
                const ClockStatus clock = Clock::status();
                char              offset[24], delay[24], jitter[24];
                if (s.synced) {
                        formatMs(s.offsetNs, offset);
                        formatSpan(s.delayNs, delay);
                        formatSpan(s.jitterNs, jitter);
                        ctx.printf("Last offset %s, delay %s, jitter %s\n", offset, delay, jitter);
                }
                const int32_t  ppb = clock.frequencyPpb;
                const uint32_t m = ppb < 0 ? 0 - static_cast<uint32_t>(ppb) : static_cast<uint32_t>(ppb);
                ctx.printf("Clock: source %s, frequency %c%lu.%03lu ppm\n", clockSourceName(clock.source),
                           ppb < 0 ? '-' : '+', static_cast<unsigned long>(m / 1000),
                           static_cast<unsigned long>(m % 1000));
                ctx.print("  Server               Address          St Reach   Last     Offset      Delay     Jitter\n");
                for (size_t i = 0; i < s.count; ++i) {
                        const NtpClient::ServerStatus &v = s.servers[i];
                        char                           name[NetNames::MaxHostName + 8];
                        std::snprintf(name, sizeof(name), "%s%s", v.name, v.fromDhcp ? " (DHCP)" : "");
                        char address[Ipv4::TextBytes + 1] = "-";
                        if (v.address != 0)
                                Ipv4::format(lwip_ntohl(v.address), address);
                        else if (v.resolving)
                                std::strcpy(address, "(looking up)");
                        char last[16] = "-";
                        if (v.sinceAnswerNs >= 0)
                                std::snprintf(
                                        last, sizeof(last), "%llu s",
                                        static_cast<unsigned long long>(v.sinceAnswerNs / Timestamp::NsPerSecond));
                        const char mark = v.chosen ? '*' : v.denied ? 'x' : ' ';
                        if (v.samples != 0) {
                                formatMs(v.offsetNs, offset);
                                formatSpan(v.delayNs, delay);
                                formatSpan(v.jitterNs, jitter);
                        } else {
                                std::strcpy(offset, "-");
                                std::strcpy(delay, "-");
                                std::strcpy(jitter, "-");
                        }
                        ctx.printf("%c %-20s %-15s %3u   %03o %6s %10s %10s %10s\n", mark, name, address, v.stratum,
                                   v.reach, last, offset, delay, jitter);
                }
                ctx.printf("Requests %lu, replies %lu, rejected %lu\n", static_cast<unsigned long>(s.requests),
                           static_cast<unsigned long>(s.replies), static_cast<unsigned long>(s.rejected));
                return Shell::ExitSuccess;
        }

        int runNtp(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return show(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "poll") == 0) {
                        const Error err = NtpClient::burst();
                        if (err == Error::InvalidState) {
                                ctx.eprint("ntp: off (net/ntp/enabled)\n");
                                return Shell::ExitFailure;
                        }
                        if (err.isError()) {
                                ctx.eprintf("ntp: %s\n", CommandContext::errorText(err));
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(ntp, runNtp, "[poll]",
                   "Show the NTP client: whether the clock follows a server, the offset, and each server; poll: ask "
                   "them all now")

BMDS_NAMESPACE_END
