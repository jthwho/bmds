/**
 * @file      ptpcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `ptp` command (devplan/network.md §31c): the PTP port's state, its master, the
 * measurements, which clock follows which and the foreign masters heard; `-v` the counts.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/shell.h>

#include "ptpservice.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;
        using State = Ptp::Port::State;

        // As linuxptp prints them: 001b21.fffe.aabbcc.
        void formatIdentity(const Ptp::ClockIdentity &id, char (&out)[24]) {
                const uint8_t *b = id.bytes;
                std::snprintf(out, sizeof(out), "%02x%02x%02x.%02x%02x.%02x%02x%02x", b[0], b[1], b[2], b[3], b[4],
                              b[5], b[6], b[7]);
                return;
        }

        // IEEE 1588-2008 table 6.
        void formatAccuracy(uint8_t code, char (&out)[16]) {
                static constexpr const char *Text[] = {"25 ns", "100 ns", "250 ns", "1 us", "2.5 us", "10 us",
                                                       "25 us", "100 us", "250 us", "1 ms", "2.5 ms", "10 ms",
                                                       "25 ms", "100 ms", "250 ms", "1 s",  "10 s",   "over 10 s"};
                if (code >= 0x20 && code <= 0x31)
                        std::snprintf(out, sizeof(out), "%s", Text[code - 0x20]);
                else if (code == 0xfe)
                        std::snprintf(out, sizeof(out), "unknown");
                else
                        std::snprintf(out, sizeof(out), "0x%02x", code);
                return;
        }

        // IEEE 1588-2008 table 7.
        const char *timeSourceName(uint8_t source) {
                switch (source) {
                        case 0x10: return "atomic clock";
                        case 0x20: return "GPS";
                        case 0x30: return "terrestrial radio";
                        case 0x40: return "PTP";
                        case 0x50: return "NTP";
                        case 0x60: return "hand set";
                        case 0x90: return "other";
                        case 0xa0: return "internal oscillator";
                }
                return "unknown source";
        }

        // "+0.123 us", or "+12.345 ms" from 10 ms up.
        void formatOffset(int64_t ns, bool sign, char (&out)[24]) {
                const uint64_t m = ns < 0 ? 0 - static_cast<uint64_t>(ns) : static_cast<uint64_t>(ns);
                const char    *s = !sign ? "" : ns < 0 ? "-" : "+";
                if (m >= 10'000'000)
                        std::snprintf(out, sizeof(out), "%s%llu.%03llu ms", s,
                                      static_cast<unsigned long long>(m / 1'000'000),
                                      static_cast<unsigned long long>(m % 1'000'000 / 1000));
                else
                        std::snprintf(out, sizeof(out), "%s%llu.%03llu us", s,
                                      static_cast<unsigned long long>(m / 1000),
                                      static_cast<unsigned long long>(m % 1000));
                return;
        }

        // "45 s", "2 min 13 s", "3 h 2 min".
        void formatDuration(int64_t ns, char (&out)[24]) {
                const unsigned long long s = static_cast<unsigned long long>(ns < 0 ? 0 : ns / 1'000'000'000);
                if (s < 60)
                        std::snprintf(out, sizeof(out), "%llu s", s);
                else if (s < 3600)
                        std::snprintf(out, sizeof(out), "%llu min %llu s", s / 60, s % 60);
                else
                        std::snprintf(out, sizeof(out), "%llu h %llu min", s / 3600, s % 3600 / 60);
                return;
        }

        bool sameClock(const Ptp::ClockIdentity &a, const Ptp::ClockIdentity &b) {
                return std::memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
        }

        void showForeign(CommandContext &ctx, const PtpService::Status &s) {
                ctx.print("  Foreign master            Port Class Pri1 Pri2 Steps Accuracy\n");
                char id[24], accuracy[16];
                for (size_t i = 0; i < s.foreignCount; ++i) {
                        const Ptp::Port::Foreign &f = s.foreign[i];
                        const Ptp::Dataset       &d = f.dataset;
                        const bool                parent = s.port.state != State::Master && f.port == s.port.parent &&
                                            s.port.state != State::Listening;
                        formatIdentity(f.port.clock, id);
                        formatAccuracy(d.quality.accuracy, accuracy);
                        ctx.printf("%c %-25s %4u %5u %4u %4u %5u %8s%s\n", parent ? '*' : ' ', id, f.port.port,
                                   d.quality.clockClass, d.priority1, d.priority2, d.stepsRemoved, accuracy,
                                   f.qualified ? "" : " (not qualified yet)");
                }
                char own[32];
                formatIdentity(s.identity.clock, id);
                std::snprintf(own, sizeof(own), "%s (this)", id);
                formatAccuracy(s.config.quality.accuracy, accuracy);
                ctx.printf("%c %-25s %4u %5u %4u %4u %5u %8s\n", s.port.state == State::Master ? '*' : ' ', own,
                           s.identity.port, s.config.slaveOnly ? 255u : s.config.quality.clockClass, s.config.priority1,
                           s.config.priority2, 0u, accuracy);
                return;
        }

        void showCounts(CommandContext &ctx, const PtpService::Status &s) {
                static constexpr Ptp::MessageType Types[] = {Ptp::MessageType::Sync, Ptp::MessageType::DelayReq,
                                                             Ptp::MessageType::FollowUp, Ptp::MessageType::DelayResp,
                                                             Ptp::MessageType::Announce};
                ctx.print("Messages          Sent  Received\n");
                uint32_t other = 0;
                for (size_t t = 0; t < 16; ++t) other += s.counts.received[t];
                for (Ptp::MessageType t : Types) {
                        const size_t n = static_cast<size_t>(t);
                        other -= s.counts.received[n];
                        ctx.printf("  %-12s %8lu %9lu\n", Ptp::typeName(t),
                                   static_cast<unsigned long>(s.counts.sent[n]),
                                   static_cast<unsigned long>(s.counts.received[n]));
                }
                ctx.printf("  %-12s %8s %9lu\n", "other", "-", static_cast<unsigned long>(other));
                const Ptp::Port::Counts &c = s.counts;
                ctx.printf("Ignored %lu, send failures %lu, announce timeouts %lu, state changes %lu, master changes "
                           "%lu, steps %lu, Follow_Ups missed %lu\n",
                           static_cast<unsigned long>(c.ignored), static_cast<unsigned long>(c.sendFailures),
                           static_cast<unsigned long>(c.announceTimeouts), static_cast<unsigned long>(c.stateChanges),
                           static_cast<unsigned long>(c.parentChanges), static_cast<unsigned long>(c.steps),
                           static_cast<unsigned long>(c.followUpsMissed));
                const NetFlow::Counters &f = s.flows;
                ctx.printf("Flows: received %llu (dropped %lu), sent %llu (failed %lu), stamps %lu (lost %lu)\n",
                           static_cast<unsigned long long>(f.rxPackets), static_cast<unsigned long>(f.rxDropped),
                           static_cast<unsigned long long>(f.txPackets), static_cast<unsigned long>(f.txErrors),
                           static_cast<unsigned long>(f.txStamps), static_cast<unsigned long>(f.txStampsLost));
                return;
        }

        int show(CommandContext &ctx, bool verbose) {
                auto *s = static_cast<PtpService::Status *>(Memory::alloc(MemSpace::Psram, sizeof(PtpService::Status)));
                if (s == nullptr) {
                        ctx.eprint("ptp: out of memory\n");
                        return Shell::ExitFailure;
                }
                new (s) PtpService::Status();
                PtpService::status(*s);
                const int rc = [&] {
                        if (!s->enabled) {
                                ctx.print("PTP: off (net/ptp/enabled)\n");
                                return Shell::ExitSuccess;
                        }
                        if (!s->running) {
                                ctx.printf("PTP: not running on %s: %s\n", s->interface,
                                           CommandContext::errorText(s->error));
                                return Shell::ExitSuccess;
                        }
                        char since[24], id[24], gm[24], accuracy[16];
                        formatDuration(s->stateForNs, since);
                        ctx.printf("PTP: %s on %s (%s, %s, domain %u) for %s\n", Ptp::Port::stateName(s->port.state),
                                   s->interface, s->l2 ? "l2" : "udp", s->defaultProfile ? "default" : "media",
                                   s->config.domain, since);
                        const Ptp::Dataset &g = s->port.grandmaster;
                        formatAccuracy(g.quality.accuracy, accuracy);
                        if (s->port.state == State::Master) {
                                ctx.printf("Grandmaster: this clock, class %u, accuracy %s, %s\n", g.quality.clockClass,
                                           accuracy, timeSourceName(s->config.timeSource));
                        } else if (s->port.state != State::Listening) {
                                formatIdentity(s->port.parent.clock, id);
                                formatIdentity(g.grandmaster, gm);
                                if (sameClock(s->port.parent.clock, g.grandmaster))
                                        ctx.printf("Master: %s port %u; grandmaster the same, class %u, accuracy %s, "
                                                   "priority %u/%u\n",
                                                   id, s->port.parent.port, g.quality.clockClass, accuracy, g.priority1,
                                                   g.priority2);
                                else
                                        ctx.printf("Master: %s port %u; grandmaster %s, class %u, accuracy %s, "
                                                   "priority %u/%u, %u steps away\n",
                                                   id, s->port.parent.port, gm, g.quality.clockClass, accuracy,
                                                   g.priority1, g.priority2, g.stepsRemoved);
                        }
                        const double   ppb = s->port.frequencyPpb;
                        const uint64_t m = static_cast<uint64_t>(std::llround(std::fabs(ppb)));
                        char           frequency[32];
                        std::snprintf(frequency, sizeof(frequency), "%c%llu.%03llu ppm", ppb < 0 ? '-' : '+',
                                      static_cast<unsigned long long>(m / 1000),
                                      static_cast<unsigned long long>(m % 1000));
                        if (s->port.haveOffset) {
                                static constexpr const char *Servo[] = {"unlocked", "stepping", "locked"};
                                char                         offset[24], delay[24];
                                formatOffset(s->port.offsetNs, true, offset);
                                formatOffset(s->port.meanPathDelayNs, false, delay);
                                ctx.printf("Offset %s, path delay %s, frequency %s, servo %s\n", offset, delay,
                                           frequency, Servo[static_cast<int>(s->port.servo)]);
                        } else {
                                ctx.printf("Frequency %s\n", frequency);
                        }
                        char follow[24];
                        formatOffset(s->followOffsetNs, true, follow);
                        const char *who =
                                s->follow == PtpService::Follow::SystemFollows ? "the system clock follows ptpclock0"
                                : s->follow == PtpService::Follow::PtpFollows  ? "ptpclock0 follows the system clock"
                                                                               : "neither clock follows the other";
                        const char *why = s->arbitraryTimescale ? "the master's time is UTC, not PTP's timescale"
                                          : s->utcFromMaster    ? "the master's"
                                          : s->utcFromFile      ? "the leap-second list's"
                                                                : "assumed: no leap-second list";
                        ctx.printf("UTC offset %d s (%s); %s", s->utcOffset, why, who);
                        if (s->follow != PtpService::Follow::None) ctx.printf(" (last offset %s)", follow);
                        ctx.print("\n");
                        showForeign(ctx, *s);
                        if (verbose) showCounts(ctx, *s);
                        return Shell::ExitSuccess;
                }();
                s->~Status();
                Memory::free(s);
                return rc;
        }

        int runPtp(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return show(ctx, false);
                if (args.size() == 2 && std::strcmp(args[1], "-v") == 0) return show(ctx, true);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(ptp, runPtp, "[-v]",
                   "Show PTP: the port's state, its master, the offset and path delay, which clock follows which, "
                   "and the foreign masters heard; -v: the message counts")

BMDS_NAMESPACE_END
