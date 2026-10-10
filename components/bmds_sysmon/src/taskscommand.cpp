/**
 * @file      taskscommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `tasks` command (devplan/sysmon.md §3.5, step 6): the tasks from the kernel's task list,
 * which leaves interrupts on (uxTaskGetSystemState() masks them for about 3 ms). Its output format
 * is a userspace interface: keep the columns and their order, and add new ones only at the end.
 */

#include <cinttypes>
#include <cstdio>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/shell.h>

#include "kernelsource.h"

BMDS_NAMESPACE_BEGIN

namespace {

        const char *stateName(uint8_t state) {
                switch (state) {
                        case 0: return "running";
                        case 1: return "ready";
                        case 2: return "blocked";
                        case 3: return "suspended";
                        case 4: return "deleted";
                        default: return "unknown";
                }
        }

        int runTasks(CommandContext &ctx) {
                if (ctx.args().size() != 1) return ctx.usage();
                KernelSource source;
                // Room for a few tasks made between counting and listing.
                size_t capacity = source.tasks(Span<Source::Task>()) + 4;
                auto  *tasks =
                        static_cast<Source::Task *>(Memory::alloc(MemSpace::Psram, capacity * sizeof(Source::Task)));
                if (tasks == nullptr) {
                        ctx.eprint("tasks: out of memory\n");
                        return Shell::ExitFailure;
                }
                const size_t total = source.tasks(Span<Source::Task>(tasks, capacity));
                const size_t count = total < capacity ? total : capacity;
                const bool   counted = source.status().cpuCounted;

                // Sorted by name, so the listing is stable from run to run.
                for (size_t i = 1; i < count; ++i)
                        for (size_t j = i; j > 0 && StringView(tasks[j].name) < StringView(tasks[j - 1].name); --j) {
                                const Source::Task t = tasks[j];
                                tasks[j] = tasks[j - 1];
                                tasks[j - 1] = t;
                        }

                ctx.printf("%-16s %-9s %4s %4s %10s %10s\n", "name", "state", "prio", "core", "stack_free", "cpu_ms");
                for (size_t i = 0; i < count; ++i) {
                        const Source::Task &t = tasks[i];
                        char                core[12] = "-";
                        if (t.core >= 0) std::snprintf(core, sizeof(core), "%d", int(t.core));
                        char cpu[24] = "-";
                        if (counted) std::snprintf(cpu, sizeof(cpu), "%" PRIu64, t.cpuUs / 1000);
                        ctx.printf("%-16s %-9s %4u %4s %10u %10s\n", t.name, stateName(t.state), unsigned(t.priority),
                                   core, unsigned(t.stackFree), cpu);
                }
                Memory::free(tasks);
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(tasks, runTasks, "", "List tasks: state, priority, core, free stack (bytes), CPU time (ms)")

BMDS_NAMESPACE_END
