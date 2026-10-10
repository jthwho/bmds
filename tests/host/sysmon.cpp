/**
 * @file      sysmon.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The System Monitor's pure part (devplan/sysmon.md §3.5): the counters' catalogue over a fake
 * source, rates, health, and values as text.
 */

#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "monitor.h"

using bmds::Error;
using bmds::Monitor;
using bmds::Source;
using bmds::Span;
using bmds::StringView;
namespace Stat = bmds::Stat;

namespace {

        // Counters in a vector, values set by the test, time moved by it.
        class FakeSource final : public Source {
                public:
                        struct Item {
                                        Counter counter;
                                        int64_t value = 0;
                                        uint8_t health = Unjudged;
                        };

                        std::vector<Item> items;
                        uint32_t          generation = 1;
                        int64_t           nowNs = 1000000000;
                        int               listings = 0;

                        void add(const char *name, Stat::Unit unit, Stat::Kind kind, int64_t value,
                                 const char *labels = "") {
                                Item it;
                                it.counter.id = uint32_t(items.size() + 1);
                                it.counter.unit = unit;
                                it.counter.kind = kind;
                                std::snprintf(it.counter.name, sizeof(it.counter.name), "%s", name);
                                std::snprintf(it.counter.labels, sizeof(it.counter.labels), "%s", labels);
                                it.value = value;
                                items.push_back(it);
                                ++generation;
                        }

                        Error counter(size_t index, Counter &out) override {
                                if (index == 0) ++listings;
                                if (index >= items.size()) return Error::NotFound;
                                out = items[index].counter;
                                out.generation = generation;
                                return Error::Ok;
                        }
                        size_t read(Span<const uint32_t> ids, Span<int64_t> values, Span<uint8_t> health,
                                    int64_t &timeNs) override {
                                for (size_t i = 0; i < ids.size(); ++i) {
                                        values[i] = Stat::Unavailable;
                                        if (i < health.size()) health[i] = Unjudged;
                                        for (const Item &it : items) {
                                                if (it.counter.id != ids[i]) continue;
                                                values[i] = it.value;
                                                if (i < health.size()) health[i] = it.health;
                                        }
                                }
                                timeNs = nowNs;
                                return ids.size();
                        }
                        size_t history(uint32_t, Span<int32_t>) override { return 0; }
                        size_t problems(uint64_t, Span<Stat::ProblemLog::Episode>) override { return 0; }
                        Error  watch(bool) override { return Error::Ok; }
                        void   markSeen(uint64_t) override {}
                        void   clearProblems() override {}
                        Status status() override {
                                Status s;
                                s.generation = generation;
                                s.count = uint32_t(items.size());
                                return s;
                        }
                        size_t tasks(Span<Task>) override { return 0; }
                        Error  process(int32_t, bool, Process &) override { return Error::NotExist; }
                        Error  signal(int32_t, int) override { return Error::NotExist; }
        };

        std::string text(Stat::Unit unit, int64_t value) {
                char out[Monitor::TextBytes];
                return Monitor::format(out, unit, value);
        }

} // namespace

TEST_CASE("Monitor: the catalogue, values, rates, health") {
        FakeSource source;
        source.add("cpu.core0.busy_us", Stat::Unit::Micros, Stat::Kind::Total, 1000);
        source.add("audio.dsp0.state", Stat::Unit::Count, Stat::Kind::State, 1, "stopped|running|faulted");
        source.add("audio.dsp0.late", Stat::Unit::Count, Stat::Kind::Total, 0);
        Monitor m(source);
        REQUIRE(m.refresh().isOk());
        CHECK(m.count() == 3);
        CHECK(source.listings == 1);
        const size_t busy = m.find("cpu.core0.busy_us");
        REQUIRE(busy == 0);
        CHECK(m.find("nothing") == Monitor::NotFound);
        CHECK(m.value(busy) == 1000);
        CHECK(m.rate(busy) == Stat::Unavailable); // one read: no rate yet
        CHECK(!m.isJudged(busy));
        CHECK(m.health(busy) == Stat::Health::Normal);

        // Half a second on, 250 ms busy: 500,000 µs a second.
        source.items[0].value += 250000;
        source.nowNs += 500000000;
        source.items[2].value = 3;
        source.items[2].health = uint8_t(Stat::Health::Critical);
        REQUIRE(m.refresh().isOk());
        CHECK(source.listings == 1); // the same generation: not listed again
        CHECK(m.rate(busy) == 500000);
        CHECK(m.rateOf("audio.dsp0.late") == 6);
        CHECK(m.rateOf("audio.dsp0.state") == Stat::Unavailable); // not a total
        CHECK(m.health(2) == Stat::Health::Critical);
        CHECK(m.worst("audio.*") == Stat::Health::Critical);
        CHECK(m.worst("cpu.*") == Stat::Health::Normal);

        char out[Monitor::TextBytes];
        CHECK(std::string(m.formatValue(out, 1)) == "running");
        CHECK(std::string(m.formatRate(out, 0)) == "500 ms/s");
        CHECK(std::string(m.formatRate(out, 1)) == ""); // not a total

        // A total that fell restarted: no rate this time.
        source.items[0].value = 10;
        source.nowNs += 500000000;
        REQUIRE(m.refresh().isOk());
        CHECK(m.rate(busy) == Stat::Unavailable);

        // A counter added: listed again, no rates across the change.
        source.add("storage.sd0.reads", Stat::Unit::Count, Stat::Kind::Total, 5);
        source.nowNs += 500000000;
        REQUIRE(m.refresh().isOk());
        CHECK(source.listings >= 2);
        CHECK(m.count() == 4);
        CHECK(m.valueOf("storage.sd0.reads") == 5);
        CHECK(m.rate(busy) == Stat::Unavailable);
}

TEST_CASE("Monitor: values as text") {
        using Stat::Unit;
        CHECK(text(Unit::Bytes, Stat::Unavailable) == "-");
        CHECK(text(Unit::Bytes, 512) == "512 B");
        CHECK(text(Unit::Bytes, 1536) == "1.50 KB");
        CHECK(text(Unit::Bytes, 32 * 1024 * 1024) == "32.0 MB");
        CHECK(text(Unit::Bytes, 300 * 1024) == "300 KB");
        CHECK(text(Unit::Micros, 999) == "999 µs");
        CHECK(text(Unit::Micros, 3400) == "3.40 ms");
        CHECK(text(Unit::Micros, 69000) == "69.0 ms");
        CHECK(text(Unit::Micros, 2500000) == "2.50 s");
        CHECK(text(Unit::Nanos, 1500) == "1.50 µs");
        CHECK(text(Unit::Permille, 125) == "12.5 %");
        CHECK(text(Unit::Hz, 48000) == "48000 Hz");
        CHECK(text(Unit::Hz, 12288000) == "12.3 MHz");
        CHECK(text(Unit::MilliHz, 47999987) == "47999.987 Hz");
        CHECK(text(Unit::Ppb, -1200) == "-1200 ppb");
        CHECK(text(Unit::Count, 1234567) == "1234567");
        CHECK(text(Unit::Micros, -3400) == "-3.40 ms");
        CHECK(text(Unit::Decibels, -10) == "-10 dB");
}
