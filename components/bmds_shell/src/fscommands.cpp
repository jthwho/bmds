/**
 * @file      fscommands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Filesystem built-ins. Their arguments and output are a userspace interface.
 */

#include <cstdio>
#include <cstdlib>
#include <bmds/blockdevices.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/semaphore.h>
#include <bmds/console.h>
#include <bmds/fscommands.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // Sizes as df -h prints them: powers of 1024, one decimal below 10,
        // rounded up (as coreutils does).
        void human(uint64_t bytes, char (&out)[24]) {
                static constexpr char Units[] = "KMGTPE";
                if (bytes < 1024) {
                        std::snprintf(out, sizeof(out), "%llu", static_cast<unsigned long long>(bytes));
                        return;
                }
                uint64_t unit = 1024;
                size_t   u = 0;
                while (u + 1 < sizeof(Units) - 1 && bytes >= unit * 1024) {
                        unit *= 1024;
                        ++u;
                }
                const uint64_t tenths = (bytes * 10 + unit - 1) / unit;
                if (tenths < 100) {
                        std::snprintf(out, sizeof(out), "%llu.%llu%c", static_cast<unsigned long long>(tenths / 10),
                                      static_cast<unsigned long long>(tenths % 10), Units[u]);
                } else {
                        std::snprintf(out, sizeof(out), "%llu%c",
                                      static_cast<unsigned long long>((bytes + unit - 1) / unit), Units[u]);
                }
                return;
        }

        // The default mount path of partition @p name ("sd0p1" is /sd0/p1); false if it isn't one.
        bool defaultMountPath(StringView name, char (&path)[Vfs::MaxMountPath + 1]) {
                const size_t p = name.lastIndexOf('p');
                if (p == StringView::NPos || p == 0 || p + 1 >= name.size()) return false;
                std::snprintf(path, sizeof(path), "/%.*s/p%.*s", static_cast<int>(p), name.data(),
                              static_cast<int>(name.size() - p - 1), name.data() + p + 1);
                return true;
        }

        // What a mount is of, as Linux names it: its device node, or its type when it has none
        // (a tmpfs).
        const char *sourceOf(const Vfs::MountInfo &mount, char (&out)[24]) {
                if (mount.device[0] == '\0') return mount.type;
                std::snprintf(out, sizeof(out), "/dev/%s", mount.device);
                return out;
        }

} // namespace

int FsCommands::mount(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() == 1) {
                Vfs::MountInfo mounts[Vfs::MaxMounts];
                const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
                char           source[24];
                for (size_t i = 0; i < n; ++i)
                        ctx.printf("%s on %s type %s\n", sourceOf(mounts[i], source), mounts[i].path, mounts[i].type);
                return Shell::ExitSuccess;
        }
        if (args.size() != 3) return ctx.usage();
        BlockDevice *device = Vfs::findDevice(args[1], ctx.vfs());
        if (device == nullptr) {
                ctx.eprintf("mount: %s: not a block device\n", args[1]);
                return Shell::ExitFailure;
        }
        const Error err = Vfs::mount(args[2], *device);
        if (err.isError()) {
                ctx.eprintf("mount: %s: %s\n", args[2], CommandContext::errorText(err));
                return Shell::ExitFailure;
        }
        return Shell::ExitSuccess;
}

int FsCommands::umount(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() != 2) return ctx.usage();
        const Error err = Vfs::unmount(args[1]);
        if (err == Error::NotExist) {
                ctx.eprintf("umount: %s: not mounted\n", args[1]);
                return Shell::ExitFailure;
        }
        if (err.isError()) {
                ctx.eprintf("umount: %s: %s\n", args[1], CommandContext::errorText(err));
                return Shell::ExitFailure;
        }
        return Shell::ExitSuccess;
}

int FsCommands::sync(CommandContext &ctx) {
        if (ctx.args().size() != 1) return ctx.usage();
        const Error err = Vfs::sync();
        if (err.isError()) {
                ctx.eprintf("sync: %s\n", CommandContext::errorText(err));
                return Shell::ExitFailure;
        }
        return Shell::ExitSuccess;
}

