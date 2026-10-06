/**
 * @file      screenshotcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `screenshot` command: display 0's picture as a PNG file
 * (devplan/display.md §5d). Its output format is a userspace interface once
 * shipped: keep the line, add new ones only at the end.
 */

#include <cstring>
#include <esp_cache.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/memory.h>
#include <bmds/png.h>
#include <bmds/shell.h>
#include <bmds/surface.h>
#include <bmds/system.h>
#include <bmds/vfs.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned int CaptureTimeoutMs = 200;
        constexpr size_t       SyncChunk = 64 * 1024; // a cache operation runs with interrupts off

        struct Copy {
                        uint8_t *to = nullptr;
        };

        // The shown buffer into the copy (the CPU never writes it: no dirty lines to lose).
        void copyShown(void *arg, const Surface &surface) {
                auto          *copy = static_cast<Copy *>(arg);
                const uint8_t *from = surface.shown();
                const size_t   bytes = surface.stride() * surface.height();
                for (size_t off = 0; off < bytes; off += SyncChunk)
                        (void)esp_cache_msync(const_cast<uint8_t *>(from) + off,
                                              bytes - off < SyncChunk ? bytes - off : SyncChunk,
                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                std::memcpy(copy->to, from, bytes);
                return;
        }

        Error toFile(void *arg, ByteSpan data) {
                auto *file = static_cast<FileHandle *>(arg);
                while (!data.isEmpty()) {
                        auto [n, err] = file->write(data, WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::IoError;
                        data = ByteSpan(data.data() + n, data.size() - n);
                }
                return Error::Ok;
        }

        int fail(CommandContext &ctx, const char *what, Error err) {
                ctx.eprintf("screenshot: %s: %s\n", what, err.desc());
                return Shell::ExitFailure;
        }

        int runScreenshot(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                int                     level = Png::DefaultLevel;
                size_t                  i = 1;
                if (args.size() == 3 && args[1][0] == '-' && args[1][1] >= '0' && args[1][1] <= '9' &&
                    args[1][2] == '\0') {
                        level = args[1][1] - '0';
                        i = 2;
                }
                if (args.size() != i + 1) return ctx.usage();
                const char *path = args[i];

                auto [d, derr] = Display::display(0);
                if (derr.isError()) return fail(ctx, "display 0", derr);
                if (!d->isRunning()) return fail(ctx, "display 0", Error::NotReady);
                Surface       &surface = d->surface();
                const size_t   bytes = surface.stride() * surface.height();
                const uint64_t start = System::uptimeUs();
                Copy           copy;
                copy.to = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes));
                if (copy.to == nullptr) return fail(ctx, "copy", Error::NoMem);
                // The desktop's last round whole; without one, the surface as it is.
                Error err;
                auto [wm, werr] = WindowManager::manager(0);
                if (werr.isOk() && wm->isRunning()) {
                        err = wm->capture(copyShown, &copy, CaptureTimeoutMs);
                } else {
                        copyShown(&copy, surface);
                }
                const uint64_t copied = System::uptimeUs();
                if (err.isError()) {
                        Memory::free(copy.to);
                        return fail(ctx, "capture", err);
                }
                auto [file, ferr] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Truncate, ctx.vfs());
                if (ferr.isError()) {
                        Memory::free(copy.to);
                        return fail(ctx, path, ferr);
                }
                Png::Stats stats;
                err = Png::write(copy.to, surface.width(), surface.height(), surface.stride(), level, toFile, &*file,
                                 &stats);
                Memory::free(copy.to);
                if (err.isError()) return fail(ctx, path, err);
                file.reset(); // closed: written out
                const uint64_t done = System::uptimeUs();
                ctx.printf("screenshot: %s, %ux%u, %lu bytes in %lu ms (copy %lu, read %lu, filter %lu, deflate %lu, "
                           "%s)\n",
                           path, surface.width(), surface.height(), static_cast<unsigned long>(stats.bytes),
                           static_cast<unsigned long>((done - start) / 1000),
                           static_cast<unsigned long>((copied - start) / 1000),
                           static_cast<unsigned long>(stats.readUs / 1000),
                           static_cast<unsigned long>((stats.filterUs - stats.readUs) / 1000),
                           static_cast<unsigned long>(stats.compressUs / 1000),
                           stats.internalState ? "internal" : "psram");
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(screenshot, runScreenshot, "[-0..-9] PATH",
                   "Save display 0's picture as a PNG file (-0 stores, -1 fastest, -2 the default, -9 smallest)")

BMDS_NAMESPACE_END
