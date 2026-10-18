/**
 * @file      benchmemory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Memory's benchmarks (devplan/bench.md §3): PSRAM through the CPU with the caches written back
 * and invalidated (were `memtest`'s rates), through the AXI GDMA, and plain copies and fills (were
 * `display stress`: the traffic that may disturb the scan, which the meanwhile counters show).
 */

#include <cstring>
#include <sdkconfig.h>
#include <esp_async_memcpy.h>
#include <esp_cache.h>
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

} // namespace

BMDS_BENCH_WITH(memoryPsramCpu, "memory.psram.cpu", runPsramCpu, BenchQuick, PsramParams,
                "PSRAM through the CPU, the caches written back and invalidated: writing, then reading")
BMDS_BENCH_WITH(memoryPsramDma, "memory.psram.dma", runPsramDma, BenchQuick, PsramParams,
                "PSRAM copied to PSRAM by the AXI GDMA")
BMDS_BENCH(memoryLatency, "memory.latency", runLatency, BenchQuick,
           "A chain of dependent loads in a random cycle: internal RAM and PSRAM by size (ns a load)")
BMDS_BENCH(memoryTraffic, "memory.traffic", runTraffic, BenchQuick,
           "memcpy and memset through the caches: PSRAM and internal RAM")

BMDS_NAMESPACE_END