int FsCommands::df(CommandContext &ctx) {
        const Args args = ctx.args();
        bool       readable = false;
        for (size_t i = 1; i < args.size(); ++i) {
                if (StringView(args[i]) != "-h") return ctx.usage();
                readable = true;
        }
        Vfs::MountInfo mounts[Vfs::MaxMounts];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
        // Rows of text first, so the columns can be sized as coreutils does.
        char cells[Vfs::MaxMounts + 1][5][24] = {};
        std::snprintf(cells[0][0], 24, "Filesystem");
        std::snprintf(cells[0][1], 24, "%s", readable ? "Size" : "1K-blocks");
        std::snprintf(cells[0][2], 24, "Used");
        std::snprintf(cells[0][3], 24, "%s", readable ? "Avail" : "Available");
        std::snprintf(cells[0][4], 24, "Use%%");
        size_t rows = 1;
        for (size_t i = 0; i < n; ++i) {
                const Result<FsInfo> fs = Vfs::statfs(mounts[i].path, ctx.vfs());
                if (fs.error().isError()) continue;
                const uint64_t total = fs.value().totalBytes;
                const uint64_t avail = fs.value().freeBytes;
                const uint64_t used = total - avail;
                char(&row)[5][24] = cells[rows];
                char source[24];
                std::snprintf(row[0], 24, "%s", sourceOf(mounts[i], source));
                const uint64_t values[3] = {total, used, avail};
                for (int k = 0; k < 3; ++k) {
                        if (readable) {
                                char text[24];
                                human(values[k], text);
                                std::snprintf(row[1 + k], 24, "%s", text);
                        } else {
                                std::snprintf(row[1 + k], 24, "%llu",
                                              static_cast<unsigned long long>((values[k] + 1023) / 1024));
                        }
                }
                const uint64_t percent = used == 0 ? 0 : (used * 100 + used + avail - 1) / (used + avail);
                std::snprintf(row[4], 24, "%llu%%", static_cast<unsigned long long>(percent));
                ++rows;
        }
        int width[5] = {14, 0, 0, 0, 0};
        for (size_t r = 0; r < rows; ++r) {
                for (int c = 0; c < 5; ++c) {
                        const int len = static_cast<int>(StringView(cells[r][c]).size());
                        if (len > width[c]) width[c] = len;
                }
        }
        for (size_t r = 0; r < rows; ++r) {
                ctx.printf("%-*s %*s %*s %*s %*s %s\n", width[0], cells[r][0], width[1], cells[r][1], width[2],
                           cells[r][2], width[3], cells[r][3], width[4], cells[r][4],
                           r == 0 ? "Mounted on" : mounts[r - 1].path);
        }
        return Shell::ExitSuccess;
}

