/**
 * @file      stackprobe.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The measurement build's stack probe (`CONFIG_BMDS_STACK_PROBE`): how deep
 * each system call goes on the calling program's stack, for the floor under
 * program stacks (devplan/programs.md §21). Each `KernelCall` paints the
 * stack below the slot's entry SP and, as it returns, finds the lowest word
 * it changed; interrupts taken during the call push their frames there too,
 * and count. `stackprobe` lists each slot's deepest call. Nothing of this is
 * built in a normal build.
 */

#include <sdkconfig.h>

#if CONFIG_BMDS_STACK_PROBE

#include "running.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <bmds/command.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Paint = 0xa5a5a5a5u;
        constexpr size_t   ReachBytes = 16 * 1024; // painted below the slot's entry; deeper reads as "16384+"
        constexpr size_t   MarginBytes = 64;       // left alone just below the painter's own SP

        struct Entry {
                        const char *slot;    // __builtin_FUNCTION(): one string per slot
                        uint32_t    deepest; // bytes below the slot's entry SP
                        uint32_t    calls;
                        bool        reached; // went as deep as the paint: deeper than measured
        };

        constexpr size_t   Capacity = 320;
        constinit Entry    entries[Capacity] = {};
        constinit size_t   used = 0;
        constinit uint32_t dropped = 0; // calls of slots past Capacity
        portMUX_TYPE       lock = portMUX_INITIALIZER_UNLOCKED;

        // The painted range's low end below @p top (word aligned): ReachBytes, or the stack's bottom.
        uintptr_t lowOf(const ProgramThread &thread, uintptr_t top) {
                const uintptr_t bottom = thread.task.stackBottom();
                return (std::max(bottom, top > ReachBytes ? top - ReachBytes : 0) + 3u) & ~uintptr_t(3);
        }

} // namespace

void StackProbe::enter(ProgramThread &thread, uintptr_t top) {
        const uintptr_t low = lowOf(thread, top);
        const uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        const uintptr_t high = (sp - MarginBytes) & ~uintptr_t(3);
        for (uintptr_t a = low; a < high; a += 4) *reinterpret_cast<volatile uint32_t *>(a) = Paint;
        return;
}

void StackProbe::leave(ProgramThread &thread, uintptr_t top, const char *slot) {
        const uintptr_t low = lowOf(thread, top);
        uintptr_t       a = low;
        while (a < top && *reinterpret_cast<volatile uint32_t *>(a) == Paint) a += 4;
        const uint32_t depth = static_cast<uint32_t>(top - a);
        const bool     reached = a == low && low > thread.task.stackBottom();
        portENTER_CRITICAL(&lock);
        Entry *e = nullptr;
        for (size_t i = 0; i < used && e == nullptr; ++i) {
                if (entries[i].slot == slot) e = &entries[i];
        }
        if (e == nullptr && used < Capacity) {
                e = &entries[used++];
                e->slot = slot;
        }
        if (e != nullptr) {
                ++e->calls;
                if (depth > e->deepest) e->deepest = depth;
                e->reached = e->reached || reached;
        } else {
                ++dropped;
        }
        portEXIT_CRITICAL(&lock);
        return;
}

namespace {

        int runStackProbe(CommandContext &ctx) {
                static Entry copy[Capacity];
                portENTER_CRITICAL(&lock);
                const size_t n = used;
                std::memcpy(copy, entries, n * sizeof(Entry));
                const uint32_t lost = dropped;
                portEXIT_CRITICAL(&lock);
                std::sort(copy, copy + n, [](const Entry &a, const Entry &b) { return a.deepest > b.deepest; });
                ctx.printf("deepest  calls  slot (bytes below the slot's entry SP)\n");
                for (size_t i = 0; i < n; ++i) {
                        ctx.printf("%6lu%s %6lu  %s\n", static_cast<unsigned long>(copy[i].deepest),
                                   copy[i].reached ? "+" : " ", static_cast<unsigned long>(copy[i].calls),
                                   copy[i].slot);
                }
                if (lost != 0)
                        ctx.printf("(%lu calls of slots past the table not counted)\n",
                                   static_cast<unsigned long>(lost));
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(stackprobe, runStackProbe, "",
                   "The deepest stack each system call used (a CONFIG_BMDS_STACK_PROBE build)")

BMDS_NAMESPACE_END

#endif // CONFIG_BMDS_STACK_PROBE
