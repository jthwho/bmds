/**
 * @file      benchsd.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SD card's benchmarks (devplan/bench.md §3; were `sd bench`).
 */

#include <bmds/bench.h>
#include <bmds/memory.h>
#include <bmds/sdcard.h>
#include <bmds/stopwatch.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   LargeRequestBytes = 64 * 1024;
        constexpr size_t   HugeRequestBytes = 256 * 1024;
        constexpr uint64_t MaxMb = 1024;

        constexpr BenchParam ReadParams[] = {
                {"mb", "8", "Megabytes read a run in large requests (a sixteenth in small ones)"},
        };

        // Reads `totalBytes` from the start of the card in requests of `requestBytes` into `buf`;
        // the rate in MB/s.
        Error readPass(uint8_t *buf, size_t requestBytes, uint64_t totalBytes, double &mbs) {
                SdCard        &card = SdCard::instance();
                const uint64_t sectors = card.sectorCount();
                const uint32_t perRequest = static_cast<uint32_t>(requestBytes / card.sectorSize());
                uint64_t       sector = 0;
                Stopwatch      watch;
                for (uint64_t done = 0; done < totalBytes; done += requestBytes) {
                        if (sector + perRequest > sectors) sector = 0;
                        if (Error err = card.read(sector, WritableByteSpan(buf, requestBytes)); err.isError())
                                return err;
                        sector += perRequest;
                }
                const int64_t us = watch.elapsedUs();
                mbs = us > 0 ? double(totalBytes) / double(us) : 0; // bytes per microsecond is MB/s
                return Error::Ok;
        }

        Error runRead(BenchContext &ctx) {
                uint64_t mb = 0;
                if (ctx.paramUnsigned("mb", mb).isError() || mb == 0 || mb > MaxMb) return Error::Invalid;
                SdCard &card = SdCard::instance();
                if (!card.isAttached()) return card.isInserted() ? Error::DeviceError : Error::NoMedia;
                // One DMA-capable, aligned buffer; 4 bytes in, the same memory is unaligned and goes
                // through the driver's bounce buffer.
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, HugeRequestBytes + 64));
                if (buf == nullptr) return Error::NoMem;
                const uint64_t total = mb * 1024 * 1024;
                struct Pass {
                                const char *metric;
                                size_t      offset;
                                size_t      request;
                                uint64_t    bytes;
                };
                // Small requests are slow: a sixteenth of the data is plenty.
                const Pass passes[] = {
                        {"direct_65536", 0, LargeRequestBytes, total},
                        {"bounced_65536", 4, LargeRequestBytes, total},
                        {"direct_512", 0, 512, total / 16},
                        {"bounced_512", 4, 512, total / 16},
                        // How a request's cost grows with its size (devplan/storage.md §18).
                        {"direct_16384", 0, 16 * 1024, total / 4},
                        {"direct_262144", 0, HugeRequestBytes, total},
                };
                Error err;
                for (const Pass &p : passes) {
                        err = ctx.measure(p.metric, "MB/s", BenchBetter::Higher, [&](double &out) {
                                return readPass(buf + p.offset, p.request, p.bytes, out);
                        });
                        if (err.isError()) break;
                }
                Memory::free(buf);
                return err;
        }

} // namespace

BMDS_BENCH_WITH(storageSdRead, "storage.sd.read", runRead, BenchQuick, ReadParams,
                "Raw reads from the start of the card, by request size, aligned and through the bounce buffer")

BMDS_NAMESPACE_END
