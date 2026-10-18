/**
 * @file      benchcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `bench` command (devplan/bench.md): lists the benchmarks registered with `BMDS_BENCH`,
 * runs the ones named, and writes a JSON file a run in `bench/folder`: the system's facts, every
 * metric's figures, and what every sysmon counter and task did meanwhile. The analysis is the
 * host's (tools/bmds-bench). Its options, the file's format and the benchmarks' names are a
 * userspace interface once shipped: append only.
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <sdkconfig.h>
#include <bmds/bench.h>
#include <bmds/benchtext.h>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/jsonwriter.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/stats.h>
#include <bmds/semaphore.h>
#include <bmds/stopwatch.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/task.h>
#include <bmds/tasklist.h>
#include <bmds/version.h>
#include <bmds/vfs.h>

// The table: the linker fragment collects every `.bmds_bench.*` section here, sorted by name.
extern "C" const bmds::Bench _bmds_bench_start[];
extern "C" const bmds::Bench _bmds_bench_end[];

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(benchFolder, "bench/folder", Config::Type::Path, "/home/bench",
                     "Where 'bench run' saves its results, a JSON file a run", 0, 255, nullptr, nullptr)

        using Args = Span<char *const>;

        constexpr uint32_t MaxRuns = 1000;
        constexpr uint32_t MaxWarmup = 100;
        constexpr size_t   MaxMetrics = 32;
        constexpr size_t   MaxTasks = 96;
        constexpr size_t   MaxNotesBytes = 4096;

        // Every benchmark's parameters (one may set its own defaults for them).
        constexpr BenchParam CommonParams[] = {
                {"runs", "5", "Runs of each metric, after the warm-up (1-1000)"},
                {"warmup", "1", "Runs first, not kept (0-100)"},
                {"probe", "none", "stall: how long the whole system stood still meanwhile"},
        };

        bool isCommon(StringView name) {
                for (const BenchParam &p : CommonParams) {
                        if (name == p.name) return true;
                }
                return false;
        }

        Span<const Bench> benches() {
                return Span<const Bench>(_bmds_bench_start, static_cast<size_t>(_bmds_bench_end - _bmds_bench_start));
        }

        const Bench *findBench(StringView name) {
                for (const Bench &b : benches()) {
                        if (name == StringView(b.name)) return &b;
                }
                return nullptr;
        }

        // A benchmark that can't run here and now (the display the desktop's, no card): skipped, not failed.
        bool isSkipped(Error err) {
                return err == Error::Busy || err == Error::NotReady || err == Error::NoMedia ||
                       err == Error::NotSupported;
        }

        bool isAssignment(const char *arg) {
                return std::strchr(arg, '=') != nullptr && arg[0] != '=' && arg[0] != '@';
        }

        // ---- Listing ----

        int list(CommandContext &ctx, Args patterns) {
                String tags(MemSpace::Psram);
                size_t shown = 0;
                for (const Bench &b : benches()) {
                        bool picked = patterns.size() == 0;
                        for (char *p : patterns) picked = picked || BenchText::matches(p, b.name, b.tags);
                        if (!picked) continue;
                        tags.clear();
                        (void)BenchText::appendTags(b.tags, tags);
                        ctx.printf("%-24s %-14s %s\n", b.name, tags.cStr(), b.help);
                        ++shown;
                }
                if (shown == 0 && patterns.size() > 0) {
                        ctx.eprintf("bench: no benchmark matches\n");
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        int info(CommandContext &ctx, StringView name) {
                const Bench *b = findBench(name);
                if (b == nullptr) {
                        ctx.eprintf("bench: %.*s: no such benchmark\n", int(name.size()), name.data());
                        return Shell::ExitFailure;
                }
                String tags(MemSpace::Psram);
                (void)BenchText::appendTags(b->tags, tags);
                ctx.printf("%s: %s\n", b->name, b->help);
                ctx.printf("Tags:       %s\n", tags.isEmpty() ? "-" : tags.cStr());
                ctx.printf("Parameters:\n");
                for (size_t i = 0; i < b->paramCount; ++i) {
                        const BenchParam &p = b->params[i];
                        if (!isCommon(p.name)) ctx.printf("  %-10s %-8s %s\n", p.name, p.defaultValue, p.help);
                }
                for (const BenchParam &common : CommonParams) {
                        const char *value = common.defaultValue; // the bench's own default, if it sets one
                        for (size_t i = 0; i < b->paramCount; ++i) {
                                if (StringView(b->params[i].name) == common.name) value = b->params[i].defaultValue;
                        }
                        ctx.printf("  %-10s %-8s %s\n", common.name, value, common.help);
                }
                return Shell::ExitSuccess;
        }

        // ---- The system's facts ----

        // Now in the shell's zone ($TZ, else the system's), as `date` shows it.
        BenchText::When now(const CommandContext &ctx) {
                const LocalTime t = ctx.displayZone().localTime(Clock::now());
                BenchText::When w;
                w.year = t.local.year;
                w.month = t.local.month;
                w.day = t.local.day;
                w.hour = t.local.hour;
                w.minute = t.local.minute;
                w.second = t.local.second;
                w.offsetMinutes = t.utcOffset / 60;
                return w;
        }

        void writeTime(const CommandContext &ctx, JsonWriter &json, StringView key) {
                String text(MemSpace::Psram);
                (void)BenchText::appendIso8601(now(ctx), text);
                json.member(key, text.view());
                return;
        }

        void writeSettings(JsonWriter &json) {
                json.key("settings"); // those not at their defaults (a secret reads as "(set)")
                json.beginObject();
                struct Walk {
                                JsonWriter *json;
                } walk{&json};
                (void)SysConf::list(
                        "",
                        [](void *arg, const SysConf::Info &i) {
                                if (i.isDefault || i.param == nullptr) return;
                                static_cast<Walk *>(arg)->json->member(i.param->key, i.value.view());
                                return;
                        },
                        &walk);
                json.endObject();
                return;
        }

        void writeSystem(JsonWriter &json, Span<TaskList::Info> tasks) {
                const SystemInfo info = System::info();
                char             hash[65] = {};
                for (size_t i = 0; i < 32; ++i) std::snprintf(hash + 2 * i, 3, "%02x", info.imageSha256[i]);
                char revision[16];
                std::snprintf(revision, sizeof(revision), "v%u.%u", info.chipRevMajor, info.chipRevMinor);
                json.key("system");
                json.beginObject();
                json.member("serial", System::serial());
                json.member("version", info.bmdsVersion);
                json.member("build", info.buildVersion);
                json.member("image_sha256", hash);
                json.member("idf", info.idfVersion);
                json.member("board", info.boardName);
                json.member("chip_revision", revision);
                json.member("cores", info.cores);
                json.member("cpu_mhz", info.cpuMhz);
                json.member("psram_mhz", CONFIG_SPIRAM_SPEED);
                json.member("uptime_us", System::uptimeUs());
                json.key("mounts");
                json.beginArray();
                Vfs::MountInfo mounts[16];
                const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
                for (size_t i = 0; i < n; ++i) {
                        json.beginObject();
                        json.member("path", mounts[i].path);
                        json.member("device", mounts[i].device);
                        json.member("type", mounts[i].type);
                        json.endObject();
                }
                json.endArray();
                writeSettings(json);
                json.key("tasks");
                json.beginArray();
                for (const TaskList::Info &t : tasks) {
                        json.beginObject();
                        json.member("name", t.name);
                        json.member("priority", t.priority);
                        json.member("core", t.core);
                        json.member("cpu_us", t.cpuUs);
                        json.endObject();
                }
                json.endArray();
                json.endObject();
                return;
        }

        // ---- Counters and tasks before and after ----

        struct Counters {
                        size_t    count = 0;
                        size_t    capacity = 0;
                        uint32_t *ids = nullptr;
                        int64_t  *before = nullptr;
                        int64_t  *after = nullptr;
                        uint8_t  *kinds = nullptr;
                        uint8_t  *rules = nullptr;
                        char (*names)[Stat::MaxName + 1] = nullptr;

                        Error allocate(size_t n) {
                                capacity = n;
                                ids = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                                before = static_cast<int64_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(int64_t)));
                                after = static_cast<int64_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(int64_t)));
                                kinds = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, n));
                                rules = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, n));
                                names = static_cast<char(*)[Stat::MaxName + 1]>(
                                        Memory::alloc(MemSpace::Psram, n * (Stat::MaxName + 1)));
                                if (ids == nullptr || before == nullptr || after == nullptr || kinds == nullptr ||
                                    rules == nullptr || names == nullptr)
                                        return Error::NoMem;
                                return Error::Ok;
                        }

                        void release() {
                                Memory::free(ids);
                                Memory::free(before);
                                Memory::free(after);
                                Memory::free(kinds);
                                Memory::free(rules);
                                Memory::free(names);
                                *this = Counters();
                                return;
                        }

                        // Lists the counters now and reads them into `before`.
                        void start() {
                                count = 0;
                                Stats::Info info;
                                for (size_t i = 0; count < capacity && Stats::describe(i, info).isOk(); ++i) {
                                        ids[count] = info.id;
                                        kinds[count] = uint8_t(info.desc.kind);
                                        rules[count] = uint8_t(info.limits.rule);
                                        std::memcpy(names[count], info.name, sizeof(names[count]));
                                        ++count;
                                }
                                (void)Stats::read(Span<const uint32_t>(ids, count), Span<int64_t>(before, count));
                                return;
                        }

                        void stop() {
                                (void)Stats::read(Span<const uint32_t>(ids, count), Span<int64_t>(after, count));
                                return;
                        }

                        bool known(size_t i) const {
                                return before[i] != Stat::Unavailable && after[i] != Stat::Unavailable;
                        }
        };

        struct Tasks {
                        TaskList::Info *before = nullptr;
                        TaskList::Info *after = nullptr;
                        size_t          beforeCount = 0;
                        size_t          afterCount = 0;

                        Error allocate() {
                                before = static_cast<TaskList::Info *>(
                                        Memory::alloc(MemSpace::Psram, MaxTasks * sizeof(TaskList::Info)));
                                after = static_cast<TaskList::Info *>(
                                        Memory::alloc(MemSpace::Psram, MaxTasks * sizeof(TaskList::Info)));
                                return before != nullptr && after != nullptr ? Error::Ok : Error::NoMem;
                        }

                        void release() {
                                Memory::free(before);
                                Memory::free(after);
                                *this = Tasks();
                                return;
                        }

                        void start() {
                                beforeCount = TaskList::list(Span<TaskList::Info>(before, MaxTasks));
                                return;
                        }

                        void stop() {
                                afterCount = TaskList::list(Span<TaskList::Info>(after, MaxTasks));
                                return;
                        }
        };

        // `probe=stall`: the rest of the system as a loop in flash at the idle tasks' priority on
        // the core the benchmark isn't on (on its own, the benchmark's work would count as
        // stopped): it stops whenever the flash does (the caches off), so the counts it loses
        // against its rate measured first are how long the whole system stood still. At priority
        // 0 it takes only idle time.
        class StallProbe {
                public:
                        static constexpr unsigned CalibrateMs = 500;

                        ~StallProbe() { (void)stop(0); }

                        void start() {
                                if (_shared != nullptr) return;
                                void *mem = Memory::alloc(MemSpace::Internal, sizeof(Shared));
                                if (mem == nullptr) return;
                                _shared = ::new (mem) Shared();
                                Task::Config tc;
                                tc.name = "benchspin";
                                tc.stackBytes =
                                        4096; // with 2 KB, a panic on core 0 hung in its handler, unseen until a watchdog
                                tc.priority = 0;
                                tc.core = Task::currentCore() == 0 ? 1 : 0;
                                if (_shared->task.start(tc, spin, _shared).isError()) {
                                        release();
                                        return;
                                }
                                const uint32_t first = _shared->count;
                                Task::sleep(CalibrateMs);
                                _rate = double(_shared->count - first) / (CalibrateMs * 1000.0); // counts a µs
                                _start = _shared->count;
                                return;
                        }

                        // Stops it; the milliseconds lost in the last @p windowUs.
                        double stop(int64_t windowUs) {
                                if (_shared == nullptr) return 0;
                                const uint32_t counted = _shared->count - _start;
                                _shared->run = false;
                                (void)_shared->stopped.acquire(WaitForever);
                                (void)_shared->task.stop();
                                release();
                                const double expected = _rate * double(windowUs);
                                return _rate > 0 && expected > counted ? (expected - counted) / _rate / 1000.0 : 0;
                        }

                private:
                        struct Shared {
                                        volatile uint32_t count = 0;
                                        volatile bool     run = true;
                                        Task              task;
                                        Semaphore         stopped{1, 0};
                        };

                        static void spin(void *arg) {
                                auto *s = static_cast<Shared *>(arg);
                                while (s->run) s->count = s->count + 1;
                                s->stopped.release();
                                for (;;) Task::sleep(1000);
                        }

                        void release() {
                                _shared->~Shared();
                                Memory::free(_shared);
                                _shared = nullptr;
                                return;
                        }

                        Shared  *_shared = nullptr;
                        double   _rate = 0;
                        uint32_t _start = 0;
        };

        // ---- Running ----

        struct Metric {
                        char        name[BenchContext::MaxMetricName + 1];
                        char        unit[BenchContext::MaxUnit + 1];
                        BenchBetter better;
                        BenchStats  stats;
        };

        void copyText(char *out, size_t outBytes, StringView text) {
                const size_t n = text.size() < outBytes - 1 ? text.size() : outBytes - 1;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

        class Runner final : public BenchContext {
                public:
                        Runner(CommandContext &ctx, Args assignments, bool quiet)
                            : _ctx(ctx), _assignments(assignments), _quiet(quiet) {}

                        ~Runner() override {
                                Memory::free(_samples);
                                Memory::free(_metrics);
                                _counters.release();
                                _tasks.release();
                        }

                        Error allocate() {
                                _samples = static_cast<double *>(
                                        Memory::alloc(MemSpace::Psram, (MaxRuns + MaxWarmup) * sizeof(double)));
                                _metrics = static_cast<Metric *>(
                                        Memory::alloc(MemSpace::Psram, MaxMetrics * sizeof(Metric)));
                                if (_samples == nullptr || _metrics == nullptr) return Error::NoMem;
                                if (Error err = _counters.allocate(Stats::count() + 32); err.isError()) return err;
                                return _tasks.allocate();
                        }

                        Tasks &tasks() { return _tasks; }

                        StringView param(StringView name) const override {
                                // The line's last assignment wins, then the bench's default.
                                StringView found;
                                bool       given = false;
                                for (char *a : _assignments) {
                                        const StringView text(a);
                                        const size_t     eq = text.indexOf('=');
                                        if (text.left(eq) == name) {
                                                found = text.mid(eq + 1);
                                                given = true;
                                        }
                                }
                                if (given) return found;
                                if (_bench != nullptr) { // a bench may set its own default for a common one
                                        for (size_t i = 0; i < _bench->paramCount; ++i) {
                                                if (name == StringView(_bench->params[i].name))
                                                        return _bench->params[i].defaultValue;
                                        }
                                }
                                if (name == "runs") return "5";
                                if (name == "warmup") return "1";
                                if (name == "probe") return "none";
                                return {};
                        }

                        bool isCancelled() const override { return _ctx.isInterrupted(); }

                        Error measureWith(StringView metric, StringView unit, BenchBetter better, Sample sample,
                                          void *arg) override {
                                const uint32_t count = runs();
                                for (uint32_t i = 0; i < warmup(); ++i) {
                                        if (isCancelled()) return Error::Cancelled;
                                        double discard = 0;
                                        if (Error err = sample(arg, discard); err.isError()) return err;
                                }
                                size_t kept = 0;
                                for (uint32_t i = 0; i < count; ++i) {
                                        if (isCancelled()) return Error::Cancelled;
                                        double v = 0;
                                        if (Error err = sample(arg, v); err.isError()) return err;
                                        _samples[kept++] = v;
                                }
                                add(metric, unit, better, BenchText::summarize(Span<double>(_samples, kept)));
                                return Error::Ok;
                        }

                        void reportSamples(StringView metric, StringView unit, BenchBetter better,
                                           Span<double> samples) override {
                                add(metric, unit, better, BenchText::summarize(samples));
                                return;
                        }

                        void note(const char *fmt, ...) override {
                                char    line[160];
                                va_list args;
                                va_start(args, fmt);
                                std::vsnprintf(line, sizeof(line), fmt, args);
                                va_end(args);
                                if (!_quiet) _ctx.printf("  %s\n", line);
                                if (_notes.size() + std::strlen(line) + 1 > MaxNotesBytes) return;
                                (void)_notes.append(StringView(line));
                                (void)_notes.append('\n');
                                return;
                        }

                        // Runs @p bench; writes its record to @p json (when there is one).
                        Error run(const Bench &bench, JsonWriter *json) {
                                _bench = &bench;
                                _metricCount = 0;
                                _notes.clear();
                                _ctx.printf("%s\n", bench.name);
                                const bool stall = param("probe") == "stall";
                                if (stall) _stall.start();
                                _counters.start();
                                _tasks.start();
                                Stopwatch     watch;
                                const Error   result = bench.run(*this);
                                const int64_t elapsedUs = watch.elapsedUs();
                                _counters.stop();
                                _tasks.stop();
                                if (stall) {
                                        // How long the whole system stood still meanwhile (the flash's writes).
                                        const double stoppedMs = _stall.stop(elapsedUs);
                                        if (result.isOk()) {
                                                report("stopped_ms", "ms", BenchBetter::Lower, stoppedMs);
                                                report("stopped_percent", "%", BenchBetter::Lower,
                                                       elapsedUs > 0 ? stoppedMs * 100000.0 / double(elapsedUs) : 0);
                                        }
                                }
                                if (isSkipped(result)) {
                                        _ctx.printf("  skipped: %s\n", result.desc());
                                } else if (result.isError() && result != Error::Cancelled) {
                                        _ctx.printf("  failed: %s\n", result.desc());
                                }
                                if (!_quiet) printProblems();
                                if (json != nullptr) write(*json, result, elapsedUs);
                                _bench = nullptr;
                                return result;
                        }

                private:
                        void add(StringView metric, StringView unit, BenchBetter better, const BenchStats &stats) {
                                if (_metricCount == MaxMetrics) return;
                                Metric &m = _metrics[_metricCount++];
                                copyText(m.name, sizeof(m.name), metric);
                                copyText(m.unit, sizeof(m.unit), unit);
                                m.better = better;
                                m.stats = stats;
                                char value[24];
                                BenchText::formatValue(stats.median, value, sizeof(value));
                                if (stats.runs > 1) {
                                        _ctx.printf("  %-28s %10s %-8s ±%.1f%% (%u runs)%s\n", m.name, value, m.unit,
                                                    stats.spread, unsigned(stats.runs), stats.noisy ? " noisy" : "");
                                } else {
                                        _ctx.printf("  %-28s %10s %s\n", m.name, value, m.unit);
                                }
                                return;
                        }

                        // The judged counters that went up meanwhile: what the system suffered.
                        void printProblems() {
                                for (size_t i = 0; i < _counters.count; ++i) {
                                        if (!_counters.known(i) || _counters.rules[i] != uint8_t(Stat::Rule::Zero))
                                                continue;
                                        const int64_t delta = _counters.after[i] - _counters.before[i];
                                        if (delta > 0)
                                                _ctx.printf("  meanwhile: %s +%lld\n", _counters.names[i],
                                                            static_cast<long long>(delta));
                                }
                                return;
                        }

                        void write(JsonWriter &json, Error result, int64_t elapsedUs) {
                                json.beginObject();
                                json.member("name", _bench->name);
                                json.key("params");
                                json.beginObject();
                                for (size_t i = 0; i < _bench->paramCount; ++i) {
                                        if (!isCommon(_bench->params[i].name))
                                                json.member(_bench->params[i].name, param(_bench->params[i].name));
                                }
                                for (const BenchParam &common : CommonParams)
                                        json.member(common.name, param(common.name));
                                json.endObject();
                                json.member("status", result.isOk()       ? "ok"
                                                      : isSkipped(result) ? "skipped"
                                                                          : result.name());
                                if (result.isError() && result != Error::Cancelled)
                                        json.member("reason", result.desc());
                                json.member("elapsed_us", elapsedUs);
                                json.key("results");
                                json.beginArray();
                                for (size_t i = 0; i < _metricCount; ++i) {
                                        const Metric &m = _metrics[i];
                                        json.beginObject();
                                        json.member("metric", m.name);
                                        json.member("unit", m.unit);
                                        json.member("better", m.better == BenchBetter::Higher ? "higher" : "lower");
                                        json.member("median", m.stats.median);
                                        json.member("min", m.stats.min);
                                        json.member("max", m.stats.max);
                                        json.member("mean", m.stats.mean);
                                        json.member("spread", m.stats.spread);
                                        json.member("runs", m.stats.runs);
                                        json.member("noisy", m.stats.noisy);
                                        json.endObject();
                                }
                                json.endArray();
                                json.key("notes");
                                json.beginArray();
                                StringView rest = _notes.view();
                                while (!rest.isEmpty()) {
                                        const size_t nl = rest.indexOf('\n');
                                        json.value(rest.left(nl));
                                        rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                                }
                                json.endArray();
                                writeCounters(json);
                                writeTasks(json);
                                json.endObject();
                                return;
                        }

                        void writeCounters(JsonWriter &json) {
                                // Totals: how much each grew; levels (and peaks, states, facts): before and after.
                                json.key("totals");
                                json.beginObject();
                                for (size_t i = 0; i < _counters.count; ++i) {
                                        if (!_counters.known(i) || _counters.kinds[i] != uint8_t(Stat::Kind::Total))
                                                continue;
                                        if (_counters.after[i] != _counters.before[i])
                                                json.member(_counters.names[i],
                                                            _counters.after[i] - _counters.before[i]);
                                }
                                json.endObject();
                                json.key("levels");
                                json.beginObject();
                                for (size_t i = 0; i < _counters.count; ++i) {
                                        if (!_counters.known(i) || _counters.kinds[i] == uint8_t(Stat::Kind::Total))
                                                continue;
                                        if (_counters.after[i] == _counters.before[i]) continue;
                                        json.key(_counters.names[i]);
                                        json.beginArray();
                                        json.value(_counters.before[i]);
                                        json.value(_counters.after[i]);
                                        json.endArray();
                                }
                                json.endObject();
                                return;
                        }

                        void writeTasks(JsonWriter &json) {
                                // Each task's CPU time meanwhile, those that ran (matched by handle and number).
                                json.key("tasks");
                                json.beginObject();
                                for (size_t a = 0; a < _tasks.afterCount; ++a) {
                                        const TaskList::Info &t = _tasks.after[a];
                                        uint64_t              was = 0;
                                        for (size_t b = 0; b < _tasks.beforeCount; ++b) {
                                                if (_tasks.before[b].task == t.task &&
                                                    _tasks.before[b].number == t.number) {
                                                        was = _tasks.before[b].cpuUs;
                                                        break;
                                                }
                                        }
                                        if (t.cpuUs > was) json.member(t.name, t.cpuUs - was);
                                }
                                json.endObject();
                                return;
                        }

                        CommandContext &_ctx;
                        Args            _assignments;
                        bool            _quiet;
                        const Bench    *_bench = nullptr;
                        double         *_samples = nullptr;
                        Metric         *_metrics = nullptr;
                        size_t          _metricCount = 0;
                        String          _notes{MemSpace::Psram};
                        Counters        _counters;
                        Tasks           _tasks;
                        StallProbe      _stall;
        };

        // ---- The results file ----

        Error makeFolders(StringView path, const Vfs::Context &context) {
                for (size_t at = path.indexOf('/', 1);; at = path.indexOf('/', at + 1)) {
                        const StringView folder = at == StringView::NPos ? path : path.left(at);
                        if (!Vfs::stat(folder, context).isOk()) {
                                const Error err = Vfs::mkdir(folder, context);
                                if (err.isError() && err != Error::Exists) return err;
                        }
                        if (at == StringView::NPos) return Error::Ok;
                }
        }

        struct Output {
                        FileHandle::Ptr file;

                        static Error sink(void *arg, StringView bytes) {
                                return static_cast<Output *>(arg)->file->write(bytes, WaitForever).error();
                        }
        };

        Error openResults(CommandContext &ctx, const char *given, String &path, Output &out) {
                const Vfs::Context context = ctx.vfs();
                if (given != nullptr) {
                        if (Error err = path.assign(given); err.isError()) return err;
                } else {
                        String folder(MemSpace::Psram);
                        if (Error err = SysConf::get("bench/folder", folder); err.isError()) return err;
                        if (Error err = makeFolders(folder.view(), context); err.isError()) return err;
                        String name(MemSpace::Psram);
                        if (Error err = BenchText::fileName(System::serial(), VersionString, now(ctx), name);
                            err.isError())
                                return err;
                        if (Error err = path.assign(folder.view()); err.isError()) return err;
                        if (!path.view().endsWith('/')) (void)path.append('/');
                        if (Error err = path.append(name.view()); err.isError()) return err;
                }
                const uint8_t flags = Vfs::Write | Vfs::Create | (given != nullptr ? Vfs::Truncate : Vfs::Exclusive);
                auto [file, err] = Vfs::open(path.view(), flags, context);
                if (err.isError()) return err;
                out.file = std::move(file);
                return Error::Ok;
        }

        int run(CommandContext &ctx, Args args) {
                const char *outPath = nullptr;
                bool        save = true;
                bool        quiet = false;
                // Options first, then names, patterns and tags, with KEY=VALUE anywhere among them.
                size_t i = 2;
                for (; i < args.size() && args[i][0] == '-' && args[i][1] != '\0'; ++i) {
                        if (std::strcmp(args[i], "-o") == 0 && i + 1 < args.size()) {
                                outPath = args[++i];
                        } else if (std::strcmp(args[i], "-n") == 0) {
                                save = false;
                        } else if (std::strcmp(args[i], "-q") == 0) {
                                quiet = true;
                        } else {
                                return ctx.usage();
                        }
                }
                char  *patterns[32];
                char  *assignments[32];
                size_t patternCount = 0;
                size_t assignmentCount = 0;
                for (; i < args.size(); ++i) {
                        if (isAssignment(args[i])) {
                                if (assignmentCount == 32) return ctx.usage();
                                assignments[assignmentCount++] = args[i];
                        } else {
                                if (patternCount == 32) return ctx.usage();
                                patterns[patternCount++] = args[i];
                        }
                }
                if (patternCount == 0) return ctx.usage(); // always named (B4)
                const Span<char *const> picks(patterns, patternCount);
                const Span<char *const> sets(assignments, assignmentCount);

                // What runs, and whether every KEY is somebody's.
                const Bench *chosen[64];
                size_t       chosenCount = 0;
                for (const Bench &b : benches()) {
                        bool picked = false;
                        for (char *p : picks) picked = picked || BenchText::matches(p, b.name, b.tags);
                        if (picked && chosenCount < 64) chosen[chosenCount++] = &b;
                }
                if (chosenCount == 0) {
                        ctx.eprintf("bench: no benchmark matches\n");
                        return Shell::ExitFailure;
                }
                for (char *a : sets) {
                        const StringView text(a);
                        const StringView key = text.left(text.indexOf('='));
                        const StringView value = text.mid(text.indexOf('=') + 1);
                        bool             known = isCommon(key);
                        for (size_t c = 0; c < chosenCount && !known; ++c) {
                                for (size_t p = 0; p < chosen[c]->paramCount; ++p)
                                        known = known || key == StringView(chosen[c]->params[p].name);
                        }
                        if (!known) {
                                ctx.eprintf("bench: %.*s: no benchmark chosen takes it\n", int(key.size()), key.data());
                                return Shell::ExitUsage;
                        }
                        if (key == "probe" && value != "none" && value != "stall") {
                                ctx.eprintf("bench: %s: none or stall\n", a);
                                return Shell::ExitUsage;
                        }
                        if (key == "runs" || key == "warmup") {
                                uint64_t v = 0;
                                bool     digits = !value.isEmpty() && value.size() < 6;
                                for (char ch : value) digits = digits && ch >= '0' && ch <= '9';
                                if (digits) {
                                        for (char ch : value) v = v * 10 + uint64_t(ch - '0');
                                }
                                const uint64_t most = key == "runs" ? MaxRuns : MaxWarmup;
                                if (!digits || v > most || (key == "runs" && v == 0)) {
                                        ctx.eprintf("bench: %s: 0-%u (runs: 1-%u)\n", a, unsigned(MaxWarmup),
                                                    unsigned(MaxRuns));
                                        return Shell::ExitUsage;
                                }
                        }
                }

                Runner runner(ctx, sets, quiet);
                if (Error err = runner.allocate(); err.isError()) {
                        ctx.eprintf("bench: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                String     path(MemSpace::Psram);
                Output     out;
                JsonWriter json(Output::sink, &out);
                if (save) {
                        if (Error err = openResults(ctx, outPath, path, out); err.isError()) {
                                ctx.eprintf("bench: %s: %s\n", path.isEmpty() ? "results" : path.cStr(), err.desc());
                                return Shell::ExitFailure;
                        }
                        json.setIndent(1);
                        json.beginObject();
                        json.member("format", 1);
                        String command(MemSpace::Psram);
                        for (size_t a = 0; a < args.size(); ++a) {
                                if (a > 0) (void)command.append(' ');
                                (void)command.append(StringView(args[a]));
                        }
                        json.member("command", command.view());
                        writeTime(ctx, json, "start");
                        runner.tasks().start();
                        writeSystem(json, Span<TaskList::Info>(runner.tasks().before, runner.tasks().beforeCount));
                        json.key("counters"); // every sysmon counter at the start
                        json.beginObject();
                        Stats::Info info;
                        for (size_t c = 0; Stats::info(c, info).isOk(); ++c) {
                                if (info.value != Stat::Unavailable) json.member(info.name, info.value);
                        }
                        json.endObject();
                        json.key("benches");
                        json.beginArray();
                }
                int status = Shell::ExitSuccess;
                for (size_t c = 0; c < chosenCount; ++c) {
                        const Error err = runner.run(*chosen[c], save ? &json : nullptr);
                        if (err == Error::Cancelled || ctx.isInterrupted()) {
                                status = Shell::ExitInterrupted;
                                break;
                        }
                        if (err.isError() && !isSkipped(err)) status = Shell::ExitFailure;
                }
                if (save) {
                        json.endArray();
                        writeTime(ctx, json, "end");
                        json.endObject();
                        const Error err = json.finish();
                        out.file.reset();
                        if (err.isError()) {
                                ctx.eprintf("bench: %s: %s\n", path.cStr(), err.desc());
                                return Shell::ExitFailure;
                        }
                        ctx.printf("saved %s\n", path.cStr());
                }
                return status;
        }

        int runBench(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return list(ctx, Args());
                const StringView verb(args[1]);
                if (verb == "list") return list(ctx, args.mid(2));
                if (verb == "info" && args.size() == 3) return info(ctx, args[2]);
                if (verb == "run") return run(ctx, args);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(bench, runBench,
                   "[list [PATTERN|@TAG...] | info NAME | run [-o FILE] [-n] [-q] PATTERN|@TAG... [KEY=VALUE...]]",
                   "List the benchmarks, or run those named, saving the results in bench/folder")

BMDS_NAMESPACE_END
