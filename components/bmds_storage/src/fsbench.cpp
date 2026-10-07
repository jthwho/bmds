/**
 * @file      fsbench.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `fsbench`: small files written, rewritten and removed in a directory
 * through the VFS, timed, with the time the whole system was stopped meanwhile
 * (devplan/storage.md §13c, §13d). A measurement tool.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/system.h>
#include <bmds/task.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr size_t   MaxFileBytes = 16384;
        constexpr unsigned SettleMs = 1500; // background writes finished, as part of the cost

        // The rest of the system, as a loop in flash on the other core: it stops whenever the
        // flash does (the caches off), so the counts it loses are the system's pause.
        struct Spinner {
                        volatile uint32_t count = 0;
                        volatile bool     run = true;
                        Task              task;
                        Semaphore         stopped{1, 0};
        };

        void spin(void *arg) {
                auto *s = static_cast<Spinner *>(arg);
                while (s->run) s->count = s->count + 1;
                s->stopped.release();
                for (;;) Task::sleep(1000);
        }

        uint32_t elapsedMs(uint64_t since) {
                return static_cast<uint32_t>((System::uptimeUs() - since) / 1000);
        }

        struct Phase {
                        uint64_t totalUs = 0;
                        uint64_t maxUs = 0;
                        unsigned errors = 0;
        };

        struct Run {
                        const char *dir = nullptr;
                        unsigned    files = 20;
                        size_t      bytes = 200;
                        unsigned    paceMs = 0;
                        uint8_t    *data = nullptr;
        };

        Error writeFile(const Run &run, const char *path, unsigned round, unsigned file) {
                for (size_t i = 0; i < run.bytes; ++i)
                        run.data[i] = static_cast<uint8_t>('a' + (file + round + i) % 26);
                Vfs::Context context;
                auto [handle, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                if (err.isError()) return err;
                auto [n, werr] = handle->write(ByteSpan(run.data, run.bytes), WaitForever);
                if (werr.isOk() && n != run.bytes) werr = Error::IoError;
                return werr; // closed with the handle
        }

        void runPhase(const Run &run, Phase &p, unsigned round, bool remove) {
                for (unsigned i = 0; i < run.files; ++i) {
                        char path[Vfs::MaxMountPath + 32];
                        std::snprintf(path, sizeof(path), "%s/.fsbench/f%03u", run.dir, i);
                        const uint64_t t = System::uptimeUs();
                        Vfs::Context   context;
                        const Error    err = remove ? Vfs::remove(path, context) : writeFile(run, path, round, i);
                        const uint64_t us = System::uptimeUs() - t;
                        p.totalUs += us;
                        if (us > p.maxUs) p.maxUs = us;
                        if (err.isError()) ++p.errors;
                        if (run.paceMs > us / 1000) Task::sleep(run.paceMs - unsigned(us / 1000));
                }
                return;
        }

        void printPhase(CommandContext &ctx, const char *name, const Run &run, const Phase &p) {
                ctx.printf("  %-8s %lu.%03lu ms each on average, %lu.%03lu ms the longest%s\n", name,
                           static_cast<unsigned long>(p.totalUs / run.files / 1000),
                           static_cast<unsigned long>(p.totalUs / run.files % 1000),
                           static_cast<unsigned long>(p.maxUs / 1000), static_cast<unsigned long>(p.maxUs % 1000),
                           p.errors != 0 ? " (errors!)" : "");
                return;
        }

        int runFsBench(CommandContext &ctx) {
                const Args args = ctx.args();
                // fsbench DIR [files [bytes [pace-ms]]]
                if (args.size() < 2 || args.size() > 5) return ctx.usage();
                Run      run;
                unsigned numbers[3] = {20, 200, 0};
                for (size_t i = 2; i < args.size(); ++i) numbers[i - 2] = unsigned(std::strtoul(args[i], nullptr, 10));
                run.dir = args[1];
                run.files = numbers[0];
                run.bytes = numbers[1];
                run.paceMs = numbers[2];
                if (run.files == 0 || run.files > 200 || run.bytes == 0 || run.bytes > MaxFileBytes ||
                    run.paceMs > 5000 || std::strlen(run.dir) > Vfs::MaxMountPath)
                        return ctx.usage();
                char         dir[Vfs::MaxMountPath + 16];
                Vfs::Context context;
                std::snprintf(dir, sizeof(dir), "%s/.fsbench", run.dir);
                const Error made = Vfs::mkdir(dir, context);
                if (made.isError() && made != Error::Exists) {
                        ctx.eprintf("fsbench: %s: %s\n", dir, made.desc());
                        return Shell::ExitFailure;
                }
                run.data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, MaxFileBytes));
                auto *spinner = static_cast<Spinner *>(Memory::alloc(MemSpace::Internal, sizeof(Spinner)));
                if (run.data == nullptr || spinner == nullptr) {
                        Memory::free(run.data);
                        Memory::free(spinner);
                        ctx.eprintf("fsbench: %s\n", Error(Error::NoMem).desc());
                        return Shell::ExitFailure;
                }
                new (spinner) Spinner();
                Task::Config sc;
                sc.name = "fsspin";
                sc.stackBytes = 2048;
                sc.priority = 1;
                sc.core = 1;
                int code = Shell::ExitSuccess;
                if (spinner->task.start(sc, spin, spinner).isOk()) {
                        Task::sleep(SettleMs); // the directory's own writes done before measuring
                        uint32_t start = spinner->count;
                        Task::sleep(500);
                        const uint32_t rate = (spinner->count - start) / 500;
                        Phase          create, rewrite, remove;
                        const uint64_t began = System::uptimeUs();
                        start = spinner->count;
                        runPhase(run, create, 0, false);
                        runPhase(run, rewrite, 1, false);
                        runPhase(run, remove, 0, true);
                        const uint32_t workMs = elapsedMs(began);
                        Task::sleep(SettleMs);
                        const uint32_t windowMs = elapsedMs(began);
                        const uint32_t counted = spinner->count - start;
                        const uint32_t expected = rate * windowMs;
                        const uint32_t lostMs = rate != 0 && expected > counted ? (expected - counted) / rate : 0;
                        ctx.printf("%s: %u files of %u bytes, %u ms apart (system idle: %lu counts/ms)\n", run.dir,
                                   run.files, static_cast<unsigned>(run.bytes), run.paceMs,
                                   static_cast<unsigned long>(rate));
                        printPhase(ctx, "create", run, create);
                        printPhase(ctx, "rewrite", run, rewrite);
                        printPhase(ctx, "remove", run, remove);
                        ctx.printf("  all: %lu ms; the system stopped %lu ms of %lu (%lu.%lu ms per file operation)\n",
                                   static_cast<unsigned long>(workMs), static_cast<unsigned long>(lostMs),
                                   static_cast<unsigned long>(windowMs),
                                   static_cast<unsigned long>(lostMs / (3 * run.files)),
                                   static_cast<unsigned long>(lostMs * 10 / (3 * run.files) % 10));
                        spinner->run = false;
                        (void)spinner->stopped.acquire(WaitForever);
                        (void)spinner->task.stop();
                } else {
                        ctx.eprintf("fsbench: no spinner task\n");
                        code = Shell::ExitFailure;
                }
                (void)Vfs::rmdir(dir, context);
                spinner->~Spinner();
                Memory::free(spinner);
                Memory::free(run.data);
                return code;
        }

} // namespace

BMDS_SHELL_COMMAND(fsbench, runFsBench, "DIR [files [bytes [pace-ms]]]",
                   "Time writing, rewriting and removing small files in DIR, and the system's pause meanwhile")

BMDS_NAMESPACE_END
