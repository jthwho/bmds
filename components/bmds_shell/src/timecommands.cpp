/**
 * @file      timecommands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `date` and `clock`. Their arguments and output are a userspace interface.
 */

#include <bmds/clock.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/timecommands.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr const char *DefaultFormat = "%a %b %e %H:%M:%S %Z %Y";

        bool isDigit(char c) {
                return c >= '0' && c <= '9';
        }

        // Reads exactly @p digits digits at @p at.
        bool fixedNumber(StringView text, size_t at, size_t digits, int &value) {
                if (at + digits > text.size()) return false;
                value = 0;
                for (size_t i = at; i < at + digits; ++i) {
                        if (!isDigit(text[i])) return false;
                        value = value * 10 + (text[i] - '0');
                }
                return true;
        }

        // "[+|-]N[.FFFFFFFFF]" seconds into nanoseconds.
        bool parseSecondsNs(StringView text, int64_t &ns) {
                bool negative = false;
                if (!text.isEmpty() && (text[0] == '-' || text[0] == '+')) {
                        negative = text[0] == '-';
                        text = text.mid(1);
                }
                int64_t whole = 0;
                size_t  i = 0;
                for (; i < text.size() && isDigit(text[i]); ++i) {
                        if (whole > INT64_MAX / Timestamp::NsPerSecond / 10) return false;
                        whole = whole * 10 + (text[i] - '0');
                }
                const bool hasWhole = i > 0;
                int64_t    frac = 0;
                int64_t    scale = Timestamp::NsPerSecond / 10;
                bool       hasFrac = false;
                if (i < text.size() && text[i] == '.') {
                        for (++i; i < text.size() && isDigit(text[i]); ++i) {
                                frac += (text[i] - '0') * scale;
                                scale /= 10;
                                hasFrac = true;
                        }
                }
                if (i != text.size() || (!hasWhole && !hasFrac)) return false;
                ns = whole * Timestamp::NsPerSecond + frac;
                if (negative) ns = -ns;
                return true;
        }

        // The zone `date` shows: UTC with -u, else as CommandContext::displayZone().
        TimeZone displayZone(CommandContext &ctx, bool utc) {
                return utc ? TimeZone() : ctx.displayZone();
        }

        // "@SECONDS[.frac]", "YYYY-MM-DD[ HH:MM[:SS]]" or "HH:MM[:SS]" (today),
        // the civil forms in @p zone.
        bool parseDate(StringView text, const TimeZone &zone, Timestamp &out) {
                if (!text.isEmpty() && text[0] == '@') {
                        int64_t ns = 0;
                        if (!parseSecondsNs(text.mid(1), ns)) return false;
                        out = Timestamp{ns};
                        return true;
                }
                DateTime dt = zone.localTime(Clock::now()).local;
                dt.hour = dt.minute = dt.second = 0;
                size_t at = 0;
                int    year = 0;
                int    month = 0;
                int    day = 0;
                if (fixedNumber(text, 0, 4, year) && text.size() >= 10 && text[4] == '-' && text[7] == '-') {
                        if (!fixedNumber(text, 5, 2, month) || !fixedNumber(text, 8, 2, day)) return false;
                        dt.year = year;
                        dt.month = static_cast<uint8_t>(month);
                        dt.day = static_cast<uint8_t>(day);
                        at = 10;
                        if (at < text.size()) {
                                if (text[at] != ' ' && text[at] != 'T') return false;
                                ++at;
                        }
                }
                if (at < text.size()) {
                        int hour = 0;
                        int minute = 0;
                        int second = 0;
                        if (!fixedNumber(text, at, 2, hour) || at + 2 >= text.size() || text[at + 2] != ':' ||
                            !fixedNumber(text, at + 3, 2, minute))
                                return false;
                        at += 5;
                        if (at < text.size()) {
                                if (text[at] != ':' || !fixedNumber(text, at + 1, 2, second)) return false;
                                at += 3;
                        }
                        if (at != text.size()) return false;
                        dt.hour = static_cast<uint8_t>(hour);
                        dt.minute = static_cast<uint8_t>(minute);
                        dt.second = static_cast<uint8_t>(second);
                } else if (at == 0) {
                        return false;
                }
                dt.nanosecond = 0;
                if (!dt.isValid()) return false;
                out = Timestamp::fromSeconds(zone.toUtc(dt.toSeconds()));
                return true;
        }

        int printTime(CommandContext &ctx, StringView format, const LocalTime &time) {
                String text;
                Error  err = formatTime(text, format, time);
                if (err.isOk()) err = text.append('\n');
                if (err.isError()) {
                        ctx.eprintf("date: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                ctx.print(text.view());
                return Shell::ExitSuccess;
        }

        String formatted(StringView format, const LocalTime &time) {
                String text;
                (void)formatTime(text, format, time);
                return text;
        }

} // namespace

int TimeCommands::date(CommandContext &ctx) {
        const Args  args = ctx.args();
        bool        utc = false;
        const char *setTo = nullptr;
        const char *format = nullptr;
        for (size_t i = 1; i < args.size(); ++i) {
                const StringView arg(args[i]);
                if (arg == "-u") {
                        utc = true;
                } else if (arg == "-s" && i + 1 < args.size() && setTo == nullptr) {
                        setTo = args[++i];
                } else if (arg.startsWith("+") && format == nullptr) {
                        format = args[i] + 1;
                } else {
                        return ctx.usage();
                }
        }
        const TimeZone zone = displayZone(ctx, utc);
        if (setTo != nullptr) {
                Timestamp when;
                if (!parseDate(setTo, zone, when)) {
                        ctx.eprintf("date: invalid date '%s'\n", setTo);
                        return Shell::ExitFailure;
                }
                const Error err = Clock::set(when, ClockSource::Manual);
                if (err.isError()) {
                        ctx.eprintf("date: cannot set the clock: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                return printTime(ctx, format != nullptr ? format : DefaultFormat, zone.localTime(when));
        }
        if (format == nullptr && !Clock::isSet()) ctx.eprint("date: the clock is not set\n");
        return printTime(ctx, format != nullptr ? format : DefaultFormat, zone.localTime(Clock::now()));
}

namespace {

        int tzdata(CommandContext &ctx);

} // namespace

int TimeCommands::clock(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() == 2 && StringView(args[1]) == "tzdata") return tzdata(ctx);
        if (args.size() == 3 && StringView(args[1]) == "adjust") {
                int64_t ns = 0;
                if (!parseSecondsNs(args[2], ns)) return ctx.usage();
                (void)Clock::adjust(ns);
                return Shell::ExitSuccess;
        }
        if (args.size() != 1) return ctx.usage();

        const ClockStatus status = Clock::status();
        const TimeZone    zone = Clock::timeZone();
        const LocalTime   now = zone.localTime(Clock::now());
        ctx.printf("Time:      %s\n", formatted("%Y-%m-%d %H:%M:%S.%N %Z", now).cStr());
        if (status.isSet()) {
                ctx.printf("Source:    %s, set %s\n", clockSourceName(status.source),
                           formatted("%Y-%m-%d %H:%M:%S %Z", zone.localTime(status.lastSet)).cStr());
        } else {
                ctx.print("Source:    none (not set; counting from 1955-11-05 06:15 local)\n");
        }
        const int64_t  slew = status.slewRemainingNs;
        const uint64_t magnitude = slew < 0 ? 0 - static_cast<uint64_t>(slew) : static_cast<uint64_t>(slew);
        ctx.printf("Slew:      %c%llu.%09llu s remaining\n", slew < 0 ? '-' : '+',
                   static_cast<unsigned long long>(magnitude / Timestamp::NsPerSecond),
                   static_cast<unsigned long long>(magnitude % Timestamp::NsPerSecond));
        ctx.printf("Frequency: %+ld ppb\n", static_cast<long>(status.frequencyPpb));
        const StringView      spec = zone.spec().isEmpty() ? StringView("UTC0") : zone.spec();
        const Clock::ZoneName name = Clock::zoneName();
        if (name.pending == Clock::ZonePending::NoDatabase) {
                ctx.printf("Zone:      %s (pending: no zone database; UTC until it is found)\n", name.name);
        } else if (name.pending == Clock::ZonePending::Unknown) {
                ctx.printf("Zone:      %s (pending: not in the zone database; UTC meanwhile)\n", name.name);
        } else if (!name.view().isEmpty()) {
                ctx.printf("Zone:      %s (%.*s; %s)\n", name.name, static_cast<int>(spec.size()), spec.data(),
                           formatted("%Z, UTC%z", now).cStr());
        } else {
                ctx.printf("Zone:      %.*s (%s)\n", static_cast<int>(spec.size()), spec.data(),
                           formatted("%Z, UTC%z", now).cStr());
        }
        return Shell::ExitSuccess;
}

namespace {

        // `clock tzdata`: the time zone database (sysconf's clock/tzdata), its path and what it holds.
        int tzdata(CommandContext &ctx) {
                const Result<String> path = Vfs::zoneDatabase();
                if (path.error().isError() || path.value().isEmpty()) {
                        ctx.print("clock: no time zone database is set\n");
                        return Shell::ExitSuccess;
                }
                const Result<Vfs::ZoneDatabaseInfo> info = Vfs::zoneDatabaseInfo();
                if (info.error().isError()) {
                        const Error err = info.error();
                        ctx.printf("%s: %s\n", path.value().cStr(),
                                   err == Error::CorruptData    ? "not a time zone database, or damaged"
                                   : err == Error::NotSupported ? "a later database format"
                                                                : CommandContext::errorText(err));
                        return Shell::ExitSuccess;
                }
                if (info.value().fromRom) {
                        ctx.printf("%s: not there: the ROM's, %.*s: tzdata %s, %lu zones\n", path.value().cStr(),
                                   int(Vfs::RomZoneDatabase.size()), Vfs::RomZoneDatabase.data(), info.value().version,
                                   static_cast<unsigned long>(info.value().count));
                } else {
                        ctx.printf("%s: tzdata %s, %lu zones\n", path.value().cStr(), info.value().version,
                                   static_cast<unsigned long>(info.value().count));
                }
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(clock, TimeCommands::clock, "[tzdata | adjust [+|-]SECONDS]",
                   "Show the system clock (source, slew, frequency, zone: sysconf's clock/zone); show the zone "
                   "database (clock/tzdata); slew the time")
BMDS_SHELL_COMMAND(date, TimeCommands::date, "[-u] [+FORMAT] | [-u] -s DATE",
                   "Show the time (in $TZ, or the kernel zone); set it: -s 'YYYY-MM-DD HH:MM[:SS]', -s @SECONDS")

BMDS_NAMESPACE_END
