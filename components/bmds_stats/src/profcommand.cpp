/**
 * @file      profcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `prof` command (devplan/sysmon.md §9). Its dump's format is a userspace interface (read by
 * tools/bmds-prof): keep the fields and their order, and add new ones only at the end.
 */

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <bmds/command.h>
#include <bmds/profiler.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // The samples by task, busiest first (IDLE and the interrupts' last), as shares.
        void summarize(CommandContext &ctx) {
                constexpr size_t MaxTasks = 32;
                struct Count {
                                char   task[Profiler::NameBytes];
                                size_t samples;
                };
                Count        counts[MaxTasks] = {};
                size_t       used = 0, other = 0, idle = 0;
                const size_t n = Profiler::sampleCount();
                for (size_t i = 0; i < n; ++i) {
                        const Profiler::Sample &s = Profiler::sample(i);
                        if (std::strncmp(s.task, "IDLE", 4) == 0) {
                                ++idle;
                                continue;
                        }
                        size_t k = 0;
                        while (k < used && std::strncmp(counts[k].task, s.task, sizeof(s.task)) != 0) ++k;
                        if (k == used) {
                                if (used == MaxTasks) {
                                        ++other;
                                        continue;
                                }
                                std::memcpy(counts[used++].task, s.task, sizeof(s.task));
                        }
                        ++counts[k].samples;
                }
                ctx.printf("%u samples, %u busy (%u%%)\n", unsigned(n), unsigned(n - idle),
                           n == 0 ? 0u : unsigned((n - idle) * 100 / n));
                for (size_t a = 0; a < used; ++a) {
                        size_t best = a;
                        for (size_t b = a + 1; b < used; ++b)
                                if (counts[b].samples > counts[best].samples) best = b;
                        const Count c = counts[best];
                        counts[best] = counts[a];
                        counts[a] = c;
                        ctx.printf("%7u %5.1f%%  %.*s\n", unsigned(c.samples), n == 0 ? 0.0 : c.samples * 100.0 / n,
                                   int(Profiler::NameBytes), c.task);
                }
                if (other > 0) ctx.printf("%7u %5.1f%%  (other tasks)\n", unsigned(other), other * 100.0 / n);
                return;
        }

        int runProf(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() < 2) return ctx.usage();
                const StringView what(args[1]);
                if (what == "start") {
                        uint32_t mask = 0;
                        if (args.size() == 4 && StringView(args[2]) == "-c") {
                                const char c = args[3][0];
                                if (c < '0' || c > '9' || args[3][1] != '\0') return ctx.usage();
                                mask = 1u << (c - '0');
                        } else if (args.size() == 2) {
                                mask = (1u << portNUM_PROCESSORS) - 1;
                        } else {
                                return ctx.usage();
                        }
                        const Error err = Profiler::start(mask);
                        if (err.isError()) {
                                ctx.eprintf("prof: %s\n", err.desc());
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                if (args.size() != 2) return ctx.usage();
                if (what == "stop") {
                        Profiler::stop();
                        summarize(ctx);
                        return Shell::ExitSuccess;
                }
                if (what == "dump") {
                        if (Profiler::isRunning()) {
                                ctx.eprintf("prof: still sampling (prof stop first)\n");
                                return Shell::ExitFailure;
                        }
                        for (size_t i = 0; i < Profiler::moduleCount(); ++i) {
                                const Profiler::Module &m = Profiler::module(i);
                                ctx.printf("# module %u %08lx %lu %s\n", unsigned(i),
                                           static_cast<unsigned long>(m.base), static_cast<unsigned long>(m.bytes),
                                           m.name);
                        }
                        const size_t n = Profiler::sampleCount();
                        for (size_t i = 0; i < n && !ctx.isInterrupted(); ++i) {
                                const Profiler::Sample &s = Profiler::sample(i);
                                ctx.printf("%u %.*s %08lx %08lx %u\n", unsigned(s.core), int(Profiler::NameBytes),
                                           s.task, static_cast<unsigned long>(s.pc), static_cast<unsigned long>(s.ra),
                                           unsigned(s.module));
                        }
                        return ctx.isInterrupted() ? Shell::ExitInterrupted : Shell::ExitSuccess;
                }
                if (what == "clear") {
                        Profiler::clear();
                        return Shell::ExitSuccess;
                }
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(prof, runProf, "start [-c CORE] | stop | dump | clear",
                   "Sample where the CPU goes, each core at every tick (stop: by task; dump: for "
                   "tools/bmds-prof; clear: frees the samples)")

BMDS_NAMESPACE_END
