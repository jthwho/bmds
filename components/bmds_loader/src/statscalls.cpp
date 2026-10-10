/**
 * @file      statscalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system monitor's calls (target only, devplan/sysmon.md §3.6): the counters'
 * registry, the tasks and the processes, as the ABI's structs.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/cputime.h>
#include <bmds/errno.h>
#include <bmds/loader.h>
#include <bmds/memory.h>
#include <bmds/stats.h>
#include <bmds/sys.h>
#include <bmds/tasklist.h>
#include "running.h"
#include "structcopy.h"

using bmds::KernelCall;
using bmds::Loader;
using bmds::Span;
using bmds::detail::copyIn;
using bmds::detail::copyOut;
using bmds::detail::copyText;
using bmds::Stat::Health;
using bmds::Stat::Kind;
using bmds::Stat::Rule;
using bmds::Stat::Unit;
namespace Stat = bmds::Stat;
namespace Stats = bmds::Stats;

namespace {

        static_assert(BMDS_UNIT_COUNT == uint32_t(Unit::Count) && BMDS_UNIT_BYTES == uint32_t(Unit::Bytes) &&
                              BMDS_UNIT_MICROS == uint32_t(Unit::Micros) &&
                              BMDS_UNIT_PERMILLE == uint32_t(Unit::Permille) && BMDS_UNIT_HZ == uint32_t(Unit::Hz) &&
                              BMDS_UNIT_MILLIHZ == uint32_t(Unit::MilliHz) && BMDS_UNIT_PPM == uint32_t(Unit::Ppm) &&
                              BMDS_UNIT_FRAMES == uint32_t(Unit::Frames) &&
                              BMDS_UNIT_CYCLES == uint32_t(Unit::Cycles) &&
                              BMDS_UNIT_DECIBELS == uint32_t(Unit::Decibels) &&
                              BMDS_UNIT_NANOS == uint32_t(Unit::Nanos) && BMDS_UNIT_PPB == uint32_t(Unit::Ppb),
                      "the ABI's units are Stat's");
        static_assert(BMDS_COUNTER_TOTAL == uint32_t(Kind::Total) && BMDS_COUNTER_LEVEL == uint32_t(Kind::Level) &&
                              BMDS_COUNTER_PEAK == uint32_t(Kind::Peak) &&
                              BMDS_COUNTER_STATE == uint32_t(Kind::State) && BMDS_COUNTER_INFO == uint32_t(Kind::Info),
                      "the ABI's kinds are Stat's");
        static_assert(BMDS_COUNTER_HISTORY == Stat::KeepHistory && BMDS_COUNTER_ADJUSTED == Stat::Adjusted,
                      "the ABI's flags are Stat's");
        static_assert(BMDS_RULE_NONE == uint32_t(Rule::None) && BMDS_RULE_ZERO == uint32_t(Rule::Zero) &&
                              BMDS_RULE_ABOVE == uint32_t(Rule::Above) && BMDS_RULE_BELOW == uint32_t(Rule::Below) &&
                              BMDS_RULE_DRIFT == uint32_t(Rule::Drift) && BMDS_RULE_STATE == uint32_t(Rule::State),
                      "the ABI's rules are Stat's");
        static_assert(BMDS_HEALTH_NORMAL == uint32_t(Health::Normal) &&
                              BMDS_HEALTH_WARNING == uint32_t(Health::Warning) &&
                              BMDS_HEALTH_CRITICAL == uint32_t(Health::Critical) &&
                              BMDS_HEALTH_UNJUDGED == Stats::Unjudged,
                      "the ABI's health is Stat's");
        static_assert(BMDS_COUNTER_UNAVAILABLE == Stat::Unavailable && BMDS_COUNTER_NO_LEVEL == Stat::NoLevel,
                      "the ABI's markers are Stat's");
        static_assert(BMDS_COUNTER_NAME_MAX == Stat::MaxName && sizeof(bmds_counter_t::name) == Stat::MaxName + 1 &&
                              sizeof(bmds_problem_t::name) == Stat::MaxName + 1,
                      "the ABI's names are Stat's");
        static_assert(BMDS_TASK_RUNNING == uint32_t(bmds::TaskList::State::Running) &&
                              BMDS_TASK_READY == uint32_t(bmds::TaskList::State::Ready) &&
                              BMDS_TASK_BLOCKED == uint32_t(bmds::TaskList::State::Blocked) &&
                              BMDS_TASK_SUSPENDED == uint32_t(bmds::TaskList::State::Suspended) &&
                              BMDS_TASK_DELETED == uint32_t(bmds::TaskList::State::Deleted),
                      "the ABI's task states are FreeRTOS's");
        static_assert(sizeof(bmds_task_info_t::name) == bmds::TaskList::NameBytes, "the ABI's task names");
        static_assert(sizeof(bmds_proc_info_t::command) == Loader::CommandBytes, "the ABI's command lines");

        // The most ids one stats_read takes (a bound on the time it holds the registry).
        constexpr uint32_t MaxReadIds = 1024;

        int64_t msToNs(int64_t ms) {
                return ms * 1000000;
        }

        // An array of the ABI's structs: the stride is the first entry's `size` (as io_poll's).
        template <typename T> int32_t strideOf(const T *out, uint32_t max, size_t &stride) {
                if (max == 0) return 0;
                if (out == nullptr) return -BMDS_EFAULT;
                stride = out->size;
                return stride < sizeof(uint32_t) ? -BMDS_EINVAL : 0;
        }

        // Puts @p full in entry @p i of an array with that stride, its `size` set to the stride.
        template <typename T> void putEntry(const T &full, T *out, size_t stride, size_t i) {
                auto *entry = reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(out) + i * stride);
                entry->size = uint32_t(stride);
                (void)copyOut(full, entry);
                return;
        }

        uint32_t procState(Loader::ProcessInfo::State state) {
                using State = Loader::ProcessInfo::State;
                switch (state) {
                        case State::Running: return BMDS_PROC_RUNNING;
                        case State::Vfork: return BMDS_PROC_VFORK;
                        case State::Ending: return BMDS_PROC_ENDING;
                        case State::Zombie: return BMDS_PROC_ZOMBIE;
                        case State::Stopped: return BMDS_PROC_STOPPED;
                }
                return BMDS_PROC_RUNNING;
        }

} // namespace

extern "C" {

int32_t bmds_k_stats_list(uint32_t index, bmds_counter_t *info) {
        KernelCall     call;
        bmds_counter_t c;
        int32_t        r = copyIn(info, c);
        if (r < 0) return r;
        Stats::Info k;
        if (Stats::describe(index, k).isError()) return -BMDS_ENOENT;
        c = bmds_counter_t{};
        c.id = k.id;
        c.generation = k.generation;
        c.unit = uint32_t(k.desc.unit);
        c.kind = uint32_t(k.desc.kind);
        c.flags = k.desc.flags;
        c.rule = uint32_t(k.limits.rule);
        c.health = uint32_t(k.limits.health);
        const bool event = k.limits.rule == Rule::Zero || (k.limits.rule == Rule::Above && k.desc.kind == Kind::Peak);
        c.hold = k.limits.hold != 0 || !event ? k.limits.hold : Stat::DefaultEventHold;
        c.warn = k.limits.warn;
        c.crit = k.limits.crit;
        copyText(bmds::StringView(k.name), c.name, sizeof(c.name));
        copyText(bmds::StringView(k.desc.labels), c.labels, sizeof(c.labels));
        copyText(bmds::StringView(k.desc.help), c.help, sizeof(c.help));
        return copyOut(c, info);
}

int32_t bmds_k_stats_read(const uint32_t *ids, uint32_t count, int64_t *values, uint8_t *health, int64_t *time_ns) {
        KernelCall call;
        if (count > MaxReadIds) return -BMDS_EINVAL;
        if (count > 0 && (ids == nullptr || values == nullptr)) return -BMDS_EFAULT;
        // Program memory is the kernel's to read and write: no copies.
        const size_t n = Stats::read(Span<const uint32_t>(ids, count), Span<int64_t>(values, count),
                                     health != nullptr ? Span<uint8_t>(health, count) : Span<uint8_t>());
        if (time_ns != nullptr) *time_ns = int64_t(bmds::Clock::monotonicNs());
        return int32_t(n);
}

int32_t bmds_k_stats_history(uint32_t id, int64_t *values, uint32_t max, int64_t *newest_ns) {
        KernelCall call;
        if (max > 0 && values == nullptr) return -BMDS_EFAULT;
        int32_t      samples[Stats::HistoryDepth];
        const size_t n = Stats::history(id, Span<int32_t>(samples, Stats::HistoryDepth));
        // The newest ones, if not all fit.
        const size_t first = n > max ? n - max : 0;
        for (size_t i = first; i < n; ++i) values[i - first] = samples[i];
        if (newest_ns != nullptr) *newest_ns = n > 0 ? msToNs(Stats::status().lastSampleMs) : 0;
        return int32_t(n - first);
}

int32_t bmds_k_stats_problems(uint64_t after, bmds_problem_t *out, uint32_t max) {
        KernelCall call;
        size_t     stride = 0;
        int32_t    r = strideOf(out, max, stride);
        if (r < 0 || max == 0) return r;
        // A few at a time: an episode is about 100 bytes, and the log holds 64.
        constexpr size_t          Batch = 8;
        Stat::ProblemLog::Episode episodes[Batch];
        size_t                    done = 0;
        while (done < max) {
                const size_t want = max - done < Batch ? max - done : Batch;
                const size_t n = Stats::problems(after, Span<Stat::ProblemLog::Episode>(episodes, want));
                for (size_t i = 0; i < n; ++i) {
                        const Stat::ProblemLog::Episode &e = episodes[i];
                        bmds_problem_t                   p = {};
                        p.id = e.id;
                        p.health = uint32_t(e.health);
                        p.flags = (e.isOpen() ? BMDS_PROBLEM_OPEN : 0u) |
                                  ((e.flags & Stat::ProblemLog::Earlier) != 0 ? BMDS_PROBLEM_EARLIER : 0u);
                        p.changed = e.changed;
                        p.start_ns = msToNs(e.startMs);
                        p.end_ns = msToNs(e.endMs);
                        p.first_value = e.firstValue;
                        p.last_value = e.lastValue;
                        p.count = e.count;
                        copyText(bmds::StringView(e.name), p.name, sizeof(p.name));
                        putEntry(p, out, stride, done + i);
                        after = e.changed;
                }
                done += n;
                if (n < want) break;
        }
        return int32_t(done);
}

int32_t bmds_k_stats_control(uint32_t op, uint64_t value) {
        KernelCall     call;
        bmds::Running &run = *call.thread().run;
        switch (op) {
                case BMDS_STATS_WATCH:
                        if (value > 1) return -BMDS_EINVAL;
                        if (value == 0) {
                                run.releaseStats();
                                return 0;
                        }
                        if (run.watchingStats.exchange(true)) return 0;
                        if (Stats::watch().isError()) {
                                run.watchingStats.store(false);
                                return -BMDS_ENOMEM;
                        }
                        return 0;
                case BMDS_STATS_SEEN: Stats::markProblemsSeen(value); return 0;
                case BMDS_STATS_CLEAR:
                        if (value != 0) return -BMDS_EINVAL;
                        Stats::clearProblems();
                        return 0;
                default: return -BMDS_EINVAL;
        }
}

int32_t bmds_k_stats_status(bmds_stats_status_t *status) {
        KernelCall          call;
        bmds_stats_status_t s;
        int32_t             r = copyIn(status, s);
        if (r < 0) return r;
        const Stats::Status k = Stats::status();
        s = bmds_stats_status_t{};
        s.generation = k.generation;
        s.count = uint32_t(k.count);
        s.watchers = uint32_t(k.watchers);
        s.interval_ms = k.intervalMs;
        s.flags = (bmds::CpuTime::isEnabled() ? BMDS_STATS_CPU_COUNTED : 0u) |
                  (call.thread().run->watchingStats.load() ? BMDS_STATS_WATCHING : 0u);
        s.unseen = uint32_t(k.unseen);
        s.cores = uint32_t(bmds::CpuTime::cores());
        s.last_change = k.lastProblemChange;
        s.last_sample_ns = msToNs(k.lastSampleMs);
        return copyOut(s, status);
}

int32_t bmds_k_task_list(bmds_task_info_t *out, uint32_t max) {
        KernelCall call;
        size_t     stride = 0;
        int32_t    r = strideOf(out, max, stride);
        if (r < 0) return r;
        // The kernel's own copies (in PSRAM), then the owners matched to them.
        const size_t room = max < 256 ? max : 256;
        auto        *tasks = static_cast<bmds::TaskList::Info *>(
                room > 0 ? bmds::Memory::alloc(bmds::MemSpace::Psram, room * sizeof(bmds::TaskList::Info)) : nullptr);
        auto *owners = static_cast<Loader::TaskOwnership *>(
                room > 0 ? bmds::Memory::alloc(bmds::MemSpace::Psram, room * sizeof(Loader::TaskOwnership)) : nullptr);
        if (room > 0 && (tasks == nullptr || owners == nullptr)) {
                bmds::Memory::free(tasks);
                bmds::Memory::free(owners);
                return -BMDS_ENOMEM;
        }
        const size_t total = bmds::TaskList::list(Span<bmds::TaskList::Info>(tasks, room));
        const size_t n = total < room ? total : room;
        for (size_t i = 0; i < n; ++i) owners[i].task = tasks[i].task;
        Loader::findTaskOwners(Span<Loader::TaskOwnership>(owners, n));
        for (size_t i = 0; i < n; ++i) {
                const bmds::TaskList::Info &t = tasks[i];
                bmds_task_info_t            e = {};
                e.pid = owners[i].pid;
                e.tid = owners[i].tid;
                e.state = uint32_t(t.state);
                e.priority = t.priority;
                e.base_priority = t.basePriority;
                e.core = t.core;
                e.stack_free = t.stackFree;
                e.cpu_us = t.cpuUs;
                e.number = t.number;
                e.flags = owners[i].main ? BMDS_TASK_MAIN : 0u;
                copyText(bmds::StringView(t.name), e.name, sizeof(e.name));
                putEntry(e, out, stride, i);
        }
        bmds::Memory::free(tasks);
        bmds::Memory::free(owners);
        return int32_t(total);
}

int32_t bmds_k_proc_info(int32_t pid, uint32_t flags, bmds_proc_info_t *info) {
        KernelCall       call;
        bmds_proc_info_t p;
        int32_t          r = copyIn(info, p);
        if (r < 0) return r;
        if ((flags & ~BMDS_PROC_INFO_NEXT) != 0) return -BMDS_EINVAL;
        Loader::ProcessInfo k;
        if (Loader::processInfo(pid, (flags & BMDS_PROC_INFO_NEXT) != 0, k).isError()) return -BMDS_ESRCH;
        p = bmds_proc_info_t{};
        p.pid = k.pid;
        p.parent = k.parent;
        p.group = k.group;
        p.session = k.session;
        p.state = procState(k.state);
        p.threads = k.threads;
        p.status = k.status;
        p.memory = k.memory;
        p.cpu_us = k.cpuUs;
        p.start_ns = k.startedUs * 1000;
        std::memcpy(p.command, k.command, sizeof(p.command));
        p.command[sizeof(p.command) - 1] = '\0';
        r = copyOut(p, info);
        return r < 0 ? r : k.pid;
}

} // extern "C"
