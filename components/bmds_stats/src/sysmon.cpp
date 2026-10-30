/**
 * @file      sysmon.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system-wide counters and the monitor's settings (devplan/sysmon.md §3.2, §6).
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/clock.h>
#include <bmds/cputime.h>
#include <bmds/kept.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/sysmon.h>
#include <bmds/system.h>
#include "keptcounters.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Stat::Health;
        using Stat::Kind;
        using Stat::Rule;
        using Stat::Unit;

        // The keys are an interface once shipped.
        BMDS_SYSCONF(sysmonInterval, "sysmon/interval", Config::Type::Int, "1000",
                     "The system monitor's sampling interval in ms, while anything watches", 100, 60000, nullptr,
                     nullptr)
        BMDS_SYSCONF(sysmonKeepInterval, "sysmon/keep-interval", Config::Type::Int, "10",
                     "Seconds between saves of what survives a reset (the boots' record, the kept counters, the "
                     "window's history; devplan/keptstate.md); 0: only at a clean restart",
                     0, 3600, nullptr, nullptr)
        BMDS_SYSCONF(sysmonLimits, "sysmon/limits", Config::Type::String, "",
                     "Overrides of the counters' rules: NAME:SETTING[,SETTING];... (devplan/sysmon.md 3.3)", 0, 0,
                     nullptr, nullptr)

        // ---- cpu.coreN -------------------------------------------------------------------------

        constexpr Stat::Desc CoreDescs[] = {
                {"busy_us",
                 Unit::Micros,
                 Kind::Total,
                 Stat::KeepHistory,
                 {Rule::Above, Health::Warning, 5, 900000, 980000},
                 "",
                 "Time the core ran anything but its idle task (a rate of 1,000,000 a second is all of it)"},
        };

        void readCore(void *arg, Span<int64_t> values) {
                const int64_t busy = CpuTime::busyUs(int(reinterpret_cast<intptr_t>(arg)));
                values[0] = busy < 0 ? Stat::Unavailable : busy;
                return;
        }

        // ---- mem.SPACE -------------------------------------------------------------------------

        enum MemIndex : size_t {
                MemTotal,
                MemFree,
                MemMinFree,
                MemLargest,
                MemUsed
        };

        constexpr Stat::Desc MemDescs[] = {
                {"total", Unit::Bytes, Kind::Info, 0, {}, "", "The heap's size"},
                {"free",
                 Unit::Bytes,
                 Kind::Level,
                 Stat::Adjusted,
                 {Rule::Below, Health::Warning, 5},
                 "",
                 "Free now (judged against a tenth and 3 % of the total)"},
                {"min_free", Unit::Bytes, Kind::Level, 0, {}, "", "The least free since boot"},
                {"largest",
                 Unit::Bytes,
                 Kind::Level,
                 Stat::Adjusted,
                 {Rule::Below, Health::Warning, 5},
                 "",
                 "The largest block that could be allocated now"},
                {"used", Unit::Bytes, Kind::Level, Stat::KeepHistory, {}, "", "In use now"},
        };

        // The largest block below which a space warns, and is critical (0: not judged).
        struct LargestLevels {
                        size_t warn;
                        size_t crit;
        };

        constexpr LargestLevels largestLevels(MemSpace space) {
                switch (space) {
                        case MemSpace::Internal: return {16 * 1024, 4 * 1024};
                        case MemSpace::InternalDma: return {8 * 1024, 2 * 1024};
                        case MemSpace::Psram:
                        case MemSpace::PsramDma: return {512 * 1024, 128 * 1024};
                        case MemSpace::Fast: return {0, 0};
                }
                return {0, 0};
        }

        constexpr const char *memGroupName(MemSpace space) {
                switch (space) {
                        case MemSpace::Internal: return "mem.internal";
                        case MemSpace::InternalDma: return "mem.internal_dma";
                        case MemSpace::Psram: return "mem.psram";
                        case MemSpace::PsramDma: return "mem.psram_dma";
                        case MemSpace::Fast: return "mem.fast";
                }
                return "mem.unknown";
        }

        MemSpace spaceOf(void *arg) {
                return MemSpace(uint8_t(reinterpret_cast<uintptr_t>(arg)));
        }

        void readMem(void *arg, Span<int64_t> values) {
                const MemStats s = Memory::stats(spaceOf(arg));
                values[MemTotal] = int64_t(s.totalBytes);
                values[MemFree] = int64_t(s.freeBytes);
                values[MemMinFree] = int64_t(s.minFreeBytes);
                values[MemLargest] = int64_t(s.largestFreeBlock);
                values[MemUsed] = int64_t(s.totalBytes - s.freeBytes);
                return;
        }

        void adjustMem(void *arg, size_t index, Stat::Limits &limits) {
                const MemSpace space = spaceOf(arg);
                if (index == MemFree) {
                        const size_t total = Memory::stats(space).totalBytes;
                        limits.warn = total == 0 ? Stat::NoLevel : int64_t(total / 10);
                        limits.crit = total == 0 ? Stat::NoLevel : int64_t(total * 3 / 100);
                } else if (index == MemLargest) {
                        const LargestLevels l = largestLevels(space);
                        limits.warn = l.warn == 0 ? Stat::NoLevel : int64_t(l.warn);
                        limits.crit = l.crit == 0 ? Stat::NoLevel : int64_t(l.crit);
                }
                return;
        }

        // The PSRAM window (devplan/psramwindow.md §3g): its pages, Memory's blocks, the policy's choices.
        constexpr Stat::Desc WindowDescs[] = {
                {"pages", Unit::Count, Kind::Info, 0, {}, "", "Its 64 KB pages (0: off)"},
                {"used", Unit::Count, Kind::Level, Stat::KeepHistory, {}, "", "Pages in use (large blocks, programs)"},
                {"longest", Unit::Count, Kind::Level, 0, {}, "", "The longest range of free pages"},
                {"blocks", Unit::Count, Kind::Level, 0, {}, "", "Large blocks mapped now"},
                {"runs", Unit::Count, Kind::Level, 0, {}, "", "The heap runs behind them"},
                {"bytes", Unit::Bytes, Kind::Level, Stat::KeepHistory, {}, "", "What they hold"},
                {"by_heap", Unit::Count, Kind::Total, 0, {}, "", "Large requests the policy sent to the heap"},
                {"by_size", Unit::Count, Kind::Total, 0, {}, "", "Sent to the window: large enough always"},
                {"by_fit",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Sent to the window: the heap's largest block too small"},
                {"by_cut_up", Unit::Count, Kind::Total, 0, {}, "", "Sent to the window: the heap cut up"},
                {"by_reserve", Unit::Count, Kind::Total, 0, {}, "", "Sent to the window: the heap's reserve kept"},
                {"to_window", Unit::Count, Kind::Total, 0, {}, "", "Heap failures the window served"},
                {"to_heap", Unit::Count, Kind::Total, 0, {}, "", "Window failures the heap served"},
                {"grown_in_place", Unit::Count, Kind::Total, 0, {}, "", "Reallocations grown without a copy"},
        };

        void readWindow(void *, Span<int64_t> values) {
                const Memory::WindowUsage u = Memory::windowUsage();
                const int64_t             v[] = {u.pages,          u.used,     u.longest, u.blocks,      u.runs,
                                                 int64_t(u.bytes), u.byHeap,   u.bySize,  u.byFit,       u.byCutUp,
                                                 u.byReserve,      u.toWindow, u.toHeap,  u.grownInPlace};
                static_assert(sizeof(v) / sizeof(v[0]) == sizeof(WindowDescs) / sizeof(WindowDescs[0]));
                for (size_t i = 0; i < values.size() && i < sizeof(v) / sizeof(v[0]); ++i) values[i] = v[i];
                return;
        }

        // ---- system, clock ---------------------------------------------------------------------

        constexpr Stat::Desc SystemDescs[] = {
                {"uptime_us", Unit::Micros, Kind::Level, 0, {}, "", "Time since boot"},
                {"tasks", Unit::Count, Kind::Level, Stat::KeepHistory, {}, "", "Tasks (FreeRTOS's, every kind)"},
                {"cpu_counted", Unit::Count, Kind::State, 0, {}, "no|yes", "Whether CPU time is counted this boot"},
        };

        void readSystem(void *, Span<int64_t> values) {
                values[0] = int64_t(System::uptimeUs());
                values[1] = int64_t(uxTaskGetNumberOfTasks());
                values[2] = CpuTime::isEnabled() ? 1 : 0;
                return;
        }

        constexpr Stat::Desc ClockDescs[] = {
                {"source",
                 Unit::Count,
                 Kind::State,
                 0,
                 {},
                 "none|manual|rtc|ntp|ptp|gps|host",
                 "Where the wall time came from"},
                {"slew_ns", Unit::Nanos, Kind::Level, 0, {}, "", "Offset still being slewed in"},
                {"frequency_ppb", Unit::Ppb, Kind::Level, 0, {}, "", "The clock's frequency correction"},
        };

        void readClock(void *, Span<int64_t> values) {
                const ClockStatus s = Clock::status();
                values[0] = int64_t(s.source);
                values[1] = s.slewRemainingNs;
                values[2] = s.frequencyPpb;
                return;
        }

        constexpr size_t countOf(const auto &array) {
                return sizeof(array) / sizeof(array[0]);
        }

        // ---- Settings --------------------------------------------------------------------------

        void applySettings() {
                if (auto [ms, err] = SysConf::getInt(bmdsConf_sysmonInterval.key); err.isOk())
                        Stats::setInterval(uint32_t(ms));
                if (auto [seconds, err] = SysConf::getInt(bmdsConf_sysmonKeepInterval.key); err.isOk())
                        Kept::setInterval(uint32_t(seconds) * 1000);
                String limits(MemSpace::Psram);
                if (SysConf::get(bmdsConf_sysmonLimits.key, limits).isError()) return;
                size_t bad = 0;
                if (Stats::setOverrides(StringView(limits.data(), limits.size()), &bad).isOk() && bad != 0)
                        bmdsWarn("sysmon/limits: %u entr%s not understood, skipped", unsigned(bad),
                                 bad == 1 ? "y" : "ies");
                return;
        }

        void settingsChanged(void *) {
                applySettings();
                return;
        }

        struct Counters {
                        Stats::Group cores[portNUM_PROCESSORS] = {
                                {CoreDescs, countOf(CoreDescs), readCore, reinterpret_cast<void *>(intptr_t(0))},
#if portNUM_PROCESSORS > 1
                                {CoreDescs, countOf(CoreDescs), readCore, reinterpret_cast<void *>(intptr_t(1))},
#endif
                        };
                        Stats::Group mem[MemSpaceCount] = {
                                {MemDescs, countOf(MemDescs), readMem, reinterpret_cast<void *>(uintptr_t(0)),
                                 adjustMem},
                                {MemDescs, countOf(MemDescs), readMem, reinterpret_cast<void *>(uintptr_t(1)),
                                 adjustMem},
                                {MemDescs, countOf(MemDescs), readMem, reinterpret_cast<void *>(uintptr_t(2)),
                                 adjustMem},
                                {MemDescs, countOf(MemDescs), readMem, reinterpret_cast<void *>(uintptr_t(3)),
                                 adjustMem},
                                {MemDescs, countOf(MemDescs), readMem, reinterpret_cast<void *>(uintptr_t(4)),
                                 adjustMem},
                        };
                        Stats::Group     window{WindowDescs, countOf(WindowDescs), readWindow, nullptr};
                        Stats::Group     system{SystemDescs, countOf(SystemDescs), readSystem, nullptr};
                        Stats::Group     clock{ClockDescs, countOf(ClockDescs), readClock, nullptr};
                        SysConf::Watcher settings;
        };

        constinit StaticObject<Counters> counters;

} // namespace

Error Sysmon::init() {
        Error err = CpuTime::init();
        if (err.isError()) return err;
        counters.create();
        Counters &c = *counters;
        for (int core = 0; core < portNUM_PROCESSORS; ++core) {
                c.cores[core].setName("cpu.core", core);
                if ((err = Stats::add(c.cores[core])).isError()) return err;
        }
        for (size_t i = 0; i < MemSpaceCount; ++i) {
                c.mem[i].setName(memGroupName(MemSpace(i)));
                if ((err = Stats::add(c.mem[i])).isError()) return err;
        }
        c.window.setName("mem.window");
        if ((err = Stats::add(c.window)).isError()) return err;
        c.system.setName("system");
        c.clock.setName("clock");
        if ((err = Stats::add(c.system)).isError()) return err;
        if ((err = Stats::add(c.clock)).isError()) return err;
        if ((err = KeptCounters::init()).isError()) return err;
        applySettings();
        return c.settings.start("sysmon", settingsChanged, nullptr);
}

BMDS_NAMESPACE_END
