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

#include <bmds/command.h>
#include <bmds/sdcard.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

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

        int runSd(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return showInfo(ctx);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(sd, runSd, "", "Show the SD card (its speed: 'bench run storage.sd')")

BMDS_NAMESPACE_END
