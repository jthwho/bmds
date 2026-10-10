/**
 * @file      stats.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <string>
#include <bmds/stat.h>

using namespace bmds;
using Stat::Health;
using Stat::Kind;
using Stat::Limits;
using Stat::Rule;

namespace {

        Limits zero(Health h = Health::Warning, uint16_t hold = 0) {
                Limits l;
                l.rule = Rule::Zero;
                l.health = h;
                l.hold = hold;
                return l;
        }

        Limits levels(Rule rule, int64_t warn, int64_t crit, uint16_t hold = 0) {
                Limits l;
                l.rule = rule;
                l.warn = warn;
                l.crit = crit;
                l.hold = hold;
                return l;
        }

} // namespace

TEST_CASE("Stat names and labels") {
        CHECK(std::string(Stat::unitName(Stat::Unit::Micros)) == "us");
        CHECK(std::string(Stat::ruleName(Rule::Drift)) == "drift");
        CHECK(std::string(Stat::healthName(Health::Critical)) == "critical");
        CHECK(Stat::label("stopped|running|faulted", 0) == StringView("stopped"));
        CHECK(Stat::label("stopped|running|faulted", 2) == StringView("faulted"));
        CHECK(Stat::label("stopped|running|faulted", 3).isEmpty());
        CHECK(Stat::label("stopped|running|faulted", -1).isEmpty());
        CHECK(Stat::label("", 0).isEmpty());
}

TEST_CASE("Stat::Judge: a Zero rule holds after each increase") {
        Stat::Judge  j;
        const Limits l = zero(Health::Critical, 10);
        CHECK(j.isFirst());
        CHECK(j.update(l, Kind::Total, 3, 0) == Health::Normal); // the first look: a baseline
        CHECK(j.delta() == 0);
        CHECK(!j.isFirst());
        CHECK(j.update(l, Kind::Total, 3, 1000) == Health::Normal);
        CHECK(j.update(l, Kind::Total, 5, 2000) == Health::Critical);
        CHECK(j.delta() == 2);
        CHECK(j.update(l, Kind::Total, 5, 11000) == Health::Critical); // 9 s after
        CHECK(j.update(l, Kind::Total, 5, 12000) == Health::Normal);   // 10 s after
        // A reset (its source restarted) counts from zero.
        CHECK(j.update(l, Kind::Total, 1, 13000) == Health::Critical);
        CHECK(j.delta() == 1);
        // The default hold is 60 s.
        Stat::Judge d;
        (void)d.update(zero(), Kind::Total, 0, 0);
        CHECK(d.update(zero(), Kind::Total, 1, 1000) == Health::Warning);
        CHECK(d.update(zero(), Kind::Total, 1, 60000) == Health::Warning);
        CHECK(d.update(zero(), Kind::Total, 1, 61000) == Health::Normal);
}

TEST_CASE("Stat::Judge: a Total's rate against levels, held") {
        Stat::Judge  j;
        const Limits l = levels(Rule::Above, 900000, 980000, 5); // a core's busy µs a second
        CHECK(j.update(l, Kind::Total, 0, 0) == Health::Normal);
        CHECK(j.rate() == Stat::Unavailable);
        CHECK(j.update(l, Kind::Total, 500000, 1000) == Health::Normal);
        CHECK(j.rate() == 500000);
        int64_t t = 1000, v = 500000;
        for (int i = 0; i < 4; ++i) { // over 90 % for 4 s: not yet
                t += 1000;
                v += 950000;
                CHECK(j.update(l, Kind::Total, v, t) == Health::Normal);
        }
        t += 1000;
        v += 950000;
        CHECK(j.update(l, Kind::Total, v, t) == Health::Warning); // 5 s
        t += 1000;
        v += 990000;
        CHECK(j.update(l, Kind::Total, v, t) == Health::Critical); // already out: follows at once
        t += 1000;
        v += 100000;
        CHECK(j.update(l, Kind::Total, v, t) == Health::Normal);
        // A rate over a longer gap.
        t += 2000;
        v += 1000000;
        CHECK(j.update(l, Kind::Total, v, t) == Health::Normal);
        CHECK(j.rate() == 500000);
}

TEST_CASE("Stat::Judge: Below, Drift and State") {
        Stat::Judge  below;
        const Limits b = levels(Rule::Below, 1000, 100);
        CHECK(below.update(b, Kind::Level, 5000, 0) == Health::Normal);
        CHECK(below.update(b, Kind::Level, 500, 1000) == Health::Warning);
        CHECK(below.update(b, Kind::Level, 50, 2000) == Health::Critical);

        Stat::Judge  drift;
        const Limits d = levels(Rule::Drift, 100, 500);
        CHECK(drift.update(d, Kind::Level, -50, 0) == Health::Normal);
        CHECK(drift.update(d, Kind::Level, -150, 1000) == Health::Warning);
        CHECK(drift.update(d, Kind::Level, 600, 2000) == Health::Critical);

        Stat::Judge  state;
        const Limits s = levels(Rule::State, Stat::NoLevel, 1 << 2); // faulted (2) is critical
        CHECK(state.update(s, Kind::State, 1, 0) == Health::Normal);
        CHECK(state.update(s, Kind::State, 2, 1000) == Health::Critical);
        CHECK(state.update(s, Kind::State, 0, 2000) == Health::Normal);
}

TEST_CASE("Stat::Judge: a Peak above a level is an event") {
        Stat::Judge  j;
        const Limits l = levels(Rule::Above, 2500, 4000, 30); // fill µs of a 5 ms period
        CHECK(j.update(l, Kind::Peak, 900, 0) == Health::Normal);
        CHECK(j.update(l, Kind::Peak, 3000, 1000) == Health::Warning);
        CHECK(j.update(l, Kind::Peak, 3000, 30000) == Health::Warning);
        CHECK(j.update(l, Kind::Peak, 3000, 31000) == Health::Normal); // stays high, but long ago
        CHECK(j.update(l, Kind::Peak, 4500, 32000) == Health::Critical);
        CHECK(Stat::eventHealth(l, Kind::Peak, 3000) == Health::Warning);
        CHECK(Stat::eventHealth(zero(), Kind::Total, 2) == Health::Warning);
        CHECK(Stat::eventHealth(zero(), Kind::Total, 0) == Health::Normal);
        CHECK(Stat::eventHealth(levels(Rule::Above, 1, 2), Kind::Level, 5) == Health::Normal);
}

TEST_CASE("Stat::Judge: an unavailable value is never a problem") {
        Stat::Judge  j;
        const Limits l = levels(Rule::Drift, 100, 500);
        CHECK(j.update(l, Kind::Level, 600, 0) == Health::Critical);
        CHECK(j.update(l, Kind::Level, Stat::Unavailable, 1000) == Health::Normal);
        CHECK(j.isFirst());
}

TEST_CASE("Stat::Judge: when a problem began, and pausing") {
        Stat::Judge  level;
        const Limits l = levels(Rule::Above, 10, 20, 3);
        CHECK(level.since() == -1);
        (void)level.update(l, Kind::Level, 5, 0);
        (void)level.update(l, Kind::Level, 15, 1000); // out since the interval began, at 0
        CHECK(level.since() == -1);                   // not held yet
        (void)level.update(l, Kind::Level, 15, 2000);
        CHECK(level.update(l, Kind::Level, 15, 3000) == Health::Warning);
        CHECK(level.since() == 0);

        Stat::Judge  event;
        const Limits z = zero(Health::Warning, 10);
        (void)event.update(z, Kind::Total, 0, 0);
        (void)event.update(z, Kind::Total, 1, 5000);
        CHECK(event.since() == 0); // it rose some time since the last look
        (void)event.update(z, Kind::Total, 2, 6000);
        CHECK(event.since() == 0); // the same problem

        // Paused: the health and rate go; the value stays, so what came meanwhile counts.
        event.pause();
        CHECK(event.health() == Health::Normal);
        CHECK(!event.isFirst());
        CHECK(event.update(z, Kind::Total, 5, 60000) == Health::Warning);
        CHECK(event.delta() == 3);
        CHECK(event.rate() == Stat::Unavailable); // none over the gap
        Stat::Judge never;
        never.pause();
        CHECK(never.isFirst());
}

TEST_CASE("Stat::ProblemLog: episodes open, widen and close") {
        Stat::ProblemLog log(MemSpace::Psram);
        CHECK(log.lastChange() == 0);
        CHECK(log.unseen() == Health::Normal);
        REQUIRE(log.note(7, "audio.dsp0.late", Health::Normal, Health::Warning, 1, 1, 1000).isOk());
        REQUIRE(log.note(7, "audio.dsp0.late", Health::Warning, Health::Critical, 4, 3, 2000).isOk());
        CHECK(log.size() == 1);
        Stat::ProblemLog::Episode out[4];
        size_t                    n = log.changedAfter(0, Span<Stat::ProblemLog::Episode>(out, 4));
        REQUIRE(n == 1);
        CHECK(out[0].isOpen());
        CHECK(out[0].health == Health::Critical);
        CHECK(out[0].count == 4);
        CHECK(out[0].firstValue == 1);
        CHECK(out[0].lastValue == 4);
        CHECK(std::string(out[0].name) == "audio.dsp0.late");
        CHECK(log.unseen() == Health::Critical);
        const uint64_t seen = log.lastChange();
        log.markSeen(seen);
        CHECK(log.unseen() == Health::Critical); // still open
        REQUIRE(log.note(7, "audio.dsp0.late", Health::Critical, Health::Normal, 4, 0, 3000).isOk());
        n = log.changedAfter(seen, Span<Stat::ProblemLog::Episode>(out, 4));
        REQUIRE(n == 1);
        CHECK(!out[0].isOpen());
        CHECK(out[0].endMs == 3000);
        CHECK(log.unseen() == Health::Critical); // closed, not yet seen
        log.markSeen(log.lastChange());
        CHECK(log.unseen() == Health::Normal);
        // A new problem of the same counter is a new episode.
        REQUIRE(log.note(7, "audio.dsp0.late", Health::Normal, Health::Warning, 5, 1, 4000).isOk());
        CHECK(log.size() == 2);
        log.markSeen(log.lastChange());
        REQUIRE(log.noteEarlier(9, "midi.port0.dropped", Health::Critical, 12, 12, 4000).isOk());
        CHECK(log.unseen() == Health::Warning); // the earlier one isn't counted; the open one is
        n = log.changedAfter(log.lastChange() - 1, Span<Stat::ProblemLog::Episode>(out, 4));
        REQUIRE(n == 1);
        CHECK((out[0].flags & Stat::ProblemLog::Earlier) != 0);
        CHECK(out[0].count == 12);
        log.close(7, 5000);
        log.clearClosed();
        CHECK(log.size() == 0);
        // An episode starts when its problem began; closeAll() closes every open one.
        REQUIRE(log.note(3, "a", Health::Normal, Health::Warning, 1, 1, 9000, 4000).isOk());
        REQUIRE(log.note(4, "b", Health::Normal, Health::Warning, 1, 1, 9000, 99999).isOk()); // nonsense: now
        log.closeAll(10000);
        n = log.changedAfter(0, Span<Stat::ProblemLog::Episode>(out, 4));
        REQUIRE(n == 2);
        CHECK(out[0].startMs == 4000);
        CHECK(out[1].startMs == 9000);
        CHECK(!out[0].isOpen());
        CHECK(!out[1].isOpen());
        CHECK(out[0].endMs == 10000);
}

TEST_CASE("Stat::ProblemLog: keeps the newest, the earliest change first") {
        Stat::ProblemLog log(MemSpace::Psram);
        for (uint32_t i = 0; i < Stat::ProblemLog::Capacity + 10; ++i) {
                REQUIRE(log.note(i, "x", Health::Normal, Health::Warning, 1, 1, i).isOk());
                log.close(i, i);
        }
        CHECK(log.size() == Stat::ProblemLog::Capacity);
        Stat::ProblemLog::Episode out[3];
        REQUIRE(log.changedAfter(0, Span<Stat::ProblemLog::Episode>(out, 3)) == 3);
        CHECK(out[0].id == 10); // 0-9 were reused
        CHECK(out[0].changed < out[1].changed);
        CHECK(out[1].changed < out[2].changed);
}

TEST_CASE("Stat::Overrides") {
        CHECK(Stat::Overrides::matches("audio.dsp0.late", "audio.dsp0.late"));
        CHECK(!Stat::Overrides::matches("audio.dsp0.late", "audio.dsp0.lat"));
        CHECK(Stat::Overrides::matches("audio.*", "audio.dsp0.late"));
        CHECK(Stat::Overrides::matches("*.late", "audio.dsp0.late"));
        CHECK(Stat::Overrides::matches("audio.dsp*.stream*.underruns", "audio.dsp0.stream3.underruns"));
        CHECK(!Stat::Overrides::matches("audio.dsp*.stream*.underruns", "audio.dsp0.stream3.late"));
        CHECK(Stat::Overrides::matches("*", "x"));

        Stat::Overrides o(MemSpace::Psram);
        size_t          bad = 99;
        REQUIRE(o.parse(" cpu.*:warn=800000, crit=none ,hold=3; audio.*.late:off ;bogus; x:what; ;"
                        "audio.dsp0.limited:crit;wm.round_us_max:rule=above,warn=-5",
                        &bad)
                        .isOk());
        CHECK(bad == 2);
        CHECK(o.size() == 4);

        Limits cpu = levels(Rule::Above, 900000, 980000, 5);
        CHECK(o.apply("cpu.core0.busy_us", cpu));
        CHECK(cpu.rule == Rule::Above);
        CHECK(cpu.warn == 800000);
        CHECK(cpu.crit == Stat::NoLevel);
        CHECK(cpu.hold == 3);

        Limits late = zero(Health::Critical);
        CHECK(o.apply("audio.dsp0.late", late));
        CHECK(late.rule == Rule::None);

        Limits limited = zero(Health::Warning);
        CHECK(o.apply("audio.dsp0.limited", limited));
        CHECK(limited.rule == Rule::Zero);
        CHECK(limited.health == Health::Critical);

        Limits wm;
        CHECK(o.apply("wm.round_us_max", wm));
        CHECK(wm.rule == Rule::Above);
        CHECK(wm.warn == -5);

        Limits other = zero();
        CHECK(!o.apply("display.scanout.stalls", other));
        CHECK(other.rule == Rule::Zero);

        // Replaced, not added to; the later entry wins.
        REQUIRE(o.parse("a.*:warn=1;a.b:warn=2", &bad).isOk());
        CHECK(bad == 0);
        Limits ab;
        CHECK(o.apply("a.b", ab));
        CHECK(ab.warn == 2);
        REQUIRE(o.parse("", &bad).isOk());
        CHECK(o.size() == 0);
        // Out of range, and a name too long.
        const std::string tooLong = "a:hold=70000;a:warn=99999999999999999999;" + std::string(60, 'n') + ":off";
        REQUIRE(o.parse(StringView(tooLong.data(), tooLong.size()), &bad).isOk());
        CHECK(bad == 3);
}

TEST_CASE("Stat::History") {
        Stat::History h(MemSpace::Psram);
        CHECK(h.setup(0, 4) == Error::Invalid);
        REQUIRE(h.setup(2, 4).isOk());
        int32_t out[8] = {};
        CHECK(h.copy(0, Span<int32_t>(out, 8)) == 0);
        for (int32_t i = 1; i <= 6; ++i) {
                const int32_t s[2] = {i, -i};
                h.push(Span<const int32_t>(s, 2));
        }
        CHECK(h.count() == 4);
        REQUIRE(h.copy(0, Span<int32_t>(out, 8)) == 4);
        CHECK(out[0] == 3);
        CHECK(out[3] == 6);
        REQUIRE(h.copy(1, Span<int32_t>(out, 2)) == 2); // the newest two
        CHECK(out[0] == -5);
        CHECK(out[1] == -6);
        CHECK(h.copy(2, Span<int32_t>(out, 8)) == 0);
        const int32_t one[1] = {9}; // a short sample: the rest 0
        h.push(Span<const int32_t>(one, 1));
        REQUIRE(h.copy(1, Span<int32_t>(out, 1)) == 1);
        CHECK(out[0] == 0);
        h.release();
        CHECK(h.count() == 0);
}

// ---- The kernel's registry ----------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <thread>
#include <bmds/stats.h>

namespace {

        struct FakeDevice {
                        int64_t late = 0;
                        int64_t busyUs = 0;
                        int64_t state = 1;
                        int64_t fillMax = 0;
                        int64_t periodUs = 5000;
                        bool    measured = true;
        };

        constexpr Stat::Desc FakeDescs[] = {
                {"late", Stat::Unit::Count, Kind::Total, 0, {Rule::Zero, Health::Critical, 10}, "", "Late periods"},
                {"busy_us",
                 Stat::Unit::Micros,
                 Kind::Total,
                 Stat::KeepHistory,
                 {Rule::Above, Health::Warning, 0, 500000, 800000},
                 "",
                 "Busy"},
                {"state",
                 Stat::Unit::Count,
                 Kind::State,
                 0,
                 {Rule::State, Health::Warning, 0, Stat::NoLevel, 1 << 2},
                 "stopped|running|faulted",
                 "State"},
                {"fill_us_max",
                 Stat::Unit::Micros,
                 Kind::Peak,
                 Stat::Adjusted | Stat::KeepHistory,
                 {Rule::Above, Health::Warning, 30},
                 "",
                 "Largest fill"},
        };

        void readFake(void *arg, Span<int64_t> values) {
                const FakeDevice *d = static_cast<FakeDevice *>(arg);
                values[0] = d->late;
                values[1] = d->measured ? d->busyUs : Stat::Unavailable;
                values[2] = d->state;
                values[3] = d->fillMax;
                return;
        }

        void adjustFake(void *arg, size_t index, Stat::Limits &limits) {
                const FakeDevice *d = static_cast<FakeDevice *>(arg);
                if (index == 3) {
                        limits.warn = d->periodUs / 2;
                        limits.crit = d->periodUs * 4 / 5;
                }
                return;
        }

        constexpr size_t FakeCount = sizeof(FakeDescs) / sizeof(FakeDescs[0]);

        // The episodes changed after @p after, as names and healths.
        std::string problemsAfter(uint64_t after) {
                Stat::ProblemLog::Episode out[16];
                const size_t              n = Stats::problems(after, Span<Stat::ProblemLog::Episode>(out, 16));
                std::string               s;
                for (size_t i = 0; i < n; ++i) {
                        s += out[i].name;
                        s += ':';
                        s += Stat::healthName(out[i].health);
                        s += out[i].isOpen() ? " open" : "";
                        s += (out[i].flags & Stat::ProblemLog::Earlier) != 0 ? " earlier" : "";
                        s += ';';
                }
                return s;
        }

} // namespace

TEST_CASE("Stats: groups, ids, listing and reading") {
        REQUIRE(Stats::init().isOk());
        const size_t before = Stats::count();
        FakeDevice   dev;
        Stats::Group g(FakeDescs, FakeCount, readFake, &dev, adjustFake);
        CHECK(Stats::add(g) == Error::Invalid); // no name
        g.setName("test.dsp", 0);
        CHECK(g.name() == StringView("test.dsp0"));
        const uint32_t gen = Stats::generation();
        REQUIRE(Stats::add(g).isOk());
        CHECK(Stats::add(g) == Error::Exists);
        CHECK(Stats::generation() != gen);
        CHECK(Stats::count() == before + FakeCount);
        CHECK(g.isAdded());

        const uint32_t lateId = Stats::find("test.dsp0.late");
        CHECK(lateId == g.idOf(0));
        CHECK(Stats::find("test.dsp0.nothing") == Stats::NoId);
        CHECK(Stats::find("test.dsp") == Stats::NoId);
        CHECK(Stats::find("test.dsp0late") == Stats::NoId);

        dev.late = 4;
        dev.state = 2;
        const uint32_t ids[] = {g.idOf(2), g.idOf(0), 12345678};
        int64_t        values[3] = {};
        CHECK(Stats::read(Span<const uint32_t>(ids, 3), Span<int64_t>(values, 3)) == 3);
        CHECK(values[0] == 2);
        CHECK(values[1] == 4);
        CHECK(values[2] == Stat::Unavailable);

        Stats::Info info;
        bool        found = false;
        for (size_t i = 0; Stats::info(i, info).isOk(); ++i) {
                if (info.id != g.idOf(3)) continue;
                found = true;
                CHECK(std::string(info.name) == "test.dsp0.fill_us_max");
                CHECK(info.limits.warn == 2500); // adjusted from the period
                CHECK(info.limits.crit == 4000);
                CHECK(std::string(info.desc.help) == "Largest fill");
                CHECK(info.value == 0);
        }
        CHECK(found);
        CHECK(Stats::info(Stats::count(), info) == Error::NotFound);
        // Described without a read; health not judged while nobody watches.
        found = false;
        for (size_t i = 0; Stats::describe(i, info).isOk(); ++i) {
                if (info.id != g.idOf(0)) continue;
                found = true;
                CHECK(std::string(info.name) == "test.dsp0.late");
                CHECK(info.value == Stat::Unavailable);
                CHECK(!info.judged);
        }
        CHECK(found);
        CHECK(Stats::describe(Stats::count(), info) == Error::NotFound);
        uint8_t health[3] = {};
        CHECK(Stats::read(Span<const uint32_t>(ids, 3), Span<int64_t>(values, 3), Span<uint8_t>(health, 2)) == 3);
        CHECK(values[1] == 4);
        CHECK(health[0] == Stats::Unjudged);
        CHECK(health[1] == Stats::Unjudged);
        const Stats::Status status = Stats::status();
        CHECK(status.count == Stats::count());
        CHECK(status.generation == Stats::generation());
        CHECK(status.watchers == 0);

        // Overrides apply to the listing and the rules.
        REQUIRE(Stats::setOverrides("test.*.fill_us_max:warn=1000").isOk());
        for (size_t i = 0; Stats::info(i, info).isOk(); ++i)
                if (info.id == g.idOf(3)) CHECK(info.limits.warn == 1000);
        REQUIRE(Stats::setOverrides("").isOk());

        Stats::remove(g);
        CHECK(!g.isAdded());
        CHECK(Stats::count() == before);
        CHECK(Stats::read(Span<const uint32_t>(ids, 1), Span<int64_t>(values, 1)) == 1);
        CHECK(values[0] == Stat::Unavailable);
        // Added again: new ids.
        REQUIRE(Stats::add(g).isOk());
        CHECK(g.idOf(0) != lateId);
}

TEST_CASE("Stats: sampling judges, logs problems, and the earlier ones") {
        REQUIRE(Stats::init().isOk());
        Stats::clearProblems();
        const uint64_t start = Stats::lastProblemChange();
        FakeDevice     dev;
        dev.late = 2; // before anyone looked
        Stats::Group g(FakeDescs, FakeCount, readFake, &dev, adjustFake);
        g.setName("test.dsp", 1);
        REQUIRE(Stats::add(g).isOk());

        Stats::sample(0);
        CHECK(problemsAfter(start) == "test.dsp1.late:critical earlier;");
        const uint64_t afterFirst = Stats::lastProblemChange();

        dev.busyUs = 600000; // 60 % of a second
        dev.late = 3;
        Stats::sample(1000);
        CHECK(problemsAfter(afterFirst) == "test.dsp1.late:critical open;test.dsp1.busy_us:warning open;");
        CHECK(Stats::unseenProblems() == Health::Critical);

        dev.busyUs += 100000; // 10 %
        dev.state = 2;        // faulted
        dev.fillMax = 4500;   // past 0.8 of the period
        const uint64_t mark = Stats::lastProblemChange();
        Stats::sample(2000);
        CHECK(problemsAfter(mark) ==
              "test.dsp1.late:critical open;test.dsp1.busy_us:warning;test.dsp1.state:critical open;"
              "test.dsp1.fill_us_max:critical open;");

        // A counter unavailable (CPU time off) is never a problem.
        dev.measured = false;
        Stats::sample(3000);
        Stats::Info info;
        for (size_t i = 0; Stats::info(i, info).isOk(); ++i)
                if (info.id == g.idOf(1)) CHECK(info.value == Stat::Unavailable);

        // Removing the group closes its open episodes.
        Stats::remove(g);
        Stat::ProblemLog::Episode out[16];
        const size_t              n = Stats::problems(start, Span<Stat::ProblemLog::Episode>(out, 16));
        for (size_t i = 0; i < n; ++i) CHECK(!out[i].isOpen());
        Stats::markProblemsSeen(Stats::lastProblemChange());
        CHECK(Stats::unseenProblems() == Health::Normal);
        Stats::clearProblems();
        CHECK(Stats::problems(0, Span<Stat::ProblemLog::Episode>(out, 16)) == 0);
}

TEST_CASE("Stats: the sampler runs while watched, with history") {
        REQUIRE(Stats::init().isOk());
        FakeDevice   dev;
        Stats::Group g(FakeDescs, FakeCount, readFake, &dev, adjustFake);
        g.setName("test.dsp", 2);
        REQUIRE(Stats::add(g).isOk());
        int32_t samples[Stats::HistoryDepth];
        CHECK(Stats::history(g.idOf(1), Span<int32_t>(samples, Stats::HistoryDepth)) == 0); // not watched

        Stats::setInterval(100);
        CHECK(Stats::interval() == 100);
        REQUIRE(Stats::watch().isOk());
        REQUIRE(Stats::watch().isOk());
        CHECK(Stats::watchers() == 2);
        // The sampler takes a sample every 100 ms; wait for three.
        for (int i = 0; i < 300 && Stats::history(g.idOf(1), Span<int32_t>(samples, 4)) < 3; ++i) {
                dev.busyUs += 1000;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const size_t n = Stats::history(g.idOf(1), Span<int32_t>(samples, Stats::HistoryDepth));
        CHECK(n >= 3);
        // Judged while watched: health at the last sample, and its time.
        const uint32_t ids[] = {g.idOf(0)};
        int64_t        value = 0;
        uint8_t        health = Stats::Unjudged;
        CHECK(Stats::read(Span<const uint32_t>(ids, 1), Span<int64_t>(&value, 1), Span<uint8_t>(&health, 1)) == 1);
        CHECK(health == uint8_t(Health::Normal));
        CHECK(Stats::status().lastSampleMs > 0);
        CHECK(Stats::status().watchers == 2);
        CHECK(Stats::history(g.idOf(0), Span<int32_t>(samples, 4)) == 0); // not kept
        Stats::unwatch();
        CHECK(Stats::watchers() == 1);
        Stats::unwatch();
        CHECK(Stats::watchers() == 0);
        Stats::unwatch(); // no effect
        // The sampler stops and frees the history.
        for (int i = 0; i < 300 && Stats::history(g.idOf(1), Span<int32_t>(samples, 4)) > 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(Stats::history(g.idOf(1), Span<int32_t>(samples, 4)) == 0);
        // Watched again after a long pause: the pause's increase isn't one interval's (no spike).
        dev.busyUs += 50000000;
        REQUIRE(Stats::watch().isOk());
        for (int i = 0; i < 300 && Stats::history(g.idOf(1), Span<int32_t>(samples, 4)) < 2; ++i) {
                dev.busyUs += 1000;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const size_t again = Stats::history(g.idOf(1), Span<int32_t>(samples, Stats::HistoryDepth));
        CHECK(again >= 2);
        for (size_t i = 0; i < again; ++i) CHECK(samples[i] < 1000000);
        Stats::unwatch();
        for (int i = 0; i < 300 && Stats::history(g.idOf(1), Span<int32_t>(samples, 4)) > 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Stats::setInterval(1);
        CHECK(Stats::interval() == 100);
        Stats::setInterval(Stats::DefaultIntervalMs);
        Stats::remove(g);
        Stats::clearProblems();
}
