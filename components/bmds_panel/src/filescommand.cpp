/**
 * @file      filescommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `files [PATH...]` (devplan/filebrowser.md §4h, FB5): a folder window on the desktop for each
 * path (the current directory without one; a file's folder with the file selected). The
 * panel's `files` service opens them (FB6): the paths go to it absolute, NUL after each.
 */

#include <bmds/command.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/vfs.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Service("files");

        // @p message to the panel's service. @return The status: 0, or 1 (told).
        int send(CommandContext &ctx, StringView message) {
                auto [wm, err] = WindowManager::manager(0);
                if (err.isOk()) {
                        err = wm->sendToService(
                                Service, ByteSpan(reinterpret_cast<const uint8_t *>(message.data()), message.size()));
                }
                if (err.isOk()) return Shell::ExitSuccess;
                if (err == Error::NotFound || err == Error::InvalidState || err == Error::NotSupported) {
                        ctx.eprintf("files: the desktop isn't running (start 'panel')\n");
                } else if (err == Error::Full) {
                        ctx.eprintf("files: the desktop is busy: try again\n");
                } else {
                        ctx.eprintf("files: %s\n", CommandContext::errorText(err));
                }
                return Shell::ExitFailure;
        }

        int files(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                for (size_t i = 1; i < args.size(); ++i)
                        if (StringView(args[i]).startsWith('-')) return ctx.usage();
                int    status = Shell::ExitSuccess;
                String message(MemSpace::Psram);
                // Each path absolute and there; sent in messages of the service's size.
                const size_t count = args.size() > 1 ? args.size() - 1 : 1;
                for (size_t i = 0; i < count; ++i) {
                        const StringView arg = args.size() > 1 ? StringView(args[i + 1]) : StringView(".");
                        auto [path, err] = Vfs::normalize(arg, ctx.cwd());
                        if (err.isOk()) err = Vfs::stat(path.view(), ctx.vfs()).error();
                        if (err.isError()) {
                                ctx.eprintf("files: %.*s: %s\n", int(arg.size()), arg.data(),
                                            err == Error::NotExist ? "not found" : CommandContext::errorText(err));
                                status = Shell::ExitFailure;
                                continue;
                        }
                        if (!message.isEmpty() && message.size() + path.size() + 1 > WindowManager::MaxServiceMessage) {
                                if (send(ctx, message.view()) != Shell::ExitSuccess) return Shell::ExitFailure;
                                message.clear();
                        }
                        if (message.append(path.view()).isError() || message.append('\0').isError()) {
                                ctx.eprintf("files: out of memory\n");
                                return Shell::ExitFailure;
                        }
                }
                if (!message.isEmpty() && send(ctx, message.view()) != Shell::ExitSuccess) return Shell::ExitFailure;
                return status;
        }

} // namespace

BMDS_SHELL_COMMAND(files, files, "[path...]", "Open folder windows on the desktop (a file's folder, the file selected)")

BMDS_NAMESPACE_END
