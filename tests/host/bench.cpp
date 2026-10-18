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
#include <vector>

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

TEST_CASE("BenchText: a program's descriptor") {
        const char                          *text = "# benchprog's\n"
                                                    "[Bench kernel.syscall]\n"
                                                    "Exec=benchprog syscall\n"
                                                    "Tags=quick\n"
                                                    "Help=A system call\n"
                                                    "\n"
                                                    "[Bench midi.decode]\n"
                                                    "Exec=benchprog midi\n"
                                                    "Tags=quick, screen\n"
                                                    "Param.file=/sd0/p0/share/bench/scale.mid;The MIDI file\n"
                                                    "Param.runs=1\n"
                                                    "[Bench no.exec]\n"
                                                    "Help=skipped: no Exec\n"
                                                    "[Other]\n"
                                                    "Exec=not a bench\n";
        std::vector<BenchText::ProgramBench> found;
        const size_t                         n = BenchText::parseDescriptor(
                text,
                [](void *arg, const BenchText::ProgramBench &b) {
                        static_cast<std::vector<BenchText::ProgramBench> *>(arg)->push_back(b);
                },
                &found);
        REQUIRE(n == 2);
        REQUIRE(found.size() == 2);
        CHECK(std::string(found[0].name) == "kernel.syscall");
        CHECK(std::string(found[0].exec) == "benchprog syscall");
        CHECK(found[0].tags == (bmds::BenchQuick | bmds::BenchProgram));
        CHECK(found[0].paramCount == 0);
        CHECK(std::string(found[1].name) == "midi.decode");
        CHECK(found[1].tags == (bmds::BenchQuick | bmds::BenchScreen | bmds::BenchProgram));
        REQUIRE(found[1].paramCount == 2);
        CHECK(std::string(found[1].params[0].name) == "file");
        CHECK(std::string(found[1].params[0].value) == "/sd0/p0/share/bench/scale.mid");
        CHECK(std::string(found[1].params[0].help) == "The MIDI file");
        CHECK(std::string(found[1].params[1].name) == "runs");
        CHECK(std::string(found[1].params[1].value) == "1");
        CHECK(std::string(found[1].params[1].help).empty());
}

TEST_CASE("BenchText: words and result lines") {
        StringView   words[8];
        const size_t n = BenchText::splitWords("  doom -timedemo  \"demo one\" x", bmds::Span<StringView>(words));
        REQUIRE(n == 4);
        CHECK(words[0] == StringView("doom"));
        CHECK(words[1] == StringView("-timedemo"));
        CHECK(words[2] == StringView("demo one"));
        CHECK(words[3] == StringView("x"));
        CHECK(BenchText::splitWords("a b c", bmds::Span<StringView>(words, 2)) == 2);

        BenchText::ResultLine line;
        REQUIRE(BenchText::parseResultLine("@bench fps 57.61 /s higher", line));
        CHECK(line.metric == StringView("fps"));
        CHECK(line.value == doctest::Approx(57.61));
        CHECK(line.unit == StringView("/s"));
        CHECK(line.higher);
        REQUIRE(BenchText::parseResultLine("@bench getppid_ns 8.5e3 ns lower", line));
        CHECK(line.value == 8500);
        CHECK_FALSE(line.higher);
        CHECK_FALSE(BenchText::parseResultLine("bench fps 1 /s higher", line));
        CHECK_FALSE(BenchText::parseResultLine("@bench fps 1x /s higher", line));
        CHECK_FALSE(BenchText::parseResultLine("@bench fps 1 /s better", line));
        CHECK_FALSE(BenchText::parseResultLine("@bench fps 1 /s", line));
}

extern "C" {
#include <bmds/benchlib.h>
}

TEST_CASE("libbmdsbench: parameters and the runs") {
        char name[] = "benchprog", what[] = "synth", runs[] = "runs=3", warm[] = "warmup=0", font[] = "font=a.sf2",
             bad[] = "count=12x", again[] = "font=b.sf2";
        char        *argv[] = {name, what, runs, warm, font, bad, again};
        bmds_bench_t b;
        bmds_bench_init(&b, 7, argv);
        CHECK(b.runs == 3);
        CHECK(b.warmup == 0);
        CHECK(std::string(bmds_bench_param(&b, "font", "")) == "b.sf2"); // the last given
        CHECK(std::string(bmds_bench_param(&b, "fon", "none")) == "none");
        CHECK(bmds_bench_param_long(&b, "count", 7) == 7); // not a number
        CHECK(bmds_bench_param_long(&b, "runs", 0) == 3);
        char  big[] = "runs=5000";
        char *argv2[] = {name, big};
        bmds_bench_init(&b, 2, argv2);
        CHECK(b.runs == 5); // past the most: the default
        CHECK(b.warmup == 1);
        CHECK(bmds_bench_now_ns() > 0);
}
