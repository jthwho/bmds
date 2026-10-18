/**
 * @file      benchfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The filesystems' benchmarks (devplan/bench.md §3; were `fsbench`): small files written,
 * rewritten and removed through the VFS, with the time the whole system stood still meanwhile
 * (devplan/storage.md §13c, §13d).
 */

#include <cstdio>
#include <cstring>
#include <bmds/bench.h>
#include <bmds/memory.h>
#include <bmds/stopwatch.h>
#include <bmds/task.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   MaxFileBytes = 16384;
        constexpr uint64_t MaxFiles = 200;
        constexpr uint64_t MaxPaceMs = 5000;
        constexpr unsigned SettleMs = 1500; // background writes finished, as part of the cost

        constexpr BenchParam SmallFilesParams[] = {
                {"dir", "/tmp", "Where the files go (in DIR/.fsbench, removed after)"},
                {"files", "20", "Files a run"},
                {"bytes", "200", "Bytes a file"},
                {"pace", "0", "Milliseconds between file operations"},
                {"probe", "stall", "How long the whole system stood still (the flash's writes stop it)"},
        };

        struct Run {
                        char     dir[Vfs::MaxMountPath + 16];
                        unsigned files = 20;
                        size_t   bytes = 200;
                        unsigned paceMs = 0;
                        uint8_t *data = nullptr;
        };

        struct Phase {
                        uint64_t totalUs = 0;
                        uint64_t maxUs = 0;
        };

        Error writeFile(const Run &run, const char *path, unsigned round, unsigned file) {
                for (size_t i = 0; i < run.bytes; ++i)
                        run.data[i] = static_cast<uint8_t>('a' + (file + round + i) % 26);
                auto [handle, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Truncate, Vfs::Context());
                if (err.isError()) return err;
                auto [n, werr] = handle->write(ByteSpan(run.data, run.bytes), WaitForever);
                if (werr.isOk() && n != run.bytes) werr = Error::IoError;
                return werr; // closed with the handle
        }

        Error runPhase(const Run &run, Phase &p, unsigned round, bool remove) {
                for (unsigned i = 0; i < run.files; ++i) {
                        char path[sizeof(run.dir) + 16];
                        std::snprintf(path, sizeof(path), "%s/f%03u", run.dir, i);
                        Stopwatch   watch;
                        const Error err = remove ? Vfs::remove(path, Vfs::Context()) : writeFile(run, path, round, i);
                        const uint64_t us = uint64_t(watch.elapsedUs());
                        if (err.isError()) return err;
                        p.totalUs += us;
                        if (us > p.maxUs) p.maxUs = us;
                        if (run.paceMs > us / 1000) Task::sleep(run.paceMs - unsigned(us / 1000));
                }
                return Error::Ok;
        }

        Error runSmallFiles(BenchContext &ctx) {
                Run              run;
                uint64_t         files = 0, bytes = 0, pace = 0;
                const StringView dir = ctx.param("dir");
                if (ctx.paramUnsigned("files", files).isError() || files == 0 || files > MaxFiles ||
                    ctx.paramUnsigned("bytes", bytes).isError() || bytes == 0 || bytes > MaxFileBytes ||
                    ctx.paramUnsigned("pace", pace).isError() || pace > MaxPaceMs || dir.isEmpty() ||
                    dir.size() > Vfs::MaxMountPath)
                        return Error::Invalid;
                run.files = unsigned(files);
                run.bytes = size_t(bytes);
                run.paceMs = unsigned(pace);
                std::snprintf(run.dir, sizeof(run.dir), "%.*s/.fsbench", int(dir.size()), dir.data());
                const Error made = Vfs::mkdir(run.dir, Vfs::Context());
                if (made.isError() && made != Error::Exists) return made;
                run.data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, MaxFileBytes));
                if (run.data == nullptr) {
                        (void)Vfs::rmdir(run.dir, Vfs::Context());
                        return Error::NoMem;
                }
                // Each run: the files made, written again, removed (the warm-up's also takes the
                // directory's own writes); at the end the background writes let finish, which the
                // stall probe counts with the rest.
                const uint32_t runs = ctx.runs(), warmup = ctx.warmup();
                double *samples = static_cast<double *>(Memory::alloc(MemSpace::Psram, 7 * runs * sizeof(double)));
                Error   err = samples == nullptr ? Error(Error::NoMem) : Error(Error::Ok);
                for (uint32_t r = 0; r < warmup + runs && err.isOk(); ++r) {
                        if (ctx.isCancelled()) {
                                err = Error::Cancelled;
                                break;
                        }
                        Phase     create, rewrite, remove;
                        Stopwatch all;
                        err = runPhase(run, create, 0, false);
                        if (err.isOk()) err = runPhase(run, rewrite, 1, false);
                        if (err.isOk()) err = runPhase(run, remove, 0, true);
                        const double allMs = double(all.elapsedUs()) / 1000;
                        if (err.isError() || r < warmup) continue;
                        const uint32_t k = r - warmup;
                        const double   n = double(run.files) * 1000;
                        samples[0 * runs + k] = double(create.totalUs) / n;
                        samples[1 * runs + k] = double(create.maxUs) / 1000;
                        samples[2 * runs + k] = double(rewrite.totalUs) / n;
                        samples[3 * runs + k] = double(rewrite.maxUs) / 1000;
                        samples[4 * runs + k] = double(remove.totalUs) / n;
                        samples[5 * runs + k] = double(remove.maxUs) / 1000;
                        samples[6 * runs + k] = allMs;
                }
                if (err.isOk()) {
                        Task::sleep(SettleMs);
                        static constexpr const char *Metrics[7] = {"create_ms",      "create_max_ms", "rewrite_ms",
                                                                   "rewrite_max_ms", "remove_ms",     "remove_max_ms",
                                                                   "all_ms"};
                        for (int m = 0; m < 7; ++m)
                                ctx.reportSamples(Metrics[m], "ms", BenchBetter::Lower,
                                                  Span<double>(samples + m * runs, runs));
                }
                // Whatever a failed run left, then the directory.
                for (unsigned i = 0; i < run.files; ++i) {
                        char path[sizeof(run.dir) + 16];
                        std::snprintf(path, sizeof(path), "%s/f%03u", run.dir, i);
                        (void)Vfs::remove(path, Vfs::Context());
                }
                (void)Vfs::rmdir(run.dir, Vfs::Context());
                Memory::free(samples);
                Memory::free(run.data);
                return err;
        }

} // namespace

BMDS_BENCH_WITH(fsSmallFiles, "fs.smallfiles", runSmallFiles, BenchWrites, SmallFilesParams,
                "Small files made, rewritten and removed through the VFS (ms a file), and the system's pause")

BMDS_NAMESPACE_END
