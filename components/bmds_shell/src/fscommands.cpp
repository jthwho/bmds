/**
 * @file      fscommands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Filesystem built-ins. Their arguments and output are a userspace interface.
 */

#include <bmds/console.h>
#include <bmds/fscommands.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // The VFS's view of this shell: where relative paths start, and its terminal.
        Vfs::Context vfsContext(CommandContext &ctx) {
                return Vfs::Context{"/", &ctx.shell().console()};
        }

} // namespace

int FsCommands::mount(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() == 1) {
                Vfs::MountInfo mounts[Vfs::MaxMounts];
                const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
                for (size_t i = 0; i < n; ++i)
                        ctx.printf("/dev/%s on %s type %s\n", mounts[i].device, mounts[i].path, mounts[i].type);
                return Shell::ExitSuccess;
        }
        if (args.size() != 3) return ctx.usage();
        BlockDevice *device = Vfs::findDevice(args[1], vfsContext(ctx));
        if (device == nullptr) {
                ctx.eprintf("mount: %s: not a block device\n", args[1]);
                return Shell::ExitFailure;
        }
        const Error err = Vfs::mount(args[2], *device);
        if (err.isError()) {
                ctx.eprintf("mount: %s: %s\n", args[2], err.desc());
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
                ctx.eprintf("umount: %s: %s\n", args[1], err.desc());
                return Shell::ExitFailure;
        }
        return Shell::ExitSuccess;
}

BMDS_SHELL_COMMAND(mount, FsCommands::mount, "[DEVICE PATH]",
                   "List the mounts; mount a device's filesystem (e.g. mount /dev/sd0p0 /data)")
BMDS_SHELL_COMMAND(umount, FsCommands::umount, "PATH", "Unmount the filesystem at PATH")

BMDS_NAMESPACE_END
