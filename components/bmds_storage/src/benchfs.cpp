/**
 * @file      benchfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The filesystems' benchmarks (devplan/bench.md §3; were `fsbench`): small files written,
 * rewritten and removed through the VFS, with the time the whole system stood still meanwhile
 * (devplan/storage.md §13c, §13d); a file read and written in big chunks; paths looked up.
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


        constexpr BenchParam SequentialParams[] = {
                {"dir", "/tmp", "Where the file goes (DIR/.fsbench.seq, removed after)"},
                {"mb", "4", "Megabytes written and read a run"},
                {"chunk", "65536", "Bytes a write or read"},
        };

        // A file written in chunks (synced, so the card's writes are counted), then read back.
        Error runSequential(BenchContext &ctx) {
                uint64_t         mb = 0, chunk = 0;
                const StringView dir = ctx.param("dir");
                if (ctx.paramUnsigned("mb", mb).isError() || mb == 0 || mb > 256 ||
                    ctx.paramUnsigned("chunk", chunk).isError() || chunk < 512 || chunk > 1024 * 1024 ||
                    dir.isEmpty() || dir.size() > Vfs::MaxMountPath)
                        return Error::Invalid;
                char path[Vfs::MaxMountPath + 24];
                std::snprintf(path, sizeof(path), "%.*s/.fsbench.seq", int(dir.size()), dir.data());
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size_t(chunk)));
                if (buf == nullptr) return Error::NoMem;
                for (size_t i = 0; i < chunk; ++i) buf[i] = uint8_t(i * 7);
                const uint64_t total = mb * 1024 * 1024;
                Error          err = ctx.measure("write_mb_s", "MB/s", BenchBetter::Higher, [&](double &out) {
                        Stopwatch watch;
                        auto [file, oerr] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Truncate, Vfs::Context());
                        if (oerr.isError()) return oerr;
                        for (uint64_t done = 0; done < total; done += chunk) {
                                auto [n, werr] = file->write(ByteSpan(buf, size_t(chunk)), WaitForever);
                                if (werr.isError()) return werr;
                        }
                        file.reset();
                        if (Error serr = Vfs::sync(); serr.isError()) return serr;
                        out = double(total) / double(watch.elapsedUs()) * 1e6 / (1024 * 1024);
                        return Error(Error::Ok);
                });
                if (err.isOk())
                        err = ctx.measure("read_mb_s", "MB/s", BenchBetter::Higher, [&](double &out) {
                                Stopwatch watch;
                                auto [file, oerr] = Vfs::open(path, Vfs::Read, Vfs::Context());
                                if (oerr.isError()) return oerr;
                                uint64_t got = 0;
                                for (;;) {
                                        auto [n, rerr] = file->read(WritableByteSpan(buf, size_t(chunk)), WaitForever);
                                        if (rerr == Error::EndOfFile) break;
                                        if (rerr.isError()) return rerr;
                                        got += n;
                                }
                                if (got != total) return Error(Error::IoError);
                                out = double(total) / double(watch.elapsedUs()) * 1e6 / (1024 * 1024);
                                return Error(Error::Ok);
                        });
                (void)Vfs::remove(path, Vfs::Context());
                Memory::free(buf);
                return err;
        }

        constexpr BenchParam LookupParams[] = {
                {"dir", "/sd0/p0/bin", "A directory with many entries (BusyBox's links)"},
                {"file", "busybox", "A file in it"},
                {"link", "ls", "A link in it"},
                {"missing", "no-such-name", "A name not in it"},
                {"stats", "200", "Lookups a run"},
        };

        // A path's lookups, each through the VFS as stat() makes them (devplan/storage.md §4).
        Error runLookup(BenchContext &ctx) {
                uint64_t stats = 0;
                if (ctx.paramUnsigned("stats", stats).isError() || stats == 0 || stats > 100000)
                        return Error(Error::Invalid);
                static constexpr const char *Names[] = {"file", "link", "missing"};
                static constexpr const char *Metrics[] = {"file_us", "link_us", "missing_us"};
                for (size_t k = 0; k < 3; ++k) {
                        char             path[Vfs::MaxMountPath + 128];
                        const StringView dir = ctx.param("dir"), name = ctx.param(Names[k]);
                        std::snprintf(path, sizeof(path), "%.*s/%.*s", int(dir.size()), dir.data(), int(name.size()),
                                      name.data());
                        const bool exists = k != 2;
                        if (Vfs::stat(path, Vfs::Context()).isOk() != exists) return Error(Error::NotExist);
                        const Error err = ctx.measure(Metrics[k], "us", BenchBetter::Lower, [&](double &out) {
                                Stopwatch watch;
                                for (uint64_t i = 0; i < stats; ++i) (void)Vfs::stat(path, Vfs::Context());
                                out = double(watch.elapsedUs()) / double(stats);
                                return Error(Error::Ok);
                        });
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

} // namespace

BMDS_BENCH_WITH(fsSequential, "fs.sequential", runSequential, BenchQuick | BenchWrites, SequentialParams,
                "A file written in 64 KB chunks and synced, then read back, through the VFS (MB/s)")
BMDS_BENCH_WITH(fsLookup, "fs.lookup", runLookup, BenchQuick, LookupParams,
                "Paths looked up through the VFS in a directory of many entries: a file, a link, a missing name (us)")
BMDS_BENCH_WITH(fsSmallFiles, "fs.smallfiles", runSmallFiles, BenchWrites, SmallFilesParams,
                "Small files made, rewritten and removed through the VFS (ms a file), and the system's pause")

BMDS_NAMESPACE_END
