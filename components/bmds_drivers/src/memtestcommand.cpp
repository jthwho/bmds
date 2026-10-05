/**
 * @file      memtestcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `memtest` command (devplan/psram.md): writes and verifies the free
 * PSRAM, through the CPU (with the caches written back and invalidated, so
 * the reads come from the PSRAM) and through the AXI GDMA.
 */

#include <cstdlib>
#include <cstring>
#include <esp_async_memcpy.h>
#include <esp_cache.h>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/memtest.h>
#include <bmds/semaphore.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr size_t   MaxRegions = 16;
        constexpr size_t   Align = 128;               // the L2 cache line, and the GDMA's PSRAM alignment
        constexpr size_t   Chunk = 64 * 1024;         // filled or checked between write-backs and Ctrl-C checks
        constexpr size_t   DmaChunk = 256 * 1024;     // one GDMA copy
        constexpr size_t   MinRegion = 1024 * 1024;   // smaller free blocks aren't worth it
        constexpr size_t   Reserve = 2 * 1024 * 1024; // left free for the rest of the system
        constexpr size_t   MaxReported = 8;
        constexpr uint64_t YieldUs = 500'000; // the idle task's turn (its watchdog)

        struct Region {
                        uint32_t *words = nullptr;
                        size_t    bytes = 0;
        };

        struct Run {
                        CommandContext  &ctx;
                        Region           regions[MaxRegions];
                        size_t           count = 0;
                        size_t           errors = 0;
                        MemTest::Failure reported[MaxReported];
                        size_t           reportedCount = 0;
                        uint64_t         yieldedUs = 0;
                        bool             interrupted = false;

                        explicit Run(CommandContext &c) : ctx(c) {}

                        ~Run() {
                                for (size_t i = 0; i < count; ++i) Memory::free(regions[i].words);
                        }

                        // Between chunks: Ctrl-C, and a tick for the idle task now and then.
                        bool keepGoing() {
                                if (ctx.isInterrupted()) interrupted = true;
                                const uint64_t now = System::uptimeUs();
                                if (now - yieldedUs > YieldUs) {
                                        Task::sleep(1);
                                        yieldedUs = now;
                                }
                                return !interrupted;
                        }

                        void record(const MemTest::Failure *f, size_t wrong) {
                                for (size_t i = 0; i < wrong && i < MaxReported && reportedCount < MaxReported; ++i)
                                        reported[reportedCount++] = f[i];
                                errors += wrong;
                                return;
                        }
        };

        bool parseSize(const char *text, size_t &out) {
                char         *end = nullptr;
                unsigned long v = std::strtoul(text, &end, 10);
                if (end == text || *end != '\0') return false;
                out = v;
                return true;
        }

        unsigned mbPerS(size_t bytes, uint64_t us) {
                return us == 0 ? 0 : static_cast<unsigned>(uint64_t(bytes) * 1'000'000 / us >> 20);
        }

        // Takes free PSRAM, largest blocks first, up to @p wanted bytes (0: all
        // but the reserve).
        void allocate(Run &run, size_t wanted) {
                size_t total = 0;
                while (run.count < MaxRegions) {
                        const size_t largest = Memory::stats(MemSpace::PsramDma).largestFreeBlock;
                        const size_t free = Memory::stats(MemSpace::PsramDma).freeBytes;
                        if (free < Reserve + MinRegion || largest < MinRegion + Align) break;
                        size_t bytes = largest - Align; // the heap's own overhead
                        if (free - bytes < Reserve) bytes = free - Reserve;
                        if (wanted != 0 && bytes > wanted - total) bytes = wanted - total;
                        bytes = bytes / DmaChunk * DmaChunk;
                        if (bytes < MinRegion) break;
                        void *p = Memory::allocAligned(MemSpace::PsramDma, bytes, Align);
                        if (p == nullptr) break;
                        run.regions[run.count++] = {static_cast<uint32_t *>(p), bytes};
                        total += bytes;
                        if (wanted != 0 && total >= wanted) break;
                }
                return;
        }

        // The CPU path: fill every chunk and push it out of the caches, then
        // read it all back from the PSRAM.
        void cpuPattern(Run &run, MemTest::Pattern pattern, uint32_t seed) {
                size_t   bytes = 0;
                uint64_t writeUs = 0, readUs = 0;
                size_t   before = run.errors;
                for (size_t r = 0; r < run.count && !run.interrupted; ++r) {
                        const Region  &region = run.regions[r];
                        const uint64_t start = System::uptimeUs();
                        for (size_t off = 0; off < region.bytes && run.keepGoing(); off += Chunk) {
                                uint32_t *w = region.words + off / 4;
                                MemTest::fill(pattern, seed, w, Chunk / 4, reinterpret_cast<uintptr_t>(w));
                                (void)esp_cache_msync(w, Chunk,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                        }
                        writeUs += System::uptimeUs() - start;
                }
                for (size_t r = 0; r < run.count && !run.interrupted; ++r) {
                        const Region  &region = run.regions[r];
                        const uint64_t start = System::uptimeUs();
                        for (size_t off = 0; off < region.bytes && run.keepGoing(); off += Chunk) {
                                const uint32_t  *w = region.words + off / 4;
                                MemTest::Failure f[MaxReported];
                                run.record(f, MemTest::verify(pattern, seed, w, Chunk / 4,
                                                              reinterpret_cast<uintptr_t>(w), f, MaxReported));
                        }
                        readUs += System::uptimeUs() - start;
                        bytes += region.bytes;
                }
                if (run.interrupted) return;
                run.ctx.printf("  %-8s write %4u MB/s  read %4u MB/s  %u errors\n", MemTest::name(pattern),
                               mbPerS(bytes, writeUs), mbPerS(bytes, readUs),
                               static_cast<unsigned>(run.errors - before));
                return;
        }

        bool dmaDone(async_memcpy_handle_t, async_memcpy_event_t *, void *arg) {
                static_cast<Semaphore *>(arg)->releaseFromIsr();
                return false;
        }

        // The DMA path: random words in each region's first half, copied by
        // the GDMA onto its second half, checked there against the source's keys.
        void dmaPattern(Run &run, async_memcpy_handle_t mcp, uint32_t seed) {
                Semaphore done{1, 0};
                size_t    bytes = 0;
                uint64_t  copyUs = 0;
                size_t    before = run.errors;
                bool      failed = false;
                for (size_t r = 0; r < run.count && !run.interrupted && !failed; ++r) {
                        const Region &region = run.regions[r];
                        const size_t  half = region.bytes / 2 / DmaChunk * DmaChunk;
                        uint8_t      *src = reinterpret_cast<uint8_t *>(region.words);
                        uint8_t      *dst = src + half;
                        for (size_t off = 0; off < half && run.keepGoing(); off += Chunk) {
                                auto *w = reinterpret_cast<uint32_t *>(src + off);
                                MemTest::fill(MemTest::Pattern::Random, seed, w, Chunk / 4,
                                              reinterpret_cast<uintptr_t>(w));
                                (void)esp_cache_msync(w, Chunk,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                        }
                        const uint64_t start = System::uptimeUs();
                        for (size_t off = 0; off < half && run.keepGoing(); off += DmaChunk) {
                                const esp_err_t err =
                                        esp_async_memcpy(mcp, dst + off, src + off, DmaChunk, dmaDone, &done);
                                if (err != ESP_OK || done.acquire(1000).isError()) {
                                        run.ctx.eprintf("memtest: DMA copy failed at %p\n", dst + off);
                                        failed = true;
                                        break;
                                }
                        }
                        copyUs += System::uptimeUs() - start;
                        // In chunks: a cache operation runs with interrupts off.
                        for (size_t off = 0; off < half; off += Chunk)
                                (void)esp_cache_msync(dst + off, Chunk, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        for (size_t off = 0; off < half && !failed && run.keepGoing(); off += Chunk) {
                                const auto      *w = reinterpret_cast<const uint32_t *>(dst + off);
                                MemTest::Failure f[MaxReported];
                                run.record(f, MemTest::verify(MemTest::Pattern::Random, seed, w, Chunk / 4,
                                                              reinterpret_cast<uintptr_t>(src + off), f, MaxReported));
                        }
                        bytes += half;
                }
                if (failed) ++run.errors;
                if (run.interrupted) return;
                run.ctx.printf("  %-8s copy  %4u MB/s                 %u errors\n", "dma", mbPerS(bytes, copyUs),
                               static_cast<unsigned>(run.errors - before));
                return;
        }

        // memtest [-p PASSES] [-m MB]
        int runMemtest(CommandContext &ctx) {
                const Args args = ctx.args();
                size_t     passes = 1, mb = 0;
                bool       dmaOnly = false;
                for (size_t i = 1; i < args.size(); ++i) {
                        if (std::strcmp(args[i], "-p") == 0 && i + 1 < args.size()) {
                                if (!parseSize(args[++i], passes)) return ctx.usage();
                        } else if (std::strcmp(args[i], "-m") == 0 && i + 1 < args.size()) {
                                if (!parseSize(args[++i], mb)) return ctx.usage();
                        } else if (std::strcmp(args[i], "-d") == 0) {
                                dmaOnly = true;
                        } else {
                                return ctx.usage();
                        }
                }
                Run run(ctx);
                allocate(run, mb * 1024 * 1024);
                size_t total = 0;
                for (size_t i = 0; i < run.count; ++i) total += run.regions[i].bytes;
                if (total == 0) {
                        ctx.eprintf("memtest: no free PSRAM to test\n");
                        return Shell::ExitFailure;
                }
                async_memcpy_config_t config = {};
                config.backlog = 8;
                config.dma_burst_size = 64;
                async_memcpy_handle_t mcp = nullptr;
                if (esp_async_memcpy_install_gdma_axi(&config, &mcp) != ESP_OK) mcp = nullptr;
                if (passes == 0)
                        ctx.printf("memtest: %u KB of PSRAM in %u blocks, until Ctrl-C\n",
                                   static_cast<unsigned>(total >> 10), static_cast<unsigned>(run.count));
                else
                        ctx.printf("memtest: %u KB of PSRAM in %u blocks, %u passes\n",
                                   static_cast<unsigned>(total >> 10), static_cast<unsigned>(run.count),
                                   static_cast<unsigned>(passes));
                const uint64_t started = System::uptimeUs();
                size_t         pass = 0;
                for (; (passes == 0 || pass < passes) && !run.interrupted; ++pass) {
                        ctx.printf("pass %u:\n", static_cast<unsigned>(pass + 1));
                        const auto seed = static_cast<uint32_t>(pass + 1);
                        for (MemTest::Pattern p : MemTest::Patterns) {
                                if (run.interrupted || dmaOnly) break;
                                cpuPattern(run, p, seed);
                        }
                        if (mcp != nullptr && !run.interrupted) dmaPattern(run, mcp, seed);
                }
                if (mcp != nullptr) (void)esp_async_memcpy_uninstall(mcp);
                for (size_t i = 0; i < run.reportedCount; ++i) {
                        const MemTest::Failure &f = run.reported[i];
                        ctx.printf("  at 0x%08lx: wrote %08lx, read %08lx (bits %08lx)\n",
                                   static_cast<unsigned long>(f.address), static_cast<unsigned long>(f.expected),
                                   static_cast<unsigned long>(f.actual),
                                   static_cast<unsigned long>(f.expected ^ f.actual));
                }
                const uint64_t secs = (System::uptimeUs() - started) / 1'000'000;
                ctx.printf("memtest: %u passes%s in %u s, %u errors\n",
                           static_cast<unsigned>(run.interrupted && pass > 0 ? pass - 1 : pass),
                           run.interrupted ? " (interrupted)" : "", static_cast<unsigned>(secs),
                           static_cast<unsigned>(run.errors));
                return run.errors == 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

} // namespace

BMDS_SHELL_COMMAND(memtest, runMemtest, "[-p PASSES (0: until Ctrl-C)] [-m MB] [-d (the DMA test only)]",
                   "Write and verify the free PSRAM through the CPU and the DMA")

BMDS_NAMESPACE_END
