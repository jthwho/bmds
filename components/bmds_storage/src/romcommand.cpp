/**
 * @file      romcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `rom` command (devplan/rom.md §5): what is in /rom, and its content
 * checked. Its output format is a userspace interface: keep the lines and
 * their order, and add new ones only at the end.
 */

#include <cstring>
#include <esp_ota_ops.h>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/datetime.h>
#include <bmds/romdisk.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        int noRom(CommandContext &ctx) {
                if (RomDisk::partitionBytes() == 0) {
                        ctx.eprintf("rom: no flash partition '%s'\n", RomDisk::PartitionLabel);
                } else {
                        ctx.eprintf("rom: no ROM image in its partition (blank or damaged: see 'log show')\n");
                }
                return Shell::ExitFailure;
        }

        int showInfo(CommandContext &ctx) {
                const RomImage *image = RomDisk::image();
                if (image == nullptr) return noRom(ctx);
                const DateTime t = DateTime::fromSeconds(image->buildTime().seconds());
                char           hash[2 * RomImage::HashBytes + 1];
                for (size_t i = 0; i < RomImage::HashBytes; ++i) {
                        hash[2 * i] = "0123456789abcdef"[image->hash()[i] >> 4];
                        hash[2 * i + 1] = "0123456789abcdef"[image->hash()[i] & 0xf];
                }
                hash[sizeof(hash) - 1] = '\0';
                ctx.printf("ROM:      %s, format 1, %u entries\n", RomDisk::Path,
                           static_cast<unsigned>(image->count()));
                ctx.printf("Built:    %04ld-%02u-%02u %02u:%02u:%02u UTC\n", static_cast<long>(t.year), t.month, t.day,
                           t.hour, t.minute, t.second);
                ctx.printf("Content:  %s\n", hash);
                ctx.printf("Matches:  %s\n", RomDisk::matchesFirmware()
                                                     ? "the firmware"
                                                     : "not the firmware's (flash both: idf.py flash)");
                ctx.printf("Size:     %u KB of the partition's %u KB\n",
                           static_cast<unsigned>((image->bytes().size() + 1023) / 1024),
                           static_cast<unsigned>(RomDisk::partitionBytes() / 1024));
                const esp_partition_t *running = esp_ota_get_running_partition();
                if (running != nullptr) {
                        const bool main = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0;
                        ctx.printf("Running:  %s (%s), at 0x%x\n", running->label, main ? "main" : "recovery",
                                   static_cast<unsigned>(running->address));
                }
                return Shell::ExitSuccess;
        }

        int check(CommandContext &ctx) {
                const RomImage *image = RomDisk::image();
                if (image == nullptr) return noRom(ctx);
                const int64_t start = Clock::monotonicNs();
                const Error   err = image->verify();
                const int64_t ms = (Clock::monotonicNs() - start) / 1000000;
                if (err.isError()) {
                        ctx.eprintf("rom: its content is damaged (its hash differs): flash it again (idf.py flash)\n");
                        return Shell::ExitFailure;
                }
                ctx.printf("rom: content good (%u KB hashed in %ld ms)\n",
                           static_cast<unsigned>(image->bytes().size() / 1024), static_cast<long>(ms));
                return Shell::ExitSuccess;
        }

        int runRom(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() == 1 || (args.size() == 2 && std::strcmp(args[1], "info") == 0)) return showInfo(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "check") == 0) return check(ctx);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(rom, runRom, "[info | check]",
                   "Show what /rom holds (the files that come with the firmware), or check its content")

BMDS_NAMESPACE_END
