/**
 * @file      bench.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The bench command's pure parts (devplan/bench.md): the statistics, which benchmarks a pattern
 * picks, the tags, the results file's name and dates, how figures print; and the Stopwatch.
 */

#include <doctest.h>

#include <string>

#include <bmds/bench.h>
#include <bmds/benchtext.h>
#include <bmds/stopwatch.h>

using bmds::BenchStats;
using bmds::Error;
using bmds::StringView;
namespace BenchText = bmds::BenchText;

namespace {

        std::string str(const bmds::String &s) {
                return std::string(s.data(), s.size());
        }

        // A context with fixed parameters, for paramUnsigned().
        class FakeContext final : public bmds::BenchContext {
                public:
                        StringView param(StringView name) const override {
                                if (name == "mb") return "16";
                                if (name == "bad") return "12x";
                                if (name == "huge") return "99999999999999999999";
                                if (name == "runs") return "7";
                                if (name == "warmup") return "500";
                                return {};
                        }
                        bool  isCancelled() const override { return false; }
                        Error measureWith(StringView, StringView, bmds::BenchBetter, Sample, void *) override {
                                return Error::Ok;
                        }
                        void reportSamples(StringView, StringView, bmds::BenchBetter, bmds::Span<double>) override {}
                        void note(const char *, ...) override {}
        };

} // namespace

TEST_CASE("BenchText: a metric's figures over its runs") {
        double     odd[] = {21.0, 20.0, 22.0, 19.0, 23.0};
        BenchStats s = BenchText::summarize(bmds::Span<double>(odd));
        CHECK(s.runs == 5);
        CHECK(s.median == 21.0);
        CHECK(s.min == 19.0);
        CHECK(s.max == 23.0);
        CHECK(s.mean == 21.0);
        CHECK(s.spread == doctest::Approx(2.0 / 21.0 * 100));
        CHECK(s.noisy); // ±9.5%

        double even[] = {10.0, 10.2, 9.9, 10.1};
        s = BenchText::summarize(bmds::Span<double>(even));
        CHECK(s.median == doctest::Approx(10.05));
        CHECK_FALSE(s.noisy);

        double one[] = {7.5};
        s = BenchText::summarize(bmds::Span<double>(one));
        CHECK(s.runs == 1);
        CHECK(s.median == 7.5);
        CHECK(s.spread == 0);

        s = BenchText::summarize(bmds::Span<double>());
        CHECK(s.runs == 0);
}

TEST_CASE("BenchText: which benchmarks a pattern picks") {
        const uint16_t tags = bmds::BenchQuick | bmds::BenchWrites;
        CHECK(BenchText::matches("storage.sd.read", "storage.sd.read", tags));
        CHECK(BenchText::matches("storage", "storage.sd.read", tags));
        CHECK(BenchText::matches("storage.sd", "storage.sd.read", tags));
        CHECK_FALSE(BenchText::matches("stor", "storage.sd.read", tags));
        CHECK_FALSE(BenchText::matches("storage.sd.read.x", "storage.sd.read", tags));
        CHECK(BenchText::matches("*.read", "storage.sd.read", tags));
        CHECK(BenchText::matches("storage.??.read", "storage.sd.read", tags));
        CHECK_FALSE(BenchText::matches("display.*", "storage.sd.read", tags));
        CHECK(BenchText::matches("@quick", "storage.sd.read", tags));
        CHECK(BenchText::matches("@writes", "storage.sd.read", tags));
        CHECK_FALSE(BenchText::matches("@screen", "storage.sd.read", tags));
        CHECK_FALSE(BenchText::matches("@nonsense", "storage.sd.read", tags));
        CHECK(BenchText::matches("@all", "storage.sd.read", 0));
}

TEST_CASE("BenchText: tags by name") {
        CHECK(BenchText::tagNamed("screen") == bmds::BenchScreen);
        CHECK(BenchText::tagNamed("nope") == 0);
        bmds::String out(bmds::MemSpace::Psram);
        CHECK(BenchText::appendTags(bmds::BenchQuick | bmds::BenchPeer | bmds::BenchProgram, out).isOk());
        CHECK(str(out) == "quick,peer,program");
}

TEST_CASE("BenchText: the results file's name and the dates") {
        BenchText::When when;
        when.year = 2026;
        when.month = 10;
        when.day = 17;
        when.hour = 14;
        when.minute = 3;
        when.second = 5;
        when.offsetMinutes = -7 * 60;
        bmds::String name(bmds::MemSpace::Psram);
        CHECK(BenchText::fileName("e8f60ae2e222", "0.1.0", when, name).isOk());
        CHECK(str(name) == "e8f60ae2e222-0.1.0-20261017-140305.json");
        // What a filesystem can't take, or a name shouldn't hold, becomes _.
        CHECK(BenchText::fileName("a/b c", "1.0+dirty", when, name).isOk());
        CHECK(str(name) == "a_b_c-1.0_dirty-20261017-140305.json");
        CHECK(BenchText::fileName("", "0.1.0", when, name).isOk());
        CHECK(str(name) == "unknown-0.1.0-20261017-140305.json");

        bmds::String iso(bmds::MemSpace::Psram);
        CHECK(BenchText::appendIso8601(when, iso).isOk());
        CHECK(str(iso) == "2026-10-17T14:03:05-07:00");
        when.offsetMinutes = 5 * 60 + 30;
        iso.clear();
        CHECK(BenchText::appendIso8601(when, iso).isOk());
        CHECK(str(iso) == "2026-10-17T14:03:05+05:30");
}

TEST_CASE("BenchText: figures for the terminal") {
        char out[24];
        BenchText::formatValue(21.3456, out, sizeof(out));
        CHECK(std::string(out) == "21.35");
        BenchText::formatValue(0.012345, out, sizeof(out));
        CHECK(std::string(out) == "0.01235");
        BenchText::formatValue(23456.7, out, sizeof(out));
        CHECK(std::string(out) == "23457");
        BenchText::formatValue(999.94, out, sizeof(out));
        CHECK(std::string(out) == "999.9");
}

TEST_CASE("BenchContext: unsigned parameters") {
        FakeContext ctx;
        uint64_t    v = 0;
        CHECK(ctx.paramUnsigned("mb", v).isOk());
        CHECK(v == 16);
        CHECK(ctx.paramUnsigned("bad", v) == Error::Invalid);
        CHECK(ctx.paramUnsigned("huge", v) == Error::Invalid);
        CHECK(ctx.paramUnsigned("missing", v) == Error::Invalid);
        CHECK(ctx.runs() == 7);
        CHECK(ctx.warmup() == 1); // 500 is past the most: the default
}

TEST_CASE("Stopwatch: microseconds and cycles go forward") {
        bmds::Stopwatch watch;
        volatile int    sink = 0;
        for (int i = 0; i < 100000; ++i) sink = sink + i;
        CHECK(watch.elapsedUs() >= 0);
        CHECK(watch.cycles() > 0);
        watch.restart();
        CHECK(watch.elapsedUs() < 1000000);
}
