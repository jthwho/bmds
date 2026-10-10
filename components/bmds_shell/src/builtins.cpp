/**
 * @file      builtins.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Built-in shell commands. Output formats are userspace interfaces: keep
 * column order and units, and add new fields only at the end.
 */

#include <cstdio>
#include <esp_heap_caps.h>
#include <bmds/console.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // How long `reboot` waits for queued output to reach the terminal.
        constexpr unsigned int RebootFlushMs = 500;

        // `log show` prints in pieces, each within the shell's write timeout.
        constexpr size_t LogShowChunkBytes = 256;

        int runVersion(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                const SystemInfo info = System::info();
                ctx.printf("BMDS %s (build %s, ESP-IDF %s)\n", info.bmdsVersion, info.buildVersion, info.idfVersion);
                ctx.printf("Board: %s\n", info.boardName);
                ctx.printf("Chip: ESP32-P4 rev v%u.%u, %u cores @ %u MHz\n", info.chipRevMajor, info.chipRevMinor,
                           info.cores, info.cpuMhz);
                return Shell::ExitSuccess;
        }

        int runUptime(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                const uint64_t ms = System::uptimeUs() / 1000;
                const uint64_t s = ms / 1000;
                ctx.printf("up %lud %02u:%02u:%02u.%03u\n", static_cast<unsigned long>(s / 86400),
                           static_cast<unsigned>(s / 3600 % 24), static_cast<unsigned>(s / 60 % 60),
                           static_cast<unsigned>(s % 60), static_cast<unsigned>(ms % 1000));
                return Shell::ExitSuccess;
        }

        // A size for people (mem -h): powers of 1024 with two decimals
        // (16.55M, 432.43k); under 1024, the bytes as they are.
        void humanBytes(uint64_t bytes, char (&out)[24]) {
                static constexpr char Units[] = "kMGT";
                if (bytes < 1024) {
                        std::snprintf(out, sizeof(out), "%llu", static_cast<unsigned long long>(bytes));
                        return;
                }
                uint64_t unit = 1024;
                size_t   u = 0;
                // Rounding may reach the next unit (1023.999k is 1.00M).
                while (u + 1 < sizeof(Units) - 1 && (bytes * 100 + unit / 2) / unit >= 1024 * 100) {
                        unit *= 1024;
                        ++u;
                }
                const uint64_t hundredths = (bytes * 100 + unit / 2) / unit;
                std::snprintf(out, sizeof(out), "%llu.%02llu%c", static_cast<unsigned long long>(hundredths / 100),
                              static_cast<unsigned long long>(hundredths % 100), Units[u]);
                return;
        }

        // @p bytes in mem's columns: as they are, or for people.
        void memCell(uint64_t bytes, bool human, char (&out)[24]) {
                if (human) {
                        humanBytes(bytes, out);
                } else {
                        std::snprintf(out, sizeof(out), "%llu", static_cast<unsigned long long>(bytes));
                }
                return;
        }

        // ---- mem mark / mem new: the heap blocks in use now that weren't at the mark (a leak's
        // hunt). The walk holds the heap's lock: its callbacks only fill arrays made before.

        struct HeapBlock {
                        const void *ptr;
                        uint32_t    size;
                        bool        psram;
        };

        constexpr size_t MaxMarked = 8192; // blocks in use at the mark
        constexpr size_t MaxNew = 64;      // new blocks listed

        constinit HeapBlock *marked = nullptr; // sorted by address
        constinit size_t     markedCount = 0;

        struct Walk {
                        HeapBlock  *out;
                        size_t      capacity;
                        size_t      count;
                        size_t      overflow;
                        const void *skip; // the arrays' own blocks
                        const void *skip2;
                        bool        psram;
                        bool        onlyNew; // only blocks not marked
        };

        bool isMarked(const void *ptr, uint32_t size) {
                size_t lo = 0, hi = markedCount;
                while (lo < hi) {
                        const size_t mid = (lo + hi) / 2;
                        if (marked[mid].ptr < ptr) {
                                lo = mid + 1;
                        } else {
                                hi = mid;
                        }
                }
                return lo < markedCount && marked[lo].ptr == ptr && marked[lo].size == size;
        }

        bool walkBlock(walker_heap_into_t, walker_block_info_t block, void *arg) {
                auto *w = static_cast<Walk *>(arg);
                if (!block.used || block.ptr == w->skip || block.ptr == w->skip2) return true;
                if (w->onlyNew && isMarked(block.ptr, uint32_t(block.size))) return true;
                if (w->count == w->capacity) {
                        ++w->overflow;
                        return true;
                }
                w->out[w->count++] = HeapBlock{block.ptr, uint32_t(block.size), w->psram};
                return true;
        }

        void walkHeaps(Walk &w) {
                w.psram = true;
                heap_caps_walk(MALLOC_CAP_SPIRAM, walkBlock, &w);
                w.psram = false;
                heap_caps_walk(MALLOC_CAP_INTERNAL, walkBlock, &w);
                return;
        }

        int runMemMark(CommandContext &ctx) {
                Memory::free(marked);
                marked = nullptr;
                markedCount = 0;
                auto *blocks = static_cast<HeapBlock *>(Memory::alloc(MemSpace::Psram, MaxMarked * sizeof(HeapBlock)));
                if (blocks == nullptr) {
                        ctx.eprint("mem: out of memory\n");
                        return Shell::ExitFailure;
                }
                Walk w{blocks, MaxMarked, 0, 0, blocks, nullptr, false, false};
                walkHeaps(w);
                // In address order, for the lookups (the heaps are walked in order, but two of them).
                for (size_t i = 1; i < w.count; ++i) {
                        const HeapBlock b = blocks[i];
                        size_t          k = i;
                        for (; k > 0 && blocks[k - 1].ptr > b.ptr; --k) blocks[k] = blocks[k - 1];
                        blocks[k] = b;
                }
                marked = blocks;
                markedCount = w.count;
                ctx.printf("mem: %u blocks in use marked%s\n", static_cast<unsigned>(w.count),
                           w.overflow != 0 ? " (more not: the rest will show as new)" : "");
                return Shell::ExitSuccess;
        }

        int runMemNew(CommandContext &ctx) {
                if (marked == nullptr) {
                        ctx.eprint("mem: no mark ('mem mark' first)\n");
                        return Shell::ExitFailure;
                }
                HeapBlock fresh[MaxNew];
                Walk      w{fresh, MaxNew, 0, 0, marked, nullptr, false, true};
                walkHeaps(w);
                uint32_t total = 0;
                for (size_t i = 0; i < w.count; ++i) {
                        const HeapBlock &b = fresh[i];
                        total += b.size;
                        // Its first bytes: a name, a vtable, a pattern tell what it is.
                        const auto  *p = static_cast<const uint8_t *>(b.ptr);
                        char         hex[16 * 3 + 1] = {};
                        char         text[17] = {};
                        const size_t n = b.size < 16 ? b.size : 16;
                        for (size_t k = 0; k < n; ++k) {
                                std::snprintf(hex + 3 * k, 4, "%02x ", p[k]);
                                text[k] = p[k] >= 0x20 && p[k] < 0x7f ? char(p[k]) : '.';
                        }
                        ctx.printf("%p %8u %-8s %-48s %s\n", b.ptr, static_cast<unsigned>(b.size),
                                   b.psram ? "psram" : "internal", hex, text);
                }
                ctx.printf("mem: %u new blocks, %u bytes%s\n", static_cast<unsigned>(w.count + w.overflow),
                           static_cast<unsigned>(total), w.overflow != 0 ? " (the rest not listed)" : "");
                return Shell::ExitSuccess;
        }

        int runMem(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 2 && StringView(args[1]) == "mark") return runMemMark(ctx);
                if (args.size() == 2 && StringView(args[1]) == "new") return runMemNew(ctx);
                bool human = false;
                for (size_t i = 1; i < args.size(); ++i) {
                        if (StringView(args[i]) != "-h") return ctx.usage();
                        human = true;
                }
                ctx.printf("%-11s %10s %10s %10s %10s\n", "space", "total", "free", "min_free", "largest");
                char cells[4][24];
                for (size_t i = 0; i < MemSpaceCount; ++i) {
                        const MemSpace space = static_cast<MemSpace>(i);
                        const MemStats s = Memory::stats(space);
                        memCell(s.totalBytes, human, cells[0]);
                        memCell(s.freeBytes, human, cells[1]);
                        memCell(s.minFreeBytes, human, cells[2]);
                        memCell(s.largestFreeBlock, human, cells[3]);
                        ctx.printf("%-11s %10s %10s %10s %10s\n", memSpaceName(space), cells[0], cells[1], cells[2],
                                   cells[3]);
                }
                // What the caches hold (the loader's programs, the synth's fonts): counted as used
                // above, given back when an allocation would fail. A line each.
                Memory::Reclaimer::Usage usage[Memory::MaxReclaimers];
                const size_t             caches =
                        Memory::reclaimerUsage(Span<Memory::Reclaimer::Usage>(usage, Memory::MaxReclaimers));
                for (size_t i = 0; i < caches && i < Memory::MaxReclaimers; ++i) {
                        const Memory::Reclaimer::Usage &u = usage[i];
                        memCell(u.bytes, human, cells[0]);
                        ctx.printf("%-11s %10s  %s %u, hits %u, misses %u, given back %u\n", "cached", cells[0],
                                   u.items, static_cast<unsigned>(u.count), static_cast<unsigned>(u.hits),
                                   static_cast<unsigned>(u.misses), static_cast<unsigned>(u.reclaims));
                }
                return Shell::ExitSuccess;
        }

        const char *levelName(LogLevel level) {
                switch (level) {
                        case LogLevel::Debug: return "debug";
                        case LogLevel::Info: return "info";
                        case LogLevel::Warn: return "warn";
                        case LogLevel::Err: return "err";
                }
                return "?";
        }

        void printLogStatus(CommandContext &ctx) {
                ctx.printf("level: %s\n", levelName(Log::level()));
                const Span<const DebugModule> modules = Log::debugModules();
                if (modules.isEmpty()) return;
                ctx.printf("%-20s %s\n", "module", "debug");
                // Print in name order (the registry is in link order): pick the
                // smallest name greater than the last one printed.
                StringView last;
                for (size_t n = 0; n < modules.size(); ++n) {
                        const DebugModule *next = nullptr;
                        for (const DebugModule &m : modules) {
                                if ((n == 0 || StringView(m.name) > last) &&
                                    (next == nullptr || StringView(m.name) < StringView(next->name))) {
                                        next = &m;
                                }
                        }
                        if (next == nullptr) break;
                        ctx.printf("%-20s %s\n", next->name, next->isEnabled() ? "on" : "off");
                        last = next->name;
                }
                return;
        }

        int showLog(CommandContext &ctx, Args args) {
                if (args.size() != 2) return ctx.usage();
                const size_t capacity = Log::historyCapacity();
                if (capacity == 0) {
                        ctx.eprint("log: no history\n");
                        return Shell::ExitFailure;
                }
                char *buf = static_cast<char *>(Memory::alloc(MemSpace::Psram, capacity));
                if (buf == nullptr) {
                        ctx.eprint("log: out of memory\n");
                        return Shell::ExitFailure;
                }
                const size_t len = Log::copyHistory(Span<char>(buf, capacity));
                int          result = Shell::ExitSuccess;
                for (size_t done = 0; done < len; done += LogShowChunkBytes) {
                        if (ctx.isInterrupted()) {
                                result = Shell::ExitInterrupted;
                                break;
                        }
                        const size_t n = len - done < LogShowChunkBytes ? len - done : LogShowChunkBytes;
                        ctx.print(StringView(buf + done, n));
                }
                Memory::free(buf);
                return result;
        }

        int runLog(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) {
                        printLogStatus(ctx);
                        return Shell::ExitSuccess;
                }
                const StringView sub = args[1];
                if (sub == "show") return showLog(ctx, args);
                return ctx.usage();
        }

        // Parses "N" or "N.FFF" seconds into milliseconds (fraction to 1 ms).
        bool parseSeconds(StringView text, unsigned int &ms) {
                constexpr uint64_t MaxMs = 7 * 24 * 3600 * 1000ull;
                uint64_t           whole = 0;
                size_t             i = 0;
                for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                        whole = whole * 10 + static_cast<uint64_t>(text[i] - '0');
                        if (whole * 1000 > MaxMs) return false;
                }
                const bool hasWhole = i > 0;
                uint64_t   frac = 0;
                uint64_t   scale = 100;
                bool       hasFrac = false;
                if (i < text.size() && text[i] == '.') {
                        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                                frac += static_cast<uint64_t>(text[i] - '0') * scale;
                                scale /= 10;
                                hasFrac = true;
                        }
                }
                if (i != text.size() || (!hasWhole && !hasFrac)) return false;
                const uint64_t total = whole * 1000 + frac;
                if (total > MaxMs) return false;
                ms = static_cast<unsigned int>(total);
                return true;
        }

        int runSleep(CommandContext &ctx) {
                const Args   args = ctx.args();
                unsigned int ms = 0;
                if (args.size() != 2 || !parseSeconds(args[1], ms)) return ctx.usage();
                if (ms == 0) return Shell::ExitSuccess; // 0 would mean WaitForever
                return ctx.shell().interrupt().sleep(ms).isOk() ? Shell::ExitSuccess : Shell::ExitInterrupted;
        }

        int runReboot(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                ctx.print("Rebooting...\n");
                (void)Vfs::sync(); // the card's directories and allocation held for writing back
                (void)ctx.shell().console().flush(RebootFlushMs);
                System::restart();
        }

} // namespace

BMDS_SHELL_COMMAND(help, Shell::runHelp, "[command]", "List commands, or show one command's usage")
BMDS_SHELL_COMMAND(log, runLog, "[show]",
                   "Show the log level and debug modules (sysconf's log/level and log/debug set them); "
                   "show the log history")
BMDS_SHELL_COMMAND(mem, runMem, "[-h | mark | new]",
                   "Show memory usage per memory space, and what caches hold (bytes; -h: 16.55M, 432.43k); "
                   "'mark' records the heap blocks in use, 'new' lists those in use now that weren't then")
BMDS_SHELL_COMMAND(reboot, runReboot, "", "Restart the system")
BMDS_SHELL_COMMAND(sleep, runSleep, "<seconds>", "Wait for a number of seconds (fractions allowed, e.g. 0.5)")
BMDS_SHELL_COMMAND(uptime, runUptime, "", "Show time since boot")
BMDS_SHELL_COMMAND(version, runVersion, "", "Show BMDS, build, board and chip versions")

BMDS_NAMESPACE_END
