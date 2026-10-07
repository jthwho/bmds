/**
 * @file      time.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <bmds/clock.h>
#include <bmds/clockmodel.h>
#include <bmds/datetime.h>
#include <bmds/seqlocked.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/timecommands.h>
#include <bmds/timezone.h>
#include "fakeconsole.h"

using bmds::ClockModel;
using bmds::ClockSource;
using bmds::DateTime;
using bmds::LocalTime;
using bmds::Timestamp;
using bmds::TimeZone;
using bmds::Calendar::daysFromCivil;

namespace {

        constexpr int64_t Second = Timestamp::NsPerSecond;

        int64_t utc(int year, unsigned month, unsigned day, int hour = 0, int minute = 0, int second = 0) {
                return daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
        }

        std::string format(const char *fmt, const LocalTime &t) {
                bmds::String out;
                REQUIRE(bmds::formatTime(out, fmt, t).isOk());
                return std::string(out.data(), out.size());
        }

        TimeZone zone(const char *spec) {
                const bmds::Result<TimeZone> z = TimeZone::parse(spec);
                REQUIRE(z.error().isOk());
                return z.value();
        }

        std::string local(const TimeZone &z, int64_t utcSeconds) {
                return format("%F %T %Z %z", z.localTime(Timestamp::fromSeconds(utcSeconds)));
        }

} // namespace

TEST_CASE("Calendar and DateTime") {
        CHECK(daysFromCivil(1970, 1, 1) == 0);
        CHECK(daysFromCivil(2000, 3, 1) == 11017);
        CHECK(daysFromCivil(1969, 12, 31) == -1);
        CHECK(ClockModel::UnsetEpoch.ns == 1767225600 * Second);

        SUBCASE("round trips over centuries") {
                for (int64_t days = daysFromCivil(1600, 1, 1); days < daysFromCivil(2400, 1, 1); days += 13) {
                        const DateTime dt = DateTime::fromSeconds(days * 86400 + 45296);
                        CHECK(dt.isValid());
                        CHECK(daysFromCivil(dt.year, dt.month, dt.day) == days);
                        CHECK(dt.toSeconds() == days * 86400 + 45296);
                        REQUIRE(dt.hour == 12);
                }
        }

        SUBCASE("fields") {
                const DateTime dt = DateTime::fromSeconds(utc(2026, 10, 3, 14, 2, 11), 5);
                CHECK(dt.year == 2026);
                CHECK(dt.month == 10);
                CHECK(dt.day == 3);
                CHECK(dt.hour == 14);
                CHECK(dt.minute == 2);
                CHECK(dt.second == 11);
                CHECK(dt.weekday == 6); // Saturday
                CHECK(dt.yearDay == 275);
                CHECK(dt.nanosecond == 5);
                const DateTime before = DateTime::fromSeconds(-1);
                CHECK(before.year == 1969);
                CHECK(before.month == 12);
                CHECK(before.day == 31);
                CHECK(before.second == 59);
                CHECK(before.weekday == 3); // Wednesday
        }

        SUBCASE("leap years and validity") {
                CHECK(bmds::Calendar::isLeapYear(2000));
                CHECK_FALSE(bmds::Calendar::isLeapYear(1900));
                CHECK(bmds::Calendar::isLeapYear(2024));
                DateTime dt;
                dt.year = 2000;
                dt.month = 2;
                dt.day = 29;
                CHECK(dt.isValid());
                dt.year = 1900;
                CHECK_FALSE(dt.isValid());
                dt.month = 13;
                CHECK_FALSE(dt.isValid());
        }

        SUBCASE("timestamps round down") {
                CHECK(Timestamp{-1}.seconds() == -1);
                CHECK(Timestamp{-1}.nanoseconds() == 999999999);
                CHECK(Timestamp{Second}.seconds() == 1);
                CHECK(Timestamp::fromSeconds(-2, 5).seconds() == -2);
        }
}

TEST_CASE("formatTime") {
        LocalTime t = TimeZone().localTime(Timestamp::fromSeconds(utc(2026, 3, 4, 15, 6, 7), 123456789));
        CHECK(format("%a %b %e %H:%M:%S %Z %Y", t) == "Wed Mar  4 15:06:07 UTC 2026");
        CHECK(format("%A %B %d %j %y %C", t) == "Wednesday March 04 063 26 20");
        CHECK(format("%F %T.%N", t) == "2026-03-04 15:06:07.123456789");
        CHECK(format("%D %R %r %I %l %k %p", t) == "03/04/26 15:06 03:06:07 PM 03  3 15 PM");
        CHECK(format("%s %u %w %z %%", t) == "1772636767 3 3 +0000 %");
        CHECK(format("%c", t) == "Wed Mar  4 15:06:07 2026");
        CHECK(format("a%nb%tc %q trailing %", t) == "a\nb\tc %q trailing %");
        CHECK(format("", t) == "");
}

TEST_CASE("TimeZone") {
        SUBCASE("UTC forms") {
                CHECK(local(TimeZone(), 0) == "1970-01-01 00:00:00 UTC +0000");
                CHECK(local(zone(""), 0) == "1970-01-01 00:00:00 UTC +0000");
                CHECK(local(zone("UTC"), 0) == "1970-01-01 00:00:00 UTC +0000");
                CHECK(local(zone("UTC0"), 0) == "1970-01-01 00:00:00 UTC +0000");
                CHECK(local(zone("GMT"), 0) == "1970-01-01 00:00:00 GMT +0000");
                CHECK(zone("UTC0").spec() == "UTC0");
        }

        SUBCASE("fixed offsets") {
                CHECK(local(zone("EST5"), 0) == "1969-12-31 19:00:00 EST -0500");
                CHECK(local(zone("<+0530>-5:30"), 0) == "1970-01-01 05:30:00 +0530 +0530");
                CHECK(local(zone("NPT-5:45"), 0) == "1970-01-01 05:45:00 NPT +0545");
        }

        SUBCASE("central Europe") {
                const TimeZone cet = zone("CET-1CEST,M3.5.0,M10.5.0/3");
                // DST from 2026-03-29 01:00 UTC to 2026-10-25 01:00 UTC.
                CHECK(local(cet, utc(2026, 3, 29, 0, 59, 59)) == "2026-03-29 01:59:59 CET +0100");
                CHECK(local(cet, utc(2026, 3, 29, 1)) == "2026-03-29 03:00:00 CEST +0200");
                CHECK(local(cet, utc(2026, 10, 25, 0, 59, 59)) == "2026-10-25 02:59:59 CEST +0200");
                CHECK(local(cet, utc(2026, 10, 25, 1)) == "2026-10-25 02:00:00 CET +0100");
                CHECK(local(cet, utc(2026, 7, 1)) == "2026-07-01 02:00:00 CEST +0200");
                CHECK(local(cet, utc(2026, 12, 31, 23)) == "2027-01-01 00:00:00 CET +0100");
                // The ambiguous hour is the earlier instant; the missing hour lands after the gap.
                CHECK(cet.toUtc(utc(2026, 10, 25, 2, 30)) == utc(2026, 10, 25, 0, 30));
                CHECK(cet.toUtc(utc(2026, 3, 29, 2, 30)) == utc(2026, 3, 29, 1, 30));
                CHECK(cet.toUtc(utc(2026, 7, 1, 12)) == utc(2026, 7, 1, 10));
                CHECK(cet.toUtc(utc(2026, 1, 1, 12)) == utc(2026, 1, 1, 11));
        }

        SUBCASE("United States, and the default rules") {
                for (const char *spec : {"EST5EDT,M3.2.0,M11.1.0", "EST5EDT"}) {
                        const TimeZone us = zone(spec);
                        // DST from 2026-03-08 07:00 UTC to 2026-11-01 06:00 UTC.
                        CHECK(local(us, utc(2026, 3, 8, 6, 59, 59)) == "2026-03-08 01:59:59 EST -0500");
                        CHECK(local(us, utc(2026, 3, 8, 7)) == "2026-03-08 03:00:00 EDT -0400");
                        CHECK(local(us, utc(2026, 11, 1, 5, 59, 59)) == "2026-11-01 01:59:59 EDT -0400");
                        CHECK(local(us, utc(2026, 11, 1, 6)) == "2026-11-01 01:00:00 EST -0500");
                }
        }

        SUBCASE("southern hemisphere") {
                const TimeZone au = zone("AEST-10AEDT,M10.1.0,M4.1.0/3");
                // DST ends 2026-04-05 03:00 AEDT (04-04 16:00 UTC), starts 2026-10-04 02:00 AEST (10-03 16:00 UTC).
                CHECK(local(au, utc(2026, 1, 15)) == "2026-01-15 11:00:00 AEDT +1100");
                CHECK(local(au, utc(2026, 4, 4, 15, 59, 59)) == "2026-04-05 02:59:59 AEDT +1100");
                CHECK(local(au, utc(2026, 4, 4, 16)) == "2026-04-05 02:00:00 AEST +1000");
                CHECK(local(au, utc(2026, 10, 3, 15, 59, 59)) == "2026-10-04 01:59:59 AEST +1000");
                CHECK(local(au, utc(2026, 10, 3, 16)) == "2026-10-04 03:00:00 AEDT +1100");
        }

        SUBCASE("Julian rules, explicit DST offset and times") {
                // J60 is March 1 in every year; day 59 counts from 0 (March 1 in leap years).
                const TimeZone j1 = zone("AAA3BBB2,J60/0,J300/0");
                CHECK(local(j1, utc(2024, 3, 1, 2, 59, 59)) == "2024-02-29 23:59:59 AAA -0300");
                CHECK(local(j1, utc(2024, 3, 1, 3)) == "2024-03-01 01:00:00 BBB -0200");
                const TimeZone j0 = zone("AAA3BBB,59/-1,300/26");
                CHECK(local(j0, utc(2024, 2, 29, 1, 59, 59)) == "2024-02-28 22:59:59 AAA -0300");
                CHECK(local(j0, utc(2024, 2, 29, 2)) == "2024-02-29 00:00:00 BBB -0200");
        }

        SUBCASE("malformed") {
                for (const char *bad :
                     {"X1", "EST", "EST5EDT,M3.2.0", "EST5EDT,M13.2.0,M11.1.0", "EST5EDT,M3.6.0,M11.1.0",
                      "EST5EDT,M3.2.7,M11.1.0", "EST25", "<+05", "EST5EDT,J0,J1", "EST5EDT,M3.2.0,M11.1.0x",
                      "CET-1CEST,M3.5.0,M10.5.0/3,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}) {
                        CAPTURE(bad);
                        CHECK(TimeZone::parse(bad).error() == bmds::Error::Invalid);
                }
        }
}

TEST_CASE("ClockModel") {
        ClockModel c;

        SUBCASE("an unset clock counts from 2026") {
                CHECK(c.wallAt(0) == ClockModel::UnsetEpoch);
                CHECK(c.wallAt(5 * Second).ns == ClockModel::UnsetEpoch.ns + 5 * Second);
                CHECK_FALSE(c.status(0).isSet());
        }

        SUBCASE("set steps") {
                c.set(10 * Second, Timestamp::fromSeconds(1000), ClockSource::Manual);
                CHECK(c.wallAt(10 * Second).ns == 1000 * Second);
                CHECK(c.wallAt(12 * Second).ns == 1002 * Second);
                CHECK(c.status(12 * Second).source == ClockSource::Manual);
                CHECK(c.status(12 * Second).lastSet.ns == 1000 * Second);
        }

        SUBCASE("adjust slews at 500 ppm and never goes backwards") {
                c.set(0, Timestamp::fromSeconds(1000), ClockSource::Manual);
                CHECK(c.adjust(0, -50'000'000) == 0); // -50 ms takes 100 s
                CHECK(c.wallAt(50 * Second).ns == 1050 * Second - 25'000'000);
                CHECK(c.status(50 * Second).slewRemainingNs == -25'000'000);
                CHECK(c.wallAt(100 * Second).ns == 1100 * Second - 50'000'000);
                CHECK(c.wallAt(200 * Second).ns == 1200 * Second - 50'000'000);
                CHECK(c.status(200 * Second).slewRemainingNs == 0);
                int64_t last = c.wallAt(0).ns;
                for (int64_t m = 0; m < 120 * Second; m += 977'777) {
                        const int64_t w = c.wallAt(m).ns;
                        REQUIRE(w >= last);
                        last = w;
                }
        }

        SUBCASE("a new adjustment replaces what was left") {
                c.set(0, Timestamp::fromSeconds(1000), ClockSource::Manual);
                (void)c.adjust(0, 10'000'000);
                CHECK(c.adjust(10 * Second, 2'000'000) == 5'000'000);
                CHECK(c.wallAt(10 * Second).ns == 1010 * Second + 5'000'000);
                CHECK(c.wallAt(20 * Second).ns == 1020 * Second + 7'000'000);
        }

        SUBCASE("frequency correction") {
                c.set(0, Timestamp::fromSeconds(0), ClockSource::Ntp);
                CHECK(c.setFrequency(0, 600'000) == bmds::Error::OutOfRange);
                REQUIRE(c.setFrequency(0, 100'000).isOk()); // +100 ppm
                CHECK(c.wallAt(1000 * Second).ns == 1000 * Second + 100'000'000);
                REQUIRE(c.setFrequency(1000 * Second, -100'000).isOk());
                CHECK(c.wallAt(2000 * Second).ns == 2000 * Second);
                // Exact over long uptimes (no overflow): 100 years at -100 ppm.
                const int64_t century = 100ll * 365 * 86400 * Second;
                CHECK(c.wallAt(1000 * Second + century).ns == 1000 * Second + 100'000'000 + century - century / 10000);
        }

        SUBCASE("step cancels the slew") {
                c.set(0, Timestamp::fromSeconds(0), ClockSource::Manual);
                (void)c.adjust(0, Second);
                c.set(10 * Second, Timestamp::fromSeconds(500), ClockSource::Manual);
                CHECK(c.status(20 * Second).slewRemainingNs == 0);
                CHECK(c.wallAt(20 * Second).ns == 510 * Second);
        }
}

TEST_CASE("SeqLocked: readers always see a consistent value") {
        struct Pair {
                        uint64_t a;
                        uint64_t b;
        };
        static constinit bmds::SeqLocked<Pair> value(Pair{1, 1});
        std::atomic<bool>                      stop = false;
        std::atomic<int>                       torn = 0;
        std::thread                            reader([&] {
                while (!stop.load()) {
                        const Pair p = value.load();
                        if (p.a != p.b) torn.fetch_add(1);
                }
        });
        for (uint64_t i = 2; i < 20000; ++i) value.store(Pair{i, i});
        const int result = value.update([](Pair &p) {
                p.a += 1;
                p.b += 1;
                return 7;
        });
        stop.store(true);
        reader.join();
        CHECK(result == 7);
        CHECK(torn.load() == 0);
        CHECK(value.load().a == 20000);
}

namespace {

        constexpr bmds::Command TimeCommandTable[] = {
                {"clock", "[zone TZ | tzdata [PATH] | adjust [+|-]SECONDS]", "clock", bmds::TimeCommands::clock},
                {"date", "[-u] [+FORMAT] | [-u] -s DATE", "date", bmds::TimeCommands::date},
        };

        struct TimeFixture {
                        TimeFixture()
                            : shell(std::make_unique<bmds::Shell>(
                                      con, bmds::Span<const bmds::Command>(TimeCommandTable), 1 << 20)) {
                                REQUIRE(shell->init().isOk());
                                REQUIRE(bmds::Clock::setTimeZone("UTC0").isOk());
                        }

                        ~TimeFixture() { (void)bmds::Clock::setTimeZone("UTC0"); }

                        std::string out(const char *line, int expectStatus = bmds::Shell::ExitSuccess) {
                                CHECK(shell->execute(line) == expectStatus);
                                return con.take();
                        }

                        FakeConsole                  con;
                        std::unique_ptr<bmds::Shell> shell;
        };

} // namespace

TEST_CASE("date and clock") {
        TimeFixture f;

        SUBCASE("set and show") {
                CHECK(f.out("date -s '2026-03-04 12:00:00'") == "Wed Mar  4 12:00:00 UTC 2026\n");
                CHECK(bmds::Clock::isSet());
                CHECK(bmds::Clock::status().source == ClockSource::Manual);
                CHECK(f.out("date -u +%F") == "2026-03-04\n");
                CHECK(f.out("date '+%Y-%m-%d %H:%M'") == "2026-03-04 12:00\n");
                CHECK(f.out("date -s @0 +%s") == "0\n");
                CHECK(f.out("date -s '1999-12-31T23:59' +%T") == "23:59:00\n");
                CHECK(f.out("date -s 2030-01-02 +%F_%T") == "2030-01-02_00:00:00\n");
                CHECK(f.out("date -s 08:30 +%T") == "08:30:00\n");
                CHECK(f.out("date +%F") == "2030-01-02\n");
        }

        SUBCASE("time zones: TZ, the kernel zone, -u") {
                CHECK(f.out("date -u -s '2026-07-01 10:00'") == "Wed Jul  1 10:00:00 UTC 2026\n");
                CHECK(f.out("clock zone CET-1CEST,M3.5.0,M10.5.0/3") == "");
                CHECK(f.out("date '+%H:%M %Z'") == "12:00 CEST\n");
                CHECK(f.out("date -s '2026-07-01 18:30' '+%T %Z'") == "18:30:00 CEST\n");
                CHECK(f.out("date -u +%T") == "16:30:00\n");
                CHECK(f.out("date -s '2026-07-01 12:00'") == "Wed Jul  1 12:00:00 CEST 2026\n");
                CHECK(f.out("TZ=EST5EDT; date '+%H:%M %Z %z'") == "06:00 EDT -0400\n");
                CHECK(f.out("date -u '+%H:%M %Z'") == "10:00 UTC\n");
                CHECK(f.out("TZ=bogus; date '+%H:%M %Z'") == "10:00 UTC\n");
        }

        SUBCASE("errors") {
                CHECK(f.out("date -s nonsense", bmds::Shell::ExitFailure) == "date: invalid date 'nonsense'\n");
                CHECK(f.out("date -s '2026-02-30 10:00'", bmds::Shell::ExitFailure) ==
                      "date: invalid date '2026-02-30 10:00'\n");
                CHECK(f.out("date -s '25:00'", bmds::Shell::ExitFailure) == "date: invalid date '25:00'\n");
                CHECK(f.out("date -x", bmds::Shell::ExitUsage) == "usage: date [-u] [+FORMAT] | [-u] -s DATE\n");
                CHECK(f.out("clock zone 'CET-1CEST,M13'", bmds::Shell::ExitFailure) ==
                      "clock: invalid time zone 'CET-1CEST,M13'\n");
                CHECK(f.out("clock adjust x", bmds::Shell::ExitUsage) ==
                      "usage: clock [zone TZ | tzdata [PATH] | adjust [+|-]SECONDS]\n");
        }

        SUBCASE("clock status") {
                CHECK(f.out("date -s '2026-03-04 12:00:00' +") == "\n");
                CHECK(f.out("clock adjust -0.25") == "");
                const std::string status = f.out("clock");
                CAPTURE(status);
                CHECK(status.find("Time:      2026-03-04 12:00:0") == 0);
                CHECK(status.find("\nSource:    manual, set 2026-03-04 12:00:00 UTC\n") != std::string::npos);
                CHECK(status.find("\nSlew:      -0.2") != std::string::npos);
                CHECK(status.find("\nFrequency: +0 ppb\nZone:      UTC0 (UTC, UTC+0000)\n") != std::string::npos);
                CHECK(bmds::Clock::adjust(0) < 0);
        }
}
