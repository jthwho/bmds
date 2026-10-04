/**
 * @file      loader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdarg>
#include <cstdio>
#include <esp_cache.h>
#include <sdkconfig.h>
#include <bmds/clock.h>
#include <bmds/console.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/list.h>
#include <bmds/loader.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/program.h>
#include <bmds/programimage.h>
#include <bmds/sys.h>
#include <bmds/task.h>
#include <bmds/vfs.h>
#include "running.h"

// Programs run from PSRAM: with XIP from PSRAM, IDF's PMP makes the PSRAM
// heap non-executable (devplan/programs.md §2).
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
#error "BMDS programs need an executable PSRAM heap: turn CONFIG_SPIRAM_XIP_FROM_PSRAM off"
#endif

// The firmware's thread-local storage (from IDF's linker script): it must
// be empty, because a program points tp at its own.
extern "C" char _thread_local_data_start[], _thread_local_bss_end[];

extern "C" const bmds_sys_t *bmds_k_sys_table(void);

BMDS_DEBUG(Loader)

BMDS_NAMESPACE_BEGIN

namespace {

        // Internal RAM left free when a program's stack goes there.
        constexpr size_t InternalReserveBytes = 64 * 1024;

        // A file through the VFS, as the format check reads it.
        class FileSource final : public ProgramImage::Source {
                public:
                        explicit FileSource(FileHandle &file) : _file(file) {}

                        Error readAt(uint32_t offset, WritableByteSpan out) override {
                                auto [pos, err] = _file.stream().seek(offset, Stream::SeekFrom::Start);
                                if (err.isError()) return err;
                                (void)pos;
                                size_t got = 0;
                                while (got < out.size()) {
                                        auto [n, rerr] = _file.read(out.mid(got), WaitForever);
                                        if (rerr.isError()) return rerr;
                                        if (n == 0) return Error::EndOfFile;
                                        got += n;
                                }
                                return Error::Ok;
                        }

                private:
                        FileHandle &_file;
        };

        // Problems go to the program's stderr, as `path: reason`.
        void report(const ProgramLauncher::Request &request, const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

        void report(const ProgramLauncher::Request &request, const char *fmt, ...) {
                char    buf[192];
                int     n = std::snprintf(buf, sizeof(buf), "%.*s: ", static_cast<int>(request.path.size()),
                                          request.path.data());
                va_list args;
                va_start(args, fmt);
                if (n > 0 && static_cast<size_t>(n) < sizeof(buf))
                        n += std::vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), fmt, args);
                va_end(args);
                if (n < 0) return;
                size_t len = static_cast<size_t>(n) < sizeof(buf) - 1 ? static_cast<size_t>(n) : sizeof(buf) - 2;
                buf[len++] = '\n';
                FileHandle *err = request.fds->handle(FdTable::Stderr);
                if (err != nullptr) (void)err->write(ByteSpan(reinterpret_cast<uint8_t *>(buf), len), WaitForever);
                return;
        }

        const char *errorText(Error err) {
                switch (err.code()) {
                        case Error::NotExist: return "No such file or directory";
                        case Error::IsDir: return "Is a directory";
                        case Error::NotDir: return "Not a directory";
                        case Error::NoMem: return "out of memory";
                        case Error::NoMedia: return "No medium found";
                        default: return err.desc();
                }
        }

        // The program's task: point tp's table entry at the Running, call
        // _start, end with what it returns (or with exit(), from inside).
        void programMain(void *arg) {
                Running &run = *static_cast<Running *>(arg);
                vTaskSetThreadLocalStoragePointer(nullptr, RunningTlsIndex, &run);
                using Entry = int32_t (*)(const bmds_sys_t *, int32_t, char **, char **);
                const Entry   entry = reinterpret_cast<Entry>(run.entry);
                const int32_t status =
                        entry(bmds_k_sys_table(), run.process->argc(), run.process->argv(), run.process->envp());
                run.finish(status);
        }

        // Waits for the program to end. The first Ctrl-C is the program's
        // to handle (its calls return EINTR); one more while it runs kills
        // it, outside any system call (see Running).
        void waitOrKill(Running &running, Task &task, Interrupt *interrupt) {
                if (interrupt == nullptr) {
                        (void)running.done.acquire(WaitForever);
                        return;
                }
                const Deadline forever(WaitForever);
                uint32_t       seen = interrupt->raises();
                const uint32_t atStart = seen;
                for (;;) {
                        if (interrupt->wait(running.done, forever, seen).isOk()) return;
                        seen = interrupt->raises();
                        if (seen < 2 || seen == atStart || running.kill.load()) continue;
                        running.kill.store(true);
                        (void)task.suspend();
                        if (!running.inKernel.load()) {
                                running.status = Running::KilledStatus;
                                return;
                        }
                        (void)task.resume(); // it ends itself on leaving the call
                }
        }

} // namespace

Result<int> Loader::run(const Request &request) {
        if (+_thread_local_bss_end != +_thread_local_data_start) {
                report(request, "cannot run programs: the kernel has thread-local storage");
                return makeError<int>(Error::NotSupported);
        }
        // The file.
        const Vfs::Context     context{request.cwd.isEmpty() ? StringView("/") : request.cwd, nullptr};
        const Result<FileInfo> info = Vfs::stat(request.path, context);
        Error                  err = info.error();
        if (err.isOk() && info.value().isDirectory()) err = Error::IsDir;
        if (err.isOk() && info.value().size > UINT32_MAX) err = Error::NotSupported;
        if (err.isError()) {
                report(request, "%s", errorText(err));
                return makeError<int>(err);
        }
        auto [file, oerr] = Vfs::open(request.path, Vfs::Read, context);
        if (oerr.isError()) {
                report(request, "%s", errorText(oerr));
                return makeError<int>(oerr);
        }
        const uint32_t        fileSize = static_cast<uint32_t>(info.value().size);
        FileSource            source(*file);
        ProgramImage::Info    image;
        ProgramImage::Problem problem;
        err = ProgramImage::inspect(source, fileSize, image, problem);
        if (err.isError()) {
                if (problem.what == nullptr) {
                        report(request, "%s", errorText(err));
                } else if (problem.hasValue) {
                        report(request, "cannot execute: %s (0x%x)", problem.what,
                               static_cast<unsigned>(problem.value));
                } else {
                        report(request, "cannot execute: %s", problem.what);
                }
                return makeError<int>(err == Error::Invalid ? Error::NotSupported : err);
        }

        // Its memory: the image, 4096-aligned and whole pages (so the cache
        // sync covers whole lines), then loaded and relocated in place.
        const size_t imageBytes = (image.imageSize + BMDS_LOAD_ALIGN - 1) & ~size_t(BMDS_LOAD_ALIGN - 1);
        auto *memory = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::Psram, imageBytes, BMDS_LOAD_ALIGN));
        if (memory == nullptr) {
                report(request, "out of memory (%u bytes)", static_cast<unsigned>(imageBytes));
                return makeError<int>(Error::NoMem);
        }
        err = ProgramImage::load(source, fileSize, image, memory,
                                 static_cast<uint32_t>(reinterpret_cast<uintptr_t>(memory)), problem);
        file = FileHandle::Ptr();
        if (err.isOk()) {
                // Write the image back from the D-cache and drop stale lines
                // everywhere, including the I-cache (fence.i isn't enough here).
                err = Error::fromEspErr(esp_cache_msync(
                        memory, imageBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
        }
        if (err.isError()) {
                report(request, "cannot load: %s", problem.what != nullptr ? problem.what : errorText(err));
                Memory::free(memory);
                return makeError<int>(err);
        }

        // Its resources and its task. The environment gets the kernel's
        // time zone as TZ unless it has one (picolibc's localtime reads TZ).
        Process          process(*request.fds, request.console, request.interrupt);
        List<StringView> environment;
        String           tz;
        err = process.setCwd(request.cwd.isEmpty() ? StringView("/") : request.cwd);
        if (err.isOk()) err = environment.reserve(request.environment.size() + 1);
        bool hasTz = false;
        for (StringView e : request.environment) {
                hasTz = hasTz || e.startsWith("TZ=");
                if (err.isOk()) err = environment.append(e);
        }
        const TimeZone zone = Clock::timeZone();
        if (err.isOk() && !hasTz && !zone.spec().isEmpty()) {
                err = tz.append("TZ=");
                if (err.isOk()) err = tz.append(zone.spec());
                if (err.isOk()) err = environment.append(tz.view());
        }
        if (err.isOk())
                err = process.setArguments(request.args,
                                           Span<const StringView>(environment.data(), environment.size()));
        if (err.isError()) {
                report(request, "%s", err == Error::NoMem ? "out of memory" : "argument list too long");
                Memory::free(memory);
                return makeError<int>(err);
        }
        Running running;
        running.process = &process;
        running.entry = reinterpret_cast<uintptr_t>(memory) + image.entry;
        Task::Config     config;
        const StringView path = request.path;
        const size_t     slash = path.lastIndexOf('/');
        char             name[configMAX_TASK_NAME_LEN];
        std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(path.size() - slash - 1), path.data() + slash + 1);
        config.name = name;
        config.stackBytes = image.stackSize != 0 ? image.stackSize : BMDS_DEFAULT_STACK_SIZE;
        // Stacks are hot: internal RAM if there is room to spare.
        config.stackSpace =
                Memory::stats(MemSpace::Internal).largestFreeBlock >= config.stackBytes + InternalReserveBytes
                        ? MemSpace::Internal
                        : MemSpace::Psram;
        config.priority = uxTaskPriorityGet(nullptr);
        // The program may change the terminal's modes: the shell gets its own back.
        const Console::TermMode savedMode = request.console != nullptr ? request.console->mode() : Console::TermMode();
        Task                    task;
        err = task.start(config, programMain, &running);
        if (err.isError()) {
                report(request, "cannot start: %s", errorText(err));
                Memory::free(memory);
                return makeError<int>(err);
        }
        bmdsDebug("Loader: %s at %p, %u bytes, stack %u in %s", name, memory, static_cast<unsigned>(imageBytes),
                  static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace));
        waitOrKill(running, task, request.interrupt);
        (void)task.stop();
        process.release();
        if (request.console != nullptr) request.console->setMode(savedMode);
        Memory::free(memory);
        return makeResult(static_cast<int>(running.status & 0xff));
}

BMDS_NAMESPACE_END
