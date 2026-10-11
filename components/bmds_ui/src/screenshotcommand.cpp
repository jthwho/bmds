/**
 * @file      screenshotcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `screenshot` command: display 0's picture as a PNG file
 * (devplan/display.md §5d), by default where `screenshot/path` says (devplan/templates.md). Its output format is a userspace interface once
 * shipped: keep the line, add new ones only at the end.
 */

#include <bmds/command.h>
#include <bmds/screenshot.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        int fail(CommandContext &ctx, const char *what, Error err) {
                ctx.eprintf("screenshot: %s: %s\n", what, err.desc());
                return Shell::ExitFailure;
        }

        int runScreenshot(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                int                     level = Png::DefaultLevel;
                size_t                  i = 1;
                if (args.size() >= 2 && args[1][0] == '-' && args[1][1] >= '0' && args[1][1] <= '9' &&
                    args[1][2] == '\0') {
                        level = args[1][1] - '0';
                        i = 2;
                }
                if (args.size() > i + 1) return ctx.usage();
                // Without a path, where `screenshot/path` says, as Print Screen (devplan/templates.md).
                String  path(MemSpace::Psram);
                uint8_t flags = 0;
                Error   err;
                if (args.size() == i + 1) {
                        err = path.append(StringView(args[i]));
                } else {
                        err = Screenshot::configuredPath(path);
                        flags = Screenshot::Unique | Screenshot::MakeDirs;
                }
                if (err.isError()) return fail(ctx, Screenshot::PathKey, err);
                Screenshot::Report report;
                err = Screenshot::save(path, level, flags, ctx.vfs(), report);
                if (err.isError()) return fail(ctx, report.failed != nullptr ? report.failed : path.cStr(), err);
                const Png::Stats &stats = report.stats;
                ctx.printf("screenshot: %s, %ux%u, %lu bytes in %lu ms (copy %lu, read %lu, filter %lu, deflate %lu, "
                           "%s)\n",
                           path.cStr(), report.width, report.height, static_cast<unsigned long>(stats.bytes),
                           static_cast<unsigned long>(report.totalUs / 1000),
                           static_cast<unsigned long>(report.copyUs / 1000),
                           static_cast<unsigned long>(stats.readUs / 1000),
                           static_cast<unsigned long>((stats.filterUs - stats.readUs) / 1000),
                           static_cast<unsigned long>(stats.compressUs / 1000),
                           stats.internalState ? "internal" : "psram");
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(screenshot, runScreenshot, "[-0..-9] [PATH]",
                   "Save display 0's picture as a PNG file, by default where screenshot/path says (-0 stores, "
                   "-1 fastest, -2 the default, -9 smallest)")

BMDS_NAMESPACE_END
