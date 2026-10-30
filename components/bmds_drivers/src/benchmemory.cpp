/**
 * @file      benchmemory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Memory's benchmarks (devplan/bench.md §3): PSRAM through the CPU with the caches written back
 * and invalidated (were `memtest`'s rates), through the AXI GDMA, and plain copies and fills (were
 * `display stress`: the traffic that may disturb the scan, which the meanwhile counters show); and
 * what the cache's own operations cost (§19: each runs with interrupts masked).
 */

#include <cstdio>
#include <cstring>
#include <sdkconfig.h>
#include <esp_async_memcpy.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <rom/cache.h>
#include <bmds/bench.h>
#include <bmds/memory.h>
#include <bmds/memtest.h>
#include <bmds/semaphore.h>
#include <bmds/stopwatch.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   Align = 128;           // the L2 cache line, and the GDMA's PSRAM alignment
        constexpr size_t   Chunk = 64 * 1024;     // written back or invalidated at a time
        constexpr size_t   DmaChunk = 256 * 1024; // one GDMA copy
        constexpr uint64_t MaxMb = 16;

        constexpr BenchParam PsramParams[] = {
                {"mb", "8", "Megabytes of PSRAM written and read a run"},
        };

        // A buffer from @p space, freed with it.
        struct Block {
                        uint8_t *p = nullptr;
                        Block(MemSpace space, size_t bytes) {
                                p = static_cast<uint8_t *>(Memory::allocAligned(space, bytes, Align));
                        }
                        ~Block() { Memory::free(p); }
        };

        double mbPerS(size_t bytes, int64_t us) {
                return us > 0 ? double(bytes) / double(us) * 1e6 / (1024 * 1024) : 0;
        }

        Error psramBytes(BenchContext &ctx, size_t &bytes) {
                uint64_t mb = 0;
                if (ctx.paramUnsigned("mb", mb).isError() || mb == 0 || mb > MaxMb) return Error::Invalid;
                bytes = size_t(mb) * 1024 * 1024;
                return Error::Ok;
        }

        // The CPU: a pattern written and pushed out of the caches, then read back from the PSRAM
        // (the caches invalidated) and checked, as `memtest` does it.
        Error runPsramCpu(BenchContext &ctx) {
                size_t bytes = 0;
                if (Error err = psramBytes(ctx, bytes); err.isError()) return err;
                Block block(MemSpace::PsramDma, bytes);
                if (block.p == nullptr) return Error::NoMem;
                auto      *words = reinterpret_cast<uint32_t *>(block.p);
                const auto pattern = MemTest::Pattern::Address;
                Error      err = ctx.measure("write_mb_s", "MB/s", BenchBetter::Higher, [&](double &out) {
                        Stopwatch watch;
                        for (size_t off = 0; off < bytes; off += Chunk) {
                                uint32_t *w = words + off / 4;
                                MemTest::fill(pattern, 1, w, Chunk / 4, reinterpret_cast<uintptr_t>(w));
                                (void)esp_cache_msync(w, Chunk,
                                                           ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                        }
                        out = mbPerS(bytes, watch.elapsedUs());
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                size_t wrong = 0;
                err = ctx.measure("read_mb_s", "MB/s", BenchBetter::Higher, [&](double &out) {
                        for (size_t off = 0; off < bytes; off += Chunk)
                                (void)esp_cache_msync(block.p + off, Chunk, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        Stopwatch watch;
                        for (size_t off = 0; off < bytes; off += Chunk) {
                                const uint32_t  *w = words + off / 4;
                                MemTest::Failure f[1];
                                wrong +=
                                        MemTest::verify(pattern, 1, w, Chunk / 4, reinterpret_cast<uintptr_t>(w), f, 1);
                        }
                        out = mbPerS(bytes, watch.elapsedUs());
                        return Error(Error::Ok);
                });
                if (err.isOk() && wrong != 0) {
                        ctx.note("%u words read back wrong (run memtest)", unsigned(wrong));
                        return Error::CorruptData;
                }
                return err;
        }

        bool dmaDone(async_memcpy_handle_t, async_memcpy_event_t *, void *arg) {
                static_cast<Semaphore *>(arg)->releaseFromIsr();
                return false;
        }

        // The AXI GDMA: half the buffer copied onto the other half.
        Error runPsramDma(BenchContext &ctx) {
                size_t bytes = 0;
                if (Error err = psramBytes(ctx, bytes); err.isError()) return err;
                bytes = bytes / (2 * DmaChunk) * (2 * DmaChunk);
                Block block(MemSpace::PsramDma, bytes);
                if (block.p == nullptr) return Error::NoMem;
                async_memcpy_config_t config = {};
                config.backlog = 8;
                config.dma_burst_size = 64;
                async_memcpy_handle_t mcp = nullptr;
                if (esp_async_memcpy_install_gdma_axi(&config, &mcp) != ESP_OK) return Error::Busy;
                Semaphore    done{1, 0};
                const size_t half = bytes / 2;
                const Error  err = ctx.measure("copy_mb_s", "MB/s", BenchBetter::Higher, [&](double &out) {
                        Stopwatch watch;
                        for (size_t off = 0; off < half; off += DmaChunk) {
                                if (esp_async_memcpy(mcp, block.p + half + off, block.p + off, DmaChunk, dmaDone,
                                                      &done) != ESP_OK ||
                                    done.acquire(1000).isError())
                                        return Error(Error::DeviceError);
                        }
                        out = mbPerS(half, watch.elapsedUs());
                        return Error(Error::Ok);
                });
                (void)esp_async_memcpy_uninstall(mcp);
                return err;
        }

        // Plain memcpy and memset through the caches, as programs move memory.
        Error runTraffic(BenchContext &ctx) {
                struct Case {
                                const char *metric;
                                MemSpace    space;
                                size_t      bytes;
                                bool        set;
                };
                const Case cases[] = {
                        {"psram_copy_mb_s", MemSpace::Psram, 1024 * 1024, false},
                        {"psram_set_mb_s", MemSpace::Psram, 1024 * 1024, true},
                        {"internal_copy_mb_s", MemSpace::Internal, 32 * 1024, false}, // two fit in internal RAM
                };
                for (const Case &c : cases) {
                        Block a(c.space, c.bytes), b(c.space, c.bytes);
                        if (a.p == nullptr || b.p == nullptr) return Error::NoMem;
                        const Error err = ctx.measure(c.metric, "MB/s", BenchBetter::Higher, [&](double &out) {
                                // A quarter second of it.
                                Stopwatch watch;
                                size_t    moved = 0;
                                while (watch.elapsedUs() < 250'000) {
                                        if (c.set) {
                                                std::memset(a.p, int(moved >> 10), c.bytes);
                                        } else {
                                                std::memcpy(b.p, a.p, c.bytes);
                                        }
                                        moved += c.bytes;
                                }
                                out = mbPerS(moved, watch.elapsedUs());
                                return Error(Error::Ok);
                        });
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }


        // A dependent chain of loads through @p bytes of @p space: each line's first word the next
        // line's address, in a random cycle (no prefetch helps); nanoseconds a load.
        Error chase(BenchContext &ctx, const char *metric, MemSpace space, size_t bytes) {
                constexpr size_t Line = 128; // the L2's line
                const size_t     lines = bytes / Line;
                Block            block(space, bytes);
                if (block.p == nullptr) return Error::NoMem;
                auto *order = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, lines * sizeof(uint32_t)));
                if (order == nullptr) return Error::NoMem;
                for (size_t i = 0; i < lines; ++i) order[i] = uint32_t(i);
                uint32_t seed = 12345;
                for (size_t i = lines - 1; i > 0; --i) { // Fisher-Yates
                        seed = seed * 1103515245u + 12345u;
                        const size_t   j = (seed >> 8) % (i + 1);
                        const uint32_t t = order[i];
                        order[i] = order[j];
                        order[j] = t;
                }
                for (size_t i = 0; i < lines; ++i)
                        *reinterpret_cast<uint8_t **>(block.p + order[i] * Line) =
                                block.p + order[(i + 1) % lines] * Line;
                Memory::free(order);
                constexpr uint32_t Loads = 200000;
                return ctx.measure(metric, "ns", BenchBetter::Lower, [&](double &out) {
                        uint8_t  *p = block.p;
                        Stopwatch watch;
                        for (uint32_t i = 0; i < Loads; ++i) p = *reinterpret_cast<uint8_t *volatile *>(p);
                        out = double(watch.cycles()) * 1000.0 / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ / Loads;
                        return p != nullptr ? Error(Error::Ok) : Error(Error::Invalid);
                });
        }

        Error runLatency(BenchContext &ctx) {
                struct Case {
                                const char *metric;
                                MemSpace    space;
                                size_t      bytes;
                };
                const Case cases[] = {
                        {"internal_16k_ns", MemSpace::Internal, 16 * 1024},
                        {"internal_64k_ns", MemSpace::Internal, 64 * 1024},
                        {"psram_64k_ns", MemSpace::Psram, 64 * 1024},
                        {"psram_256k_ns", MemSpace::Psram, 256 * 1024},
                        {"psram_4m_ns", MemSpace::Psram, 4 * 1024 * 1024},
                };
                for (const Case &c : cases) {
                        if (Error err = chase(ctx, c.metric, c.space, c.bytes); err.isError()) return err;
                }
                return Error::Ok;
        }

        // The cache's operations on @p bytes of @p space, as the drivers call them (esp_cache_msync()
        // in 64 KB pieces): a write-back of lines the CPU just wrote (dirty) and of the same lines
        // again (clean), and an invalidate; µs each. Each piece runs with interrupts masked.
        Error cacheOps(BenchContext &ctx, const char *name, MemSpace space, size_t bytes) {
                Block block(space, bytes);
                if (block.p == nullptr) return Error::NoMem;
                auto sync = [&](int flags) {
                        for (size_t off = 0; off < bytes; off += Chunk)
                                (void)esp_cache_msync(block.p + off, bytes - off < Chunk ? bytes - off : Chunk, flags);
                };
                char metric[48];
                std::snprintf(metric, sizeof(metric), "%s_writeback_dirty_us", name);
                Error err = ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                        std::memset(block.p, 0x5a, bytes);
                        Stopwatch watch;
                        sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        out = double(watch.cycles()) / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                std::snprintf(metric, sizeof(metric), "%s_writeback_clean_us", name);
                err = ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                        sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        Stopwatch watch;
                        sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        out = double(watch.cycles()) / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                std::snprintf(metric, sizeof(metric), "%s_invalidate_us", name);
                return ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                        sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        Stopwatch watch;
                        sync(ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        out = double(watch.cycles()) / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
                        return Error(Error::Ok);
                });
        }

        Error runCache(BenchContext &ctx) {
                if (Error err = cacheOps(ctx, "psram_4k", MemSpace::Psram, 4 * 1024); err.isError()) return err;
                if (Error err = cacheOps(ctx, "psram_64k", MemSpace::Psram, 64 * 1024); err.isError()) return err;
                if (Error err = cacheOps(ctx, "psram_576k", MemSpace::Psram, 576 * 1024); err.isError()) return err;
                return cacheOps(ctx, "internal_64k", MemSpace::InternalDma, 64 * 1024);
        }

        // The LP RAM (RTC memory), uncached, as records kept across resets use it
        // (devplan/keptstate.md): a log line's copy, 4 KB written and read, a word's store.
        Error runRtc(BenchContext &ctx) {
                constexpr size_t Bytes = 4096;
                struct Buffers {
                                uint8_t *rtc = static_cast<uint8_t *>(heap_caps_malloc(Bytes, MALLOC_CAP_RTCRAM));
                                uint8_t *src = static_cast<uint8_t *>(Memory::alloc(MemSpace::Internal, Bytes));
                                ~Buffers() {
                                        heap_caps_free(rtc);
                                        Memory::free(src);
                                }
                } b;
                if (b.rtc == nullptr || b.src == nullptr) return Error::NoMem;
                uint8_t *rtc = b.rtc;
                uint8_t *src = b.src;
                std::memset(src, 'x', Bytes);
                auto ns = [](uint32_t cycles) {
                        return double(cycles) * 1000.0 / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
                };
                Error err = ctx.measure("line_100_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        for (size_t i = 0; i < 32; ++i) std::memcpy(rtc + i * 101 % (Bytes - 128), src, 100);
                        out = ns(watch.cycles()) / 32;
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                err = ctx.measure("write_4k_us", "us", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        std::memcpy(rtc, src, Bytes);
                        out = ns(watch.cycles()) / 1000;
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                err = ctx.measure("read_4k_us", "us", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        std::memcpy(src, rtc, Bytes);
                        out = ns(watch.cycles()) / 1000;
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                {
                        // Against it: a line written into PSRAM, then its cache lines written back
                        // by address (L1, then L2), as the log could after each line.
                        Block psram(MemSpace::Psram, Bytes);
                        if (psram.p == nullptr) return Error::NoMem;
                        err = ctx.measure("psram_line_writeback_ns", "ns", BenchBetter::Lower, [&](double &out) {
                                uint32_t cycles = 0;
                                for (size_t i = 0; i < 32; ++i) {
                                        uint8_t *at = psram.p + i * 101 % (Bytes - 128);
                                        std::memcpy(at, src, 100);
                                        Stopwatch  watch;
                                        const auto addr = uint32_t(uintptr_t(at));
                                        (void)Cache_WriteBack_Addr(CACHE_MAP_L1_DCACHE, addr, 100);
                                        (void)Cache_WriteBack_Addr(CACHE_MAP_L2_CACHE, addr, 100);
                                        cycles += watch.cycles();
                                }
                                out = ns(cycles) / 32;
                                return Error(Error::Ok);
                        });
                        if (err.isError()) return err;
                        err = ctx.measure("psram_line_msync_ns", "ns", BenchBetter::Lower, [&](double &out) {
                                uint32_t cycles = 0;
                                for (size_t i = 0; i < 32; ++i) {
                                        uint8_t *at = psram.p + i * 101 % (Bytes - 128);
                                        std::memcpy(at, src, 100);
                                        Stopwatch watch;
                                        (void)esp_cache_msync(
                                                at, 100, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                                        cycles += watch.cycles();
                                }
                                out = ns(cycles) / 32;
                                return Error(Error::Ok);
                        });
                        if (err.isError()) return err;
                }
                err = ctx.measure("store_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        volatile uint32_t *w = reinterpret_cast<volatile uint32_t *>(rtc);
                        Stopwatch          watch;
                        for (uint32_t i = 0; i < 256; ++i) w[i] = i;
                        out = ns(watch.cycles()) / 256;
                        return Error(Error::Ok);
                });
                return err;
        }

} // namespace

BMDS_BENCH(memoryRtc, "memory.rtc", runRtc, BenchQuick,
           "The LP RAM (RTC memory, uncached): a 100-byte line copied, 4 KB written and read, a word stored")
BMDS_BENCH(memoryCache, "memory.cache", runCache, BenchQuick,
           "The cache's write-back (of dirty and clean lines) and invalidate, by size: us each")
BMDS_BENCH_WITH(memoryPsramCpu, "memory.psram.cpu", runPsramCpu, BenchQuick, PsramParams,
                "PSRAM through the CPU, the caches written back and invalidated: writing, then reading")
BMDS_BENCH_WITH(memoryPsramDma, "memory.psram.dma", runPsramDma, BenchQuick, PsramParams,
                "PSRAM copied to PSRAM by the AXI GDMA")
BMDS_BENCH(memoryLatency, "memory.latency", runLatency, BenchQuick,
           "A chain of dependent loads in a random cycle: internal RAM and PSRAM by size (ns a load)")
BMDS_BENCH(memoryTraffic, "memory.traffic", runTraffic, BenchQuick,
           "memcpy and memset through the caches: PSRAM and internal RAM")

BMDS_NAMESPACE_END
