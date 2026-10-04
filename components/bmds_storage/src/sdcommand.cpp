/**
 * @file      sdcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `sd` command. Its output format is a userspace interface: keep the
 * lines and their order, and add new ones only at the end.
 */

#include <cstdlib>
#include <cstring>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/sdcard.h>
#include <bmds/shell.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr size_t   BenchBufferBytes = 64 * 1024;
        constexpr uint32_t DefaultBenchMb = 16;
        constexpr uint32_t MaxBenchMb = 1024;

        const char *typeName(SdCard::Type type) {
                switch (type) {
                        case SdCard::Type::Sdsc: return "SDSC";
                        case SdCard::Type::Sdhc: return "SDHC";
                        case SdCard::Type::Sdxc: return "SDXC";
                        case SdCard::Type::Mmc: return "MMC";
                        default: return "other";
                }
        }

        int noCard(CommandContext &ctx, SdCard &card) {
                ctx.eprintf("sd: %s\n", card.isInserted() ? "card not usable (see 'log show')" : "no card");
                return Shell::ExitFailure;
        }

        int showInfo(CommandContext &ctx) {
                SdCard            &card = SdCard::instance();
                const SdCard::Info info = card.info();
                if (!info.attached) return noCard(ctx, card);
                const uint64_t bytes = info.sectorCount * info.sectorSize;
                const uint64_t tenthsGb = bytes / 100000000;
                ctx.printf("Card:     %s (%s)\n", info.name, typeName(info.type));
                ctx.printf("Capacity: %llu.%llu GB, %llu sectors of %u bytes\n",
                           static_cast<unsigned long long>(tenthsGb / 10),
                           static_cast<unsigned long long>(tenthsGb % 10),
                           static_cast<unsigned long long>(info.sectorCount), static_cast<unsigned>(info.sectorSize));
                ctx.printf("Bus:      %u-bit, %u.%u MHz\n", info.busWidth, static_cast<unsigned>(info.clockKhz / 1000),
                           static_cast<unsigned>(info.clockKhz % 1000 / 100));
                ctx.printf("Maker:    0x%02x, OEM 0x%04x, revision %u.%u, serial 0x%08x, made %04u-%02u\n",
                           info.manufacturerId, info.oemId, info.revision >> 4, info.revision & 0xf,
                           static_cast<unsigned>(info.serial), info.year, info.month);
                return Shell::ExitSuccess;
        }

        // Reads `totalBytes` from the start of the card in requests of
        // `requestBytes` into `buf`; prints the rate. False if stopped.
        bool benchRead(CommandContext &ctx, const char *label, uint8_t *buf, size_t requestBytes, uint64_t totalBytes) {
                SdCard        &card = SdCard::instance();
                const uint64_t sectors = card.sectorCount();
                const uint32_t perRequest = static_cast<uint32_t>(requestBytes / card.sectorSize());
                uint64_t       sector = 0;
                const uint64_t start = System::uptimeUs();
                for (uint64_t done = 0; done < totalBytes; done += requestBytes) {
                        if (ctx.isInterrupted()) return false;
                        if (sector + perRequest > sectors) sector = 0;
                        const Error err = card.read(sector, WritableByteSpan(buf, requestBytes));
                        if (err.isError()) {
                                ctx.eprintf("sd: read failed at sector %llu: %s\n",
                                            static_cast<unsigned long long>(sector), err.desc());
                                return false;
                        }
                        sector += perRequest;
                }
                const uint64_t us = System::uptimeUs() - start;
                // Bytes per microsecond is MB/s.
                const uint64_t tenthsMbs = us == 0 ? 0 : totalBytes * 10 / us;
                const uint64_t ms = us / 1000;
                ctx.printf("%-8s %5u B requests: %4llu KB in %llu.%03llu s, %llu.%llu MB/s\n", label,
                           static_cast<unsigned>(requestBytes), static_cast<unsigned long long>(totalBytes / 1024),
                           static_cast<unsigned long long>(ms / 1000), static_cast<unsigned long long>(ms % 1000),
                           static_cast<unsigned long long>(tenthsMbs / 10),
                           static_cast<unsigned long long>(tenthsMbs % 10));
                return true;
        }

        int runBench(CommandContext &ctx, const char *sizeArg) {
                uint32_t mb = DefaultBenchMb;
                if (sizeArg != nullptr) {
                        char         *end = nullptr;
                        unsigned long v = std::strtoul(sizeArg, &end, 10);
                        if (end == sizeArg || *end != '\0' || v == 0 || v > MaxBenchMb) return ctx.usage();
                        mb = static_cast<uint32_t>(v);
                }
                SdCard &card = SdCard::instance();
                if (!card.isAttached()) return noCard(ctx, card);
                // One DMA-capable, aligned buffer; 4 bytes in, the same memory
                // is unaligned and goes through the driver's bounce buffer.
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, BenchBufferBytes + 64));
                if (buf == nullptr) {
                        ctx.eprintf("sd: %s\n", Error(Error::NoMem).desc());
                        return Shell::ExitFailure;
                }
                const uint64_t total = static_cast<uint64_t>(mb) * 1024 * 1024;
                // Small requests are slow; a sixteenth of the data is plenty.
                const bool ok = benchRead(ctx, "direct", buf, BenchBufferBytes, total) &&
                                benchRead(ctx, "bounced", buf + 4, BenchBufferBytes, total) &&
                                benchRead(ctx, "direct", buf, 512, total / 16) &&
                                benchRead(ctx, "bounced", buf + 4, 512, total / 16);
                Memory::free(buf);
                if (ok) return Shell::ExitSuccess;
                return ctx.isInterrupted() ? Shell::ExitInterrupted : Shell::ExitFailure;
        }

        int runSd(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return showInfo(ctx);
                if (std::strcmp(args[1], "bench") == 0 && args.size() <= 3)
                        return runBench(ctx, args.size() == 3 ? args[2] : nullptr);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(sd, runSd, "[bench [megabytes]]",
                   "Show the SD card; 'bench' measures reading (default 16 MB, from the start of the card)")

BMDS_NAMESPACE_END