int FsCommands::mkfs(CommandContext &ctx) {
        const Args  args = ctx.args();
        FsFormat    format = FsFormat::Auto;
        bool        confirmed = false;
        const char *path = nullptr;
        for (size_t i = 1; i < args.size(); ++i) {
                const StringView arg(args[i]);
                if (arg == "-y") {
                        confirmed = true;
                } else if (arg == "-t" && i + 1 < args.size()) {
                        const StringView type(args[++i]);
                        if (type == "fat32") {
                                format = FsFormat::Fat32;
                        } else if (type == "exfat") {
                                format = FsFormat::ExFat;
                        } else if (type != "auto") {
                                return ctx.usage();
                        }
                } else if (path == nullptr && !arg.startsWith('-')) {
                        path = args[i];
                } else {
                        return ctx.usage();
                }
        }
        if (path == nullptr) return ctx.usage();
        BlockDevice *device = Vfs::findDevice(path, ctx.vfs());
        char         name[BlockDevices::MaxName + 1] = {};
        if (device == nullptr || BlockDevices::nameOf(*device, name).isError()) {
                ctx.eprintf("mkfs: %s: not a block device\n", path);
                return Shell::ExitFailure;
        }
        if (!confirmed) {
                ctx.eprintf("mkfs: this erases everything on /dev/%s; run again with -y to go ahead\n", name);
                return Shell::ExitFailure;
        }
        // A partition (sd0p0) is formatted alone; a whole device gets a new
        // partition table with one partition, re-read afterwards.
        bool isWhole = true;
        {
                BlockDevices::Entry entries[BlockDevices::MaxDevices];
                const size_t        count = BlockDevices::list(Span<BlockDevices::Entry>(entries));
                for (size_t i = 0; i < count; ++i) {
                        if (entries[i].device == device) isWhole = entries[i].parent == nullptr;
                }
        }
        char       mountPath[Vfs::MaxMountPath + 1];
        const bool partition = !isWhole && defaultMountPath(name, mountPath);
        if (isWhole) {
                Vfs::eject(*device);
        } else {
                (void)Vfs::unmountDevice(*device);
        }
        Error err = Vfs::format(*device, format);
        if (err.isError()) {
                ctx.eprintf("mkfs: /dev/%s: %s\n", name, CommandContext::errorText(err));
                return Shell::ExitFailure;
        }
        if (isWhole) {
                err = Vfs::automount(*device).error();
        } else if (partition) {
                err = Vfs::mount(mountPath, *device);
        }
        if (err.isError())
                ctx.eprintf("mkfs: /dev/%s: formatted, but not mounted: %s\n", name, CommandContext::errorText(err));
        Vfs::MountInfo mounts[Vfs::MaxMounts];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
        for (size_t i = 0; i < n; ++i) {
                if (StringView(mounts[i].device).startsWith(StringView(name)))
                        ctx.printf("/dev/%s on %s type %s\n", mounts[i].device, mounts[i].path, mounts[i].type);
        }
        return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
}

int FsCommands::waitmount(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() != 3) return ctx.usage();
        char      *end = nullptr;
        const long ms = std::strtol(args[2], &end, 10);
        if (end == args[2] || *end != '\0' || ms < 0 || ms > 600000) return ctx.usage();
        const Result<String> path = Vfs::normalize(args[1], ctx.cwd());
        if (path.error().isError()) return ctx.usage();
        if (ms == 0) return Vfs::isMounted(path.value().view()) ? Shell::ExitSuccess : Shell::ExitFailure;
        // Woken by each mount, the end of settling (or Ctrl-C); the mount table is looked at after
        // registering, so a mount in between isn't missed.
        Interrupt     &interrupt = ctx.shell().interrupt();
        const Deadline deadline(static_cast<unsigned int>(ms));
        for (;;) {
                Semaphore        woken(1, 0);
                WaitList::Entry  entry(Vfs::mountWaiters(), woken);
                Interrupt::Watch watch(interrupt, woken);
                if (Vfs::isMounted(path.value().view())) return Shell::ExitSuccess;
                if (interrupt.isRaised()) return Shell::ExitInterrupted;
                // Nothing on its way (no card in the slot): no use waiting.
                if (deadline.isExpired() || !Vfs::isSettling()) return Shell::ExitFailure;
                if (entry.isAdded()) {
                        (void)woken.acquire(deadline);
                } else {
                        (void)woken.acquire(10); // every waiter's slot taken: look again shortly
                }
        }
}

BMDS_SHELL_COMMAND(df, FsCommands::df, "[-h]", "Show size and free space of the mounted filesystems")
BMDS_SHELL_COMMAND(mkfs, FsCommands::mkfs, "[-t auto|fat32|exfat] -y DEVICE",
                   "Format a device (a whole card: new partition table) or partition, then mount it")
BMDS_SHELL_COMMAND(mount, FsCommands::mount, "[DEVICE PATH]",
                   "List the mounts; mount a device's filesystem (e.g. mount /dev/sd0p0 /data)")
BMDS_SHELL_COMMAND(sync, FsCommands::sync, "",
                   "Write what the filesystems hold to their devices (the card's directories and allocation "
                   "are otherwise written within a second)")
BMDS_SHELL_COMMAND(umount, FsCommands::umount, "PATH", "Unmount the filesystem at PATH")
BMDS_SHELL_COMMAND(waitmount, FsCommands::waitmount, "PATH MS",
                   "Wait up to MS milliseconds for a filesystem at PATH while one may come (e.g. waitmount "
                   "/sd0/p0 3000); an empty card slot fails at once")

BMDS_NAMESPACE_END
