/**
 * @file      statscommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `stats` command (devplan/sysmon.md §3.5). Its output format is a userspace interface: keep
 * the columns and their order, and add new ones only at the end.
 */

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/boots.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/string.h>
#include <bmds/command.h>
#include <bmds/shell.h>
#include <bmds/stats.h>
#include "keptcounters.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // A value with its unit, or its state's label; "-" if unavailable.
        void formatValue(char (&out)[48], const Stats::Info &info, int64_t value) {
                if (value == Stat::Unavailable) {
                        std::snprintf(out, sizeof(out), "-");
                        return;
                }
                if (info.desc.kind == Stat::Kind::State) {
                        const StringView label = Stat::label(info.desc.labels, value);
                        if (!label.isEmpty()) {
                                std::snprintf(out, sizeof(out), "%.*s", int(label.size()), label.data());
                                return;
                        }
                }
                const char *unit = Stat::unitName(info.desc.unit);
                std::snprintf(out, sizeof(out), "%" PRId64 "%s%s", value, unit[0] != '\0' ? " " : "", unit);
                return;
        }

        int showCounters(CommandContext &ctx, const char *pattern) {
                ctx.printf("%-36s %20s %16s %s\n", "COUNTER", "VALUE", "RATE/S", "HEALTH");
                Stats::Info info;
                for (size_t i = 0; Stats::info(i, info).isOk(); ++i) {
                        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                        if (pattern != nullptr && !Stat::Overrides::matches(pattern, info.name)) continue;
                        char value[48];
                        char rate[48];
                        formatValue(value, info, info.value);
                        if (info.desc.kind == Stat::Kind::Total)
                                formatValue(rate, info, info.rate);
                        else
                                std::snprintf(rate, sizeof(rate), "%s", "");
                        ctx.printf("%-36s %20s %16s %s\n", info.name, value, rate,
                                   info.judged ? Stat::healthName(info.health) : "-");
                }
                return Shell::ExitSuccess;
        }

        int showRules(CommandContext &ctx, const char *pattern) {
                ctx.printf("%-36s %-6s %-8s %12s %12s %5s %s\n", "COUNTER", "KIND", "RULE", "WARN", "CRIT", "HOLD",
                           "HELP");
                Stats::Info info;
                for (size_t i = 0; Stats::info(i, info).isOk(); ++i) {
                        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                        if (pattern != nullptr && !Stat::Overrides::matches(pattern, info.name)) continue;
                        const Stat::Limits &l = info.limits;
                        char                warn[24] = "-";
                        char                crit[24] = "-";
                        if (l.rule == Stat::Rule::Zero) {
                                std::snprintf(l.health == Stat::Health::Critical ? crit : warn, 24, "any");
                        } else if (l.rule != Stat::Rule::None) {
                                if (l.warn != Stat::NoLevel) std::snprintf(warn, sizeof(warn), "%" PRId64, l.warn);
                                if (l.crit != Stat::NoLevel) std::snprintf(crit, sizeof(crit), "%" PRId64, l.crit);
                        }
                        const bool event = l.rule == Stat::Rule::Zero ||
                                           (l.rule == Stat::Rule::Above && info.desc.kind == Stat::Kind::Peak);
                        const unsigned hold = l.hold != 0 || !event ? l.hold : Stat::DefaultEventHold;
                        ctx.printf("%-36s %-6s %-8s %12s %12s %5u %s\n", info.name, Stat::kindName(info.desc.kind),
                                   Stat::ruleName(l.rule), warn, crit, hold, info.desc.help);
                }
                return Shell::ExitSuccess;
        }

        int showProblems(CommandContext &ctx) {
                Stat::ProblemLog::Episode episodes[8];
                uint64_t                  after = 0;
                const int64_t             nowMs = int64_t(Clock::monotonicNs() / 1000000);
                bool                      any = false;
                for (;;) {
                        const size_t n = Stats::problems(after, Span<Stat::ProblemLog::Episode>(episodes, 8));
                        if (n == 0) break;
                        for (size_t i = 0; i < n; ++i) {
                                const Stat::ProblemLog::Episode &e = episodes[i];
                                after = e.changed;
                                if (!any)
                                        ctx.printf("%-8s %-6s %10s %10s %10s %s\n", "HEALTH", "STATE", "AGO", "LASTED",
                                                   "COUNT", "COUNTER");
                                any = true;
                                const char *state = e.isOpen()                                   ? "open"
                                                    : (e.flags & Stat::ProblemLog::Earlier) != 0 ? "before"
                                                                                                 : "closed";
                                ctx.printf("%-8s %-6s %9" PRId64 "s %9" PRId64 "s %10" PRIu64 " %s (%" PRId64
                                           " to %" PRId64 ")\n",
                                           Stat::healthName(e.health), state, (nowMs - e.startMs) / 1000,
                                           (e.endMs - e.startMs) / 1000, e.count, e.name, e.firstValue, e.lastValue);
                        }
                }
                if (!any) ctx.printf("No problems seen%s.\n", Stats::watchers() == 0 ? " (nothing is watching)" : "");
                return Shell::ExitSuccess;
        }

        // The counters kept across boots (devplan/keptstate.md §5).
        int showKept(CommandContext &ctx) {
                const KeptCounters::Summary sum = KeptCounters::summary();
                String                      since(MemSpace::Psram);
                Timestamp                   started;
                if (Boots::wallTime(sum.startUs, started))
                        (void)formatTime(since, "%Y-%m-%d %H:%M:%S", ctx.displayZone().localTime(started));
                else
                        (void)since.append("this boot's start");
                ctx.printf("Kept since %s, %u boot%s.\n", since.data(), unsigned(sum.boots), sum.boots == 1 ? "" : "s");
                ctx.printf("%-36s %-5s %20s %20s\n", "COUNTER", "KEPT", "NOW", "ALL BOOTS");
                KeptCounters::Row rows[32];
                const size_t      n = KeptCounters::rows(Span<KeptCounters::Row>(rows, 32));
                for (size_t i = 0; i < n; ++i) {
                        const KeptCounters::Row &r = rows[i];
                        char                     now[24] = "-";
                        char                     all[24] = "-";
                        if (r.thisBoot != Stat::Unavailable) std::snprintf(now, sizeof(now), "%" PRId64, r.thisBoot);
                        if (r.allBoots != Stat::Unavailable) std::snprintf(all, sizeof(all), "%" PRId64, r.allBoots);
                        const char *mode = r.mode == KeptCounters::Mode::Low    ? "low"
                                           : r.mode == KeptCounters::Mode::High ? "high"
                                                                                : "total";
                        ctx.printf("%-36s %-5s %20s %20s\n", r.name, mode, now, all);
                }
                return Shell::ExitSuccess;
        }

        int runStats(CommandContext &ctx) {
                const Args args = ctx.args();
                size_t     at = 1;
                long       watchS = 0;
                if (at + 1 < args.size() && std::strcmp(args[at], "-w") == 0) {
                        char *end = nullptr;
                        watchS = std::strtol(args[at + 1], &end, 10);
                        if (end == args[at + 1] || *end != '\0' || watchS < 1 || watchS > 3600) return ctx.usage();
                        at += 2;
                }
                const char *word = at < args.size() ? args[at] : nullptr;
                if (word != nullptr && std::strcmp(word, "kept") == 0 && watchS == 0) {
                        if (at + 1 == args.size()) return showKept(ctx);
                        if (at + 2 == args.size() && std::strcmp(args[at + 1], "clear") == 0) {
                                KeptCounters::clear();
                                return Shell::ExitSuccess;
                        }
                        return ctx.usage();
                }
                if (word != nullptr && std::strcmp(word, "clear") == 0 && at + 1 == args.size()) {
                        Stats::clearProblems();
                        return Shell::ExitSuccess;
                }
                if (watchS > 0) { // sample while waiting, so rates, rules and problems are current
                        if (Stats::watch().isError()) {
                                ctx.eprintf("stats: the sampler couldn't start\n");
                                return Shell::ExitFailure;
                        }
                        (void)ctx.shell().interrupt().sleep(unsigned(watchS * 1000)); // Ctrl-C wakes it
                }
                int status;
                if (word != nullptr && std::strcmp(word, "problems") == 0 && at + 1 == args.size())
                        status = showProblems(ctx);
                else if (word != nullptr && std::strcmp(word, "rules") == 0 && at + 2 >= args.size())
                        status = showRules(ctx, at + 1 < args.size() ? args[at + 1] : nullptr);
                else if (at + 1 >= args.size())
                        status = showCounters(ctx, word);
                else
                        status = ctx.usage();
                if (watchS > 0) Stats::unwatch();
                return ctx.isInterrupted() ? Shell::ExitInterrupted : status;
        }

} // namespace

BMDS_SHELL_COMMAND(stats, runStats, "[-w SECONDS] [PATTERN | rules [PATTERN] | problems | clear] | kept [clear]",
                   "Show the system's counters, their rules, or the problems seen; -w watches first. 'kept': the "
                   "lows, highs and totals kept across resets ('kept clear' starts them again)")

BMDS_NAMESPACE_END
