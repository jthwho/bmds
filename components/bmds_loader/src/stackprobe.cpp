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
 *
 * It also keeps how much of each U-mode thread's kernel stack (its FreeRTOS
 * stack: system calls, trap frames, interrupts taken in U-mode) was ever used,
 * read from FreeRTOS's fill as the thread is reaped, the deepest per program
 * (devplan/protection.md §21).
 */

#include <sdkconfig.h>

#if CONFIG_BMDS_STACK_PROBE

#include "running.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
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

        // A program's U-mode threads' kernel stacks: the most any of them used.
        struct KernelEntry {
                        char     program[24]; // its file's name, cut
                        uint32_t deepest;     // bytes ever used of `bytes`
                        uint32_t bytes;
                        uint32_t threads;
        };

        constexpr size_t      KernelCapacity = 64;
        constinit KernelEntry kernelEntries[KernelCapacity] = {};
        constinit size_t      kernelUsed = 0;
        constinit uint32_t    kernelDropped = 0;

        // Left alone above the stack's start: ESP-IDF's port carves a task's FPU and PIE register
        // save areas from the bottom of its stack, lazily (when another task takes the unit from
        // it), and moves the stack's start above them. Painting them made the task's next FPU
        // restore load fcsr 0xa5 (an invalid rounding mode): every FP instruction then trapped
        // and was retried forever (docs/platforms/esp32p4.md §5a). The areas are under 512 bytes
        // together, and one may be carved while this paints (the task preempted): a margin, not
        // just the start as it is now.
        constexpr uintptr_t CoprocMarginBytes = 1024;

        // Where the paint may go down to: above the stack's start and its coprocessor areas.
        uintptr_t floorOf(const ProgramThread &thread) {
                const auto start = reinterpret_cast<uintptr_t>(xTaskGetStackStart(thread.task.handle()));
                return std::max(start, thread.task.stackBottom()) + CoprocMarginBytes;
        }

        // The painted range's low end below @p top (word aligned): ReachBytes, or the floor.
        uintptr_t lowOf(const ProgramThread &thread, uintptr_t top) {
                return (std::max(floorOf(thread), top > ReachBytes ? top - ReachBytes : 0) + 3u) & ~uintptr_t(3);
        }

} // namespace

void StackProbe::enter(ProgramThread &thread, uintptr_t top) {
        const uintptr_t low = lowOf(thread, top);
        // What ran since the last call (a U-mode thread's interrupts, its trap entries) may have gone
        // deeper than this call's paint reaches back: note it before painting over it.
        uintptr_t seen = low;
        while (seen < top && *reinterpret_cast<volatile uint32_t *>(seen) == Paint) seen += 4;
        if (thread.stackDeepest == 0 || seen < thread.stackDeepest) thread.stackDeepest = static_cast<uint32_t>(seen);
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
        const bool     reached = a == low; // as deep as the paint: deeper than measured
        if (thread.stackDeepest == 0 || a < thread.stackDeepest) thread.stackDeepest = static_cast<uint32_t>(a);
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

void StackProbe::threadEnded(const ProgramThread &thread, size_t bytes) {
        // FreeRTOS filled the stack when it made the task; what it reports is the fill left
        // untouched below everything ever pushed.
        // Painting a call's reach refills what earlier ones used (the paint is FreeRTOS's fill
        // byte, 0xa5): their marks were noted as they went (stackDeepest).
        const uintptr_t bottom = thread.task.stackBottom();
        const size_t    free = uxTaskGetStackHighWaterMark(thread.task.handle()) * sizeof(StackType_t);
        uint32_t        used = static_cast<uint32_t>(bytes > free ? bytes - free : 0);
        if (thread.stackDeepest != 0 && thread.stackDeepest >= bottom)
                used = std::max<uint32_t>(used, static_cast<uint32_t>(bottom + bytes - thread.stackDeepest));
        const char *path = thread.run->path;
        const char *slash = std::strrchr(path, '/');
        const char *name = slash != nullptr ? slash + 1 : path;
        if (*name == '\0') name = "?";
        portENTER_CRITICAL(&lock);
        KernelEntry *e = nullptr;
        for (size_t i = 0; i < kernelUsed && e == nullptr; ++i) {
                if (std::strncmp(kernelEntries[i].program, name, sizeof(e->program) - 1) == 0) e = &kernelEntries[i];
        }
        if (e == nullptr && kernelUsed < KernelCapacity) {
                e = &kernelEntries[kernelUsed++];
                std::strncpy(e->program, name, sizeof(e->program) - 1);
                e->bytes = static_cast<uint32_t>(bytes);
        }
        if (e != nullptr) {
                ++e->threads;
                if (used > e->deepest) e->deepest = used;
        } else {
                ++kernelDropped;
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

                static KernelEntry kcopy[KernelCapacity];
                portENTER_CRITICAL(&lock);
                const size_t kn = kernelUsed;
                std::memcpy(kcopy, kernelEntries, kn * sizeof(KernelEntry));
                const uint32_t klost = kernelDropped;
                portEXIT_CRITICAL(&lock);
                std::sort(kcopy, kcopy + kn,
                          [](const KernelEntry &a, const KernelEntry &b) { return a.deepest > b.deepest; });
                ctx.printf("\nU-mode kernel stacks: deepest of  threads  program\n");
                for (size_t i = 0; i < kn; ++i) {
                        ctx.printf("%6lu %6lu  %7lu  %s\n", static_cast<unsigned long>(kcopy[i].deepest),
                                   static_cast<unsigned long>(kcopy[i].bytes),
                                   static_cast<unsigned long>(kcopy[i].threads), kcopy[i].program);
                }
                if (klost != 0)
                        ctx.printf("(%lu threads of programs past the table not counted)\n",
                                   static_cast<unsigned long>(klost));
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(stackprobe, runStackProbe, "",
                   "The deepest stack each system call used (a CONFIG_BMDS_STACK_PROBE build)")

BMDS_NAMESPACE_END

#endif // CONFIG_BMDS_STACK_PROBE
