/**
 * @file      loader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>
#include <optional>
#include <esp_cache.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <sdkconfig.h>
#include <bmds/audiodevice.h>
#include <bmds/builtinprograms.h>
#include <bmds/clock.h>
#include <bmds/console.h>
#include <bmds/deadline.h>
#include <bmds/errno.h>
#include <bmds/futex.h>
#include <bmds/imagecache.h>
#include <bmds/interrupt.h>
#include <bmds/intervaltimers.h>
#include <bmds/list.h>
#include <bmds/apps.h>
#include <bmds/loader.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/processtable.h>
#include <bmds/profiler.h>
#include <bmds/program.h>
#include <bmds/programimage.h>
#include <bmds/pty.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>
#include <bmds/shebang.h>
#include <bmds/sys.h>
#include <bmds/system.h>
#include <bmds/tasklist.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>
#include "imagewindow.h"
#include "realtimethreads.h"
#include "running.h"
#include "structcopy.h"

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
        // The largest stack internal RAM takes: bigger ones are the deep-recursion budgets (key
        // exchanges, Doom's BSP walk), mostly untouched, and a daemon's held it for good
        // (dropbear's 64 KB from boot, ~10 KB used: devplan/games.md §26).
        constexpr size_t InternalStackMaxBytes = 32 * 1024;

        constinit StaticObject<ProcessTable> processes;

        // What runs the programs a built-in program starts.
        constinit Loader builtinLauncher;

        // Deepest nesting of `#!` interpreters in an exec (the shell's limit).
        constexpr int MaxInterpreterDepth = 4;

        // Ended threads whose tasks wait to be stopped (see queueStop()).
        constinit StaticObject<Mutex> stopLock;
        constinit ProgramThread      *stopQueue = nullptr;

        // Program threads waiting on words (the futex call).
        constinit StaticObject<FutexTable> futexes;

        // Programs read from files, for the next exec (devplan/programs.md §14.1).
        constinit StaticObject<ImageCache> imageCache;

        // The VFS's change hook (storage lock held).
        void fileChanged(uint32_t device, uint64_t id) {
                imageCache->changed(device, id);
                return;
        }

        // Thread ids (but each process's first thread's, its pid): above every pid, below 2^30.
        constexpr int32_t              FirstThreadId = 32768;
        constexpr int32_t              MaxThreadId = (1 << 30) - 1;
        constinit std::atomic<int32_t> lastThreadId{FirstThreadId - 1};

        // Who has a terminal's foreground: held while a program joins or makes a job's group and
        // takes the foreground, and while one that ends gives it back (devplan/processes.md §3.6.1).
        constinit StaticObject<Mutex> groupLock;

        // Held while a process's end is recorded and its parent told (SIGCHLD); a wait that reaps
        // passes through it before returning, so the signal is pending by then, as on Linux.
        constinit StaticObject<Mutex> endLock;

        // Consoles with signals to deliver (their hook queues them; a console may be queued more
        // than once: a later delivery finds its set empty) and the task that delivers them,
        // above the shells' priority so a busy program can't hold a Ctrl-C up.
        constexpr UBaseType_t   SignalQueueLength = 16;
        constexpr size_t        SignalTaskStackBytes = 8192; // it may end programs (close their files)
        constinit QueueHandle_t signalQueue = nullptr;
        constinit Task          signalTask;

        // Interval timers (devplan/programs.md §18): one per process, `SIGALRM` when due, run by
        // the signal task, which waits for its queue until the earliest.
        using ProcessTimers = IntervalTimers<ProcessTable::Capacity>;
        constinit ProcessTimers       timers;    // by pid, on the monotonic clock
        constinit StaticObject<Mutex> timerLock; // timers'

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

        // A program loaded, relocated and synced, not yet running; or a
        // built-in one (no memory).
        struct Image {
                        uint8_t              *memory = nullptr; // where it runs (a mapping's base)
                        size_t                bytes = 0;
                        ImageWindow::Mapping  mapping; // shared pages (§14.2); else a plain copy at memory
                        uint32_t              entry = 0;
                        uint32_t              stackBytes = 0;
                        bool                  display = false;   // BMDS_PROGRAM_DISPLAY
                        const ProgramEntry   *builtin = nullptr; // a program built in: no image
                        char                  path[Loader::PathBytes] = {}; // its file, looked up ("": unknown)
        };

        // Gives @p image @p path, the program file as looked up, if it fits.
        void setImagePath(Image &image, StringView path) {
                if (path.size() < sizeof(image.path)) std::memcpy(image.path, path.data(), path.size());
                return;
        }

        // Why a load failed: the error, and for messages a stage ("cannot
        // execute", "cannot load") and the format check's problem.
        struct LoadFailure {
                        const char           *stage = nullptr;
                        ProgramImage::Problem problem;
        };

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

        Vfs::Context contextFor(StringView cwd) {
                return Vfs::Context{cwd.isEmpty() ? StringView("/") : cwd, nullptr};
        }

        // A program as an exec finds it, looked up once (devplan/programs.md §19): its path with
        // links followed; a program built into the firmware, or a file's cache key and size, and
        // the cache's generation from before it was looked at.
        struct ProgramFile {
                        String              path;
                        const ProgramEntry *builtin = nullptr; // /bin/NAME (devplan/programs.md §17)
                        ImageCache::Key     key;
                        uint32_t            size = 0;
                        uint32_t            generation = 0;
        };

        // Looks up the program at @p path (relative to @p cwd) to run it.
        Error lookUpProgram(StringView path, StringView cwd, ProgramFile &file) {
                file.generation = imageCache->generation();
                auto [found, err] = Vfs::resolve(path, contextFor(cwd));
                if (err.isError()) return err;
                file.path = std::move(found.path);
                file.builtin = found.program;
                if (file.builtin != nullptr) return Error::Ok;
                if (found.info.isDirectory()) return Error::IsDir;
                if (found.info.size > UINT32_MAX) return Error::NotSupported;
                file.key.device = found.info.device;
                file.key.id = found.info.id;
                file.key.size = found.info.size;
                file.key.modified = found.info.modified.ns;
                file.size = static_cast<uint32_t>(found.info.size);
                return Error::Ok;
        }

        // The memory for a program of @p header: 4096-aligned and whole pages (so
        // the cache sync covers whole lines).
        Error allocateImage(const ProgramImage::Info &header, Image &image) {
                image.bytes = (header.imageSize + BMDS_LOAD_ALIGN - 1) & ~size_t(BMDS_LOAD_ALIGN - 1);
                image.memory =
                        static_cast<uint8_t *>(Memory::allocAligned(MemSpace::Psram, image.bytes, BMDS_LOAD_ALIGN));
                return image.memory != nullptr ? Error::Ok : Error::NoMem;
        }

        // Frees a program's image: its mapping, or its plain copy.
        void releaseImage(uint8_t *&memory, ImageWindow::Mapping &mapping) {
                if (mapping.isMapped())
                        ImageWindow::unmap(mapping);
                else
                        Memory::free(memory);
                memory = nullptr;
                return;
        }

        void releaseImage(Image &image) {
                releaseImage(image.memory, image.mapping);
                return;
        }

        // A loaded image (@p err: how loading went) made ready to run, or freed.
        Error finishImage(Error err, const ProgramImage::Info &header, Image &image, LoadFailure &failure) {
                if (err.isOk()) {
                        // Write the image back from the D-cache and drop stale lines
                        // everywhere, including the I-cache (fence.i isn't enough here).
                        err = Error::fromEspErr(
                                esp_cache_msync(image.memory, image.bytes,
                                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
                }
                if (err.isError()) {
                        failure.stage = "cannot load";
                        Memory::free(image.memory); // a plain copy (a mapping is never finished)
                        image.memory = nullptr;
                        return err;
                }
                image.entry = header.entry;
                image.stackBytes = header.stackSize != 0 ? header.stackSize : BMDS_DEFAULT_STACK_SIZE;
                image.display = (header.flags & BMDS_PROGRAM_DISPLAY) != 0;
                return Error::Ok;
        }

        // Places the program of a cache entry: mapped with its shared pages when it has them and
        // the window has room (devplan/programs.md §14.2), else in memory of its own.
        Error placeFrom(ImageCache::Entry &entry, Image &image, LoadFailure &failure) {
                const ProgramImage::Info &header = entry.info();
                if (entry.hasPages() && ImageWindow::isEnabled()) {
                        if (ImageWindow::map(*imageCache, entry, image.mapping).isOk()) {
                                image.memory = image.mapping.base;
                                image.bytes = size_t(image.mapping.pages) * ProgramImage::SharePageBytes;
                                image.entry = header.entry;
                                image.stackBytes = header.stackSize != 0 ? header.stackSize : BMDS_DEFAULT_STACK_SIZE;
                                image.display = (header.flags & BMDS_PROGRAM_DISPLAY) != 0;
                                return Error::Ok;
                        }
                        ImageWindow::countFallback(); // no room: a plain copy
                }
                Error err = allocateImage(header, image);
                if (err.isError()) return err;
                ProgramImage::place(header, entry.bytes(), entry.patches(), image.memory,
                                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(image.memory)));
                return finishImage(Error::Ok, header, image, failure);
        }

        // Loads the program in @p file (@p program, unchanged since it was
        // looked at): read into a cache entry once and placed from it, the
        // entry then kept; straight from the file without memory for one.
        Error loadFrom(FileHandle &file, const ProgramFile &program, Image &image, LoadFailure &failure) {
                FileSource         source(file);
                ProgramImage::Info header;
                Error              err;
                err = ProgramImage::inspect(source, program.size, header, failure.problem);
                if (err.isError()) {
                        if (failure.problem.what != nullptr) failure.stage = "cannot execute";
                        return err == Error::Invalid ? Error::NotSupported : err;
                }
                if (ImageCache::Entry *entry = imageCache->create(program.key, header, program.generation)) {
                        err = ProgramImage::read(source, program.size, header, entry->bytes(), entry->patches(),
                                                 failure.problem);
                        if (err.isError()) {
                                imageCache->release(entry);
                                failure.stage = "cannot load";
                                return err;
                        }
                        // Its shared pages are mapped as they are: in PSRAM before any is.
                        for (uint32_t p = 0; entry->hasPages() && p < ProgramImage::pageCount(header.imageSize); ++p) {
                                if (ProgramImage::isSharedPage(header, p))
                                        (void)esp_cache_msync(entry->bytes() + size_t(p) * ProgramImage::SharePageBytes,
                                                              ProgramImage::SharePageBytes,
                                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        }
                        err = placeFrom(*entry, image, failure);
                        // The entry is good either way; but without memory for the
                        // image, it may be what is in the way (below).
                        if (err != Error::NoMem) {
                                (void)imageCache->keep(entry);
                                return err;
                        }
                        imageCache->release(entry);
                }
                err = allocateImage(header, image);
                if (err.isError()) return err;
                err = ProgramImage::load(source, program.size, header, image.memory,
                                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(image.memory)),
                                         failure.problem);
                return finishImage(err, header, image, failure);
        }

        // An exec of @p program that the cache can serve: true with @p err set if it did.
        bool loadCached(const ProgramFile &program, Image &image, LoadFailure &failure, Error &err) {
                ImageCache::Entry *entry = imageCache->acquire(program.key);
                if (entry == nullptr) return false;
                err = placeFrom(*entry, image, failure);
                imageCache->release(entry);
                return true;
        }

        // An application's folder (devplan/apps.md §3, §11): BMDS_APP_DIR names it to a program in
        // one, and BMDS_APP_ID its id.
        constexpr StringView AppDirVariable("BMDS_APP_DIR=");
        constexpr StringView AppIdVariable("BMDS_APP_ID=");

        bool isAppVariable(StringView e) {
                return e.startsWith(AppDirVariable) || e.startsWith(AppIdVariable);
        }

        // "BMDS_APP_DIR=FOLDER" into @p dir and "BMDS_APP_ID=ID" into @p id if @p program
        // (links followed: /apps's too) is in a NAME.app folder; else both are left empty. The id
        // is the installed application's (Apps), else the folder's name without .app (a folder
        // outside apps/path), if that is a valid one.
        Error appVariablesFor(const ProgramFile &program, String &dir, String &id) {
                const StringView r = program.path.view();
                const size_t     slash = r.lastIndexOf('/');
                if (slash == StringView::NPos || slash == 0) return Error::Ok;
                const StringView folder = r.left(slash);
                const StringView name = folder.mid(folder.lastIndexOf('/') + 1);
                if (!Apps::isFolderName(name)) return Error::Ok;
                Error e = dir.append(AppDirVariable);
                if (e.isOk()) e = dir.append(folder);
                char       installed[DesktopEntry::MaxId + 1] = {};
                Apps      *apps = Apps::system();
                StringView appId = name.left(name.size() - Apps::Extension.size());
                if (apps != nullptr && apps->idOf(folder, installed)) appId = StringView(installed);
                if (e.isOk() && Apps::isValidName(appId)) {
                        e = id.append(AppIdVariable);
                        if (e.isOk()) e = id.append(appId);
                }
                return e;
        }

        // Loads @p program: a file, or a program built into the firmware.
        Error loadImage(const ProgramFile &program, Image &image, LoadFailure &failure) {
                if (program.builtin != nullptr) {
                        image.builtin = program.builtin;
                        image.stackBytes = static_cast<uint32_t>(program.builtin->program.stackBytes);
                        return Error::Ok;
                }
                Error err;
                if (loadCached(program, image, failure, err)) return err;
                auto [file, oerr] = Vfs::open(program.path, Vfs::Read, contextFor("/"));
                return oerr.isError() ? oerr : loadFrom(*file, program, image, failure);
        }

        // @p args with @p front in place of its first element.
        Error withFront(List<String> &args, std::initializer_list<StringView> front) {
                List<String> out;
                Error        err = out.reserve(args.size() + front.size());
                for (StringView f : front) {
                        if (err.isError()) break;
                        auto [s, serr] = String::fromView(f);
                        err = serr.isOk() ? out.append(std::move(s)) : serr;
                }
                for (size_t i = 1; err.isOk() && i < args.size(); ++i) err = out.append(std::move(args[i]));
                if (err.isOk()) args = std::move(out);
                return err;
        }

        // What runs @p path for an exec (devplan/programs.md §13, 8b, §17): a
        // program built into the firmware (`/bin/NAME`) or in a file; a `#!`
        // script's interpreter as named (a file or a built-in: `#!/bin/sh`,
        // `#!/bin/env name`; a path that doesn't exist fails), in front of the
        // script in @p args; the shell for a `.sh` file. @p program is @p path looked up
        // already, or null.
        Error resolveExec(const ProgramFile *program, StringView path, StringView cwd, Span<const StringView> env,
                          List<String> &args, Image &image, LoadFailure &failure, int depth) {
                if (depth > MaxInterpreterDepth) return Error::TooManyLinks;
                ProgramFile looked;
                if (program == nullptr) {
                        const Error err = lookUpProgram(path, cwd, looked);
                        if (err.isError()) return err;
                        program = &looked;
                }
                // A program built into the firmware: `/bin/NAME`, or a link to one (devplan/programs.md §17).
                if (program->builtin != nullptr) return loadImage(*program, image, failure);
                // A program cached: it was an ELF file (scripts aren't cached).
                Error err;
                if (loadCached(*program, image, failure, err)) return err;
                auto [file, oerr] = Vfs::open(program->path, Vfs::Read, contextFor("/"));
                if (oerr.isError()) return oerr;
                char   head[Shebang::HeadBytes];
                size_t got = 0;
                while (got < sizeof(head)) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(head) + got, sizeof(head) - got),
                                WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                        if (rerr.isError()) return rerr;
                        got += n;
                }
                const StringView headView(head, got);
                if (headView.startsWith(StringView("\x7f"
                                                   "ELF")))
                        return loadFrom(*file, *program, image, failure);
                file = FileHandle::Ptr();
                if (Shebang::isScript(headView)) {
                        Shebang line;
                        if (!Shebang::parse(headView, program->size <= got, line) || line.interpreter.isEmpty())
                                return Error::NotSupported;
                        // Copies: the views point into head, and path may be an element of args.
                        auto [script, cerr] = String::fromView(path);
                        auto [interp, ierr] = String::fromView(line.interpreter);
                        auto [arg, aerr] = String::fromView(line.argument);
                        if (cerr.isError() || ierr.isError() || aerr.isError()) return Error::NoMem;
                        err = arg.isEmpty() ? withFront(args, {interp.view(), script.view()})
                                            : withFront(args, {interp.view(), arg.view(), script.view()});
                        if (err.isError()) return err;
                        // The interpreter as named: a file or a built-in (/bin/sh, /bin/env); nothing guessed.
                        return resolveExec(nullptr, interp.view(), cwd, env, args, image, failure, depth + 1);
                }
                if (path.endsWith(StringView(".sh"))) {
                        auto [script, cerr] = String::fromView(path);
                        if (cerr.isError()) return Error::NoMem;
                        err = withFront(args, {StringView("sh"), script.view()});
                        if (err.isError()) return err;
                        return resolveExec(nullptr, StringView("/bin/sh"), cwd, env, args, image, failure, depth + 1);
                }
                return Error::NotSupported;
        }

        // A program thread's task: its owner is its record, its interrupt its part of the signals.
        void enterTask(ProgramThread &thread) {
                TaskOwner::setCurrent(&thread);
                Interrupt::setCurrent(&thread.signals->interrupt);
                RealTimeThreads::started(thread);
                return;
        }

        // The program's first task: call _start; returning from it is exit() (devplan/programs.md §12).
        void programMain(void *arg) {
                Running       &run = *static_cast<Running *>(arg);
                ProgramThread &thread = run.main;
                enterTask(thread);
                thread.leave(); // stopped or killed before it ran; then into program code
                using Entry = int32_t (*)(const bmds_sys_t *, int32_t, char **, char **);
                const Entry   entry = reinterpret_cast<Entry>(run.entry);
                const int32_t status =
                        entry(bmds_k_sys_table(), run.process->argc(), run.process->argv(), run.process->envp());
                thread.endProcess(ProcessTable::exitStatus(status));
        }

        // A thread the program started (thread_start): its entry with its thread pointer.
        void threadMain(void *arg) {
                ProgramThread &thread = *static_cast<ProgramThread *>(arg);
                enterTask(thread);
                thread.leave();
                // The kernel has no thread-local storage (Loader::begin() checks): tp is the program's.
                __asm__ volatile("mv tp, %0" : : "r"(thread.tls));
                const int32_t status = thread.entry(thread.arg);
                thread.exitThread(status, nullptr);
        }

        // A built-in program's task (the shell as /bin/sh): all of it is
        // kernel code, so a signal that ends it leaves it to end itself (its
        // interrupt stops the shell).
        void builtinMain(void *arg) {
                Running       &run = *static_cast<Running *>(arg);
                ProgramThread &thread = run.main;
                enterTask(thread);
                thread.signals->interrupt.park(); // stopped before it ran
                int status = 0;
                {
                        Process         &p = *run.process;
                        List<StringView> args;
                        List<StringView> env;
                        Error            err = Error::Ok;
                        for (char **a = p.argv(); err.isOk() && a != nullptr && *a != nullptr; ++a)
                                err = args.append(StringView(*a, std::strlen(*a)));
                        for (char **e = p.envp(); err.isOk() && e != nullptr && *e != nullptr; ++e)
                                err = env.append(StringView(*e, std::strlen(*e)));
                        BuiltinProgram::Context context;
                        context.fds = &p.fds();
                        context.cwd = p.cwd();
                        context.args = Span<const StringView>(args.data(), args.size());
                        context.environment = Span<const StringView>(env.data(), env.size());
                        context.console = p.console();
                        context.interrupt = &p.signals().interrupt();
                        context.launcher = &builtinLauncher;
                        context.program = run.builtin->name;
                        context.process = &run;
                        context.data = run.data;
                        status = err.isOk() ? run.builtin->program.main(context) : 126;
                }
                if (run.killed.load()) thread.finish(run.killStatus.load());
                const int ending = run.process->signals().terminateSignal();
                thread.finish(ending != 0 ? ProcessTable::killedStatus(ending) : ProcessTable::exitStatus(status));
        }

        // A Running for @p image and @p process (owned once it starts), not yet started.
        Running *createRunning(const Image &image, Process *process, int32_t pid, StringView path, StringView cwd) {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(Running));
                if (memory == nullptr) return nullptr;
                auto *run = new (memory) Running;
                run->process = run->main.current = process;
                run->pid = run->main.currentPid = run->main.tid = pid;
                run->main.signals = &process->signals().mainThread();
                run->image = image.memory;
                run->imageBytes = image.bytes;
                run->mapping = image.mapping;
                run->builtin = image.builtin;
                // Kernel code until it reaches the program's: a kill before then leaves it to end
                // itself (a built-in program always gets its data: devplan/processes.md §3.8.1).
                run->main.inKernel.store(true);
                run->entry = image.builtin != nullptr ? 0 : reinterpret_cast<uintptr_t>(image.memory) + image.entry;
                // Its command line for ps, cut to fit.
                size_t used = 0;
                for (char **a = process->argv(); a != nullptr && *a != nullptr; ++a) {
                        const int n = std::snprintf(run->command + used, sizeof(run->command) - used, "%s%s",
                                                    used > 0 ? " " : "", *a);
                        if (n < 0 || used + size_t(n) >= sizeof(run->command)) break;
                        used += size_t(n);
                }
                // Its program file, whole (where its icons are: a surface's window shows them): as
                // looked up (programs.md §19), else the path normalized.
                if (image.path[0] != '\0') {
                        std::memcpy(run->path, image.path, sizeof(run->path));
                } else {
                        auto [full, ferr] = Vfs::normalize(path, cwd);
                        if (ferr.isOk() && full.view().size() < sizeof(run->path))
                                std::memcpy(run->path, full.view().data(), full.view().size());
                }
                return run;
        }

        void destroyRunning(Running *run) {
                run->releaseStats(); // an exec that failed to start, say
                run->~Running();
                Memory::free(run);
                return;
        }

        void destroyThread(ProgramThread *thread) {
                thread->~ProgramThread();
                Memory::free(thread);
                return;
        }

        // Where a program's task's stack goes. Stacks are hot: internal RAM if there is room to
        // spare and it is no bigger than InternalStackMaxBytes. Not a built-in's (the shell's
        // recursion budget is large).
        MemSpace stackSpaceFor(size_t bytes, bool builtin) {
                return !builtin && bytes <= InternalStackMaxBytes &&
                                       Memory::stats(MemSpace::Internal).largestFreeBlock >=
                                               bytes + InternalReserveBytes
                               ? MemSpace::Internal
                               : MemSpace::Psram;
        }

        // The core the display's own work runs on: the surfaces' presenters and the scanout
        // (ProgramSurface, Scanout).
        constexpr int DisplayCore = 1;

        // The core a program's first task keeps to (devplan/processes.md §3.12a): one that draws
        // to the screen continuously (@p display, BMDS_PROGRAM_DISPLAY) always the one the
        // display's work isn't on (its frames converted there in parallel: Doom 53-58 frames a
        // second against 44 sharing, devplan/bench.md §17). Any other, the one less busy over the
        // last window of a second or more ending at a program's start (the idle tasks' run time;
        // shorter gaps keep the last estimate: every shell command is a program), ties (within 15
        // points) to core 1 (core 0 has the window manager, audio and the network). Its other
        // threads may choose (BMDS_SCHED_PIN); the game library puts its audio on the other core.
        // A task is pinned by its first floating point anyway: this chooses where, instead of
        // wherever it happens to run then.
        int coreForProgram(bool display) {
                static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
                static uint64_t     lastUs = 0, lastIdle[portNUM_PROCESSORS] = {};
                static uint32_t     busyPercent[portNUM_PROCESSORS] = {};
                const uint64_t      now = uint64_t(System::uptimeUs());
                uint64_t            idle[portNUM_PROCESSORS];
                for (int c = 0; c < portNUM_PROCESSORS; ++c) idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
                int core = 1;
                portENTER_CRITICAL(&lock);
                const uint64_t span = now - lastUs;
                if (lastUs != 0 && span >= 1000000) { // else the last estimate (a second at least)
                        for (int c = 0; c < portNUM_PROCESSORS; ++c) {
                                const uint64_t idleUs = idle[c] - lastIdle[c];
                                busyPercent[c] = idleUs >= span ? 0 : uint32_t((span - idleUs) * 100 / span);
                        }
                }
                if (lastUs == 0 || span >= 1000000) {
                        lastUs = now;
                        for (int c = 0; c < portNUM_PROCESSORS; ++c) lastIdle[c] = idle[c];
                }
                if (busyPercent[1] > busyPercent[0] + 15) core = 0;
                portEXIT_CRITICAL(&lock);
                return display ? 1 - DisplayCore : core;
        }

        // Starts @p run's first task, named after @p path's last component, on the core for
        // @p display (coreForProgram()), stored in @p core; @p exec: it goes on from the calling
        // thread (an exec in place), whose scheduling it keeps.
        Error startRunning(Running &run, StringView path, size_t stackBytes, bool display, bool exec, int &core) {
                Task::Config config;
                const size_t slash = path.lastIndexOf('/');
                char         name[configMAX_TASK_NAME_LEN];
                std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(path.size() - slash - 1),
                              path.data() + slash + 1);
                config.name = name;
                config.stackBytes = stackBytes;
                config.stackSpace = stackSpaceFor(config.stackBytes, run.builtin != nullptr);
                // Its starter's priority; the shells' for one the kernel starts (pid 1). A program
                // thread's normal one, and its policy (devplan/processes.md §3.12).
                const ProgramThread *starter = ProgramThread::ofCurrentTask();
                const unsigned       normal = starter != nullptr                    ? starter->normalPriority
                                              : Running::ofCurrentTask() != nullptr ? uxTaskPriorityGet(nullptr)
                                                                                    : CONFIG_BMDS_SHELL_PRIORITY;
                config.priority = RealTimeThreads::inherit(run.main, starter, normal, exec);
                config.core = core = coreForProgram(display);
                const Error err = run.main.task.start(config, run.builtin != nullptr ? builtinMain : programMain, &run);
                if (err.isOk())
                        bmdsDebug("Loader: %s (pid %d) at %p, stack %u in %s", name, static_cast<int>(run.pid),
                                  run.image, static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace));
                return err;
        }

        // Stops an ended thread's task and frees its record; the Running goes with its last one.
        void reap(ProgramThread &thread) {
                // An audio device may change a raised thread's priority until it forgets it.
                if (thread.realtime) AudioDevice::forgetTask(thread.task.handle());
                RealTimeThreads::forget(thread);
                (void)thread.task.stop();
                Running &run = *thread.run;
                bool     last = false;
                {
                        Mutex::Locker locker(run.threadsLock, WaitForever);
                        last = --run.records == 0;
                }
                if (&thread != &run.main) destroyThread(&thread);
                if (last) destroyRunning(&run);
                return;
        }

        // Stops the queued tasks and frees their records: from the timer
        // service task, and at every start (in case its queue was full).
        void drainStops() {
                for (;;) {
                        ProgramThread *thread = nullptr;
                        {
                                Mutex::Locker locker(*stopLock, WaitForever);
                                thread = stopQueue;
                                if (thread != nullptr) stopQueue = thread->nextStop;
                        }
                        if (thread == nullptr) return;
                        reap(*thread);
                }
        }

        void drainStopsLater(void *, uint32_t) {
                drainStops();
                return;
        }

        // Queues the calling thread's task to be stopped; it then suspends
        // itself (or is suspended by the stop, whichever comes first).
        [[noreturn]] void queueStop(ProgramThread &thread) {
                {
                        Mutex::Locker locker(*stopLock, WaitForever);
                        thread.nextStop = stopQueue;
                        stopQueue = &thread;
                }
                (void)xTimerPendFunctionCall(drainStopsLater, nullptr, 0, 0);
                for (;;) vTaskSuspend(nullptr);
        }

        // The next thread id: above every pid, below 2^30 (musl keeps owners' ids in lock words).
        int32_t newThreadId() {
                int32_t last = lastThreadId.load();
                int32_t next = 0;
                do {
                        next = last >= MaxThreadId ? FirstThreadId : last + 1;
                } while (!lastThreadId.compare_exchange_weak(last, next));
                return next;
        }

        void tellParent(int32_t parent);

        // Records that @p pid stopped (by @p sig) or continued (0), and tells its parent.
        void recordChange(int32_t pid, int sig) {
                const int32_t parent = processes->setStopped(pid, sig);
                if (parent > 0) tellParent(parent);
                return;
        }

        // Sends signals (programs.md §15). Its calls run with the process table locked, and take
        // the process's thread lock; threads caught in program code by a kill are ended, and
        // stops and continues recorded, when the Sender goes, after the locks.
        class Sender {
                public:
                        Sender() : _self(ProgramThread::ofCurrentTask()) {}
                        ~Sender() {
                                endFrozen();
                                for (int i = 0; i < _changeCount; ++i) recordChange(_changes[i].pid, _changes[i].sig);
                        }
                        Sender(const Sender &) = delete;
                        Sender &operator=(const Sender &) = delete;

                        // Sends @p sig to process @p pid of @p run; false if it had no
                        // effect (dropped, or not there yet).
                        bool post(Running &run, int32_t pid, int sig, int code) {
                                Mutex::Locker threads(run.threadsLock, WaitForever);
                                Process      *p = processOf(run, pid);
                                if (p == nullptr) return false;
                                Signals &s = p->signals();
                                // Pid 1 gets only what it set an action for (as Linux's init).
                                if (pid == ProcessTable::InitPid && s.action(sig) == Signals::Action::Default)
                                        return false;
                                return act(run, *p, pid, sig, s.post(sig, code));
                        }

                        // Sends @p sig (0: none) to thread @p tid of @p run, and with @p wake ends the
                        // call it is blocked in: 0, or -BMDS_ESRCH.
                        int32_t postToThread(Running &run, int32_t tid, int sig, bool wake) {
                                Mutex::Locker  threads(run.threadsLock, WaitForever);
                                ProgramThread *to = nullptr;
                                if (!run.main.ended && run.main.tid == tid) to = &run.main;
                                for (ProgramThread *t = run.others; to == nullptr && t != nullptr; t = t->next) {
                                        if (t->tid == tid) to = t;
                                }
                                if (to == nullptr || run.process == nullptr) return -BMDS_ESRCH;
                                Signals &s = run.process->signals();
                                if (wake) s.wake(*to->signals);
                                if (sig != 0)
                                        (void)act(run, *run.process, run.pid, sig,
                                                  s.postTo(*to->signals, sig, BMDS_SI_USER));
                                return 0;
                        }

                        // Ends every thread of @p run with @p status (the first kill's status stays):
                        // @p hard (SIGKILL) also those running a vfork child.
                        void killTask(Running &run, int32_t status, bool hard) {
                                Mutex::Locker threads(run.threadsLock, WaitForever);
                                kill(run, status, hard);
                                return;
                        }

                        // Ends @p run's threads but the caller (an exec): they end without ending the process.
                        void killOthers(Running &run) {
                                Mutex::Locker threads(run.threadsLock, WaitForever);
                                forEach(run, [&](ProgramThread &t) {
                                        if (&t != _self) handshake(t);
                                });
                                return;
                        }

                private:
                        struct Change {
                                        int32_t pid;
                                        int     sig; // 0: continued
                        };

                        // Calls @p fn with each of @p run's threads not yet ended.
                        template <typename Fn> static void forEach(Running &run, Fn &&fn) {
                                if (!run.main.ended) fn(run.main);
                                for (ProgramThread *t = run.others; t != nullptr; t = t->next) fn(*t);
                        }

                        // As Running::processOf(), with the thread lock held.
                        static Process *processOf(Running &run, int32_t pid) {
                                if (pid == run.pid) return run.process;
                                Process *found = nullptr;
                                forEach(run, [&](ProgramThread &t) {
                                        for (int i = 0; i < t.depth; ++i) {
                                                if (t.children[i].pid == pid) found = t.children[i].process;
                                        }
                                });
                                return found;
                        }

                        // What a signal that was @p posted to @p p (process @p pid of @p run) does.
                        bool act(Running &run, Process &p, int32_t pid, int sig, Signals::Posted posted) {
                                if (posted == Signals::Posted::Dropped) return false;
                                Signals &s = p.signals();
                                if (posted == Signals::Posted::Stop) stopThreads(run, p, pid, sig);
                                if (posted == Signals::Posted::Continue) continueThreads(run, p, pid);
                                // A vfork child (or its parent while it runs) ends through the SDK;
                                // SIGKILL takes the whole task. A continue may let a signal that
                                // came while it was stopped end it.
                                const bool ends = posted == Signals::Posted::Terminate ||
                                                  (posted == Signals::Posted::Continue && s.terminateSignal() != 0);
                                if (ends && &p == run.process) {
                                        const int ending = sig == BMDS_SIGKILL ? sig : s.terminateSignal();
                                        kill(run, ProcessTable::killedStatus(ending), sig == BMDS_SIGKILL);
                                }
                                // Every thread it is due for takes it at its next call.
                                forEach(run, [&](ProgramThread &t) {
                                        if (t.current == &p && t.hint != nullptr && s.isDue(t.currentSignals()))
                                                *t.hint = 1;
                                });
                                return true;
                        }

                        void kill(Running &run, int32_t status, bool hard) {
                                bool first = false;
                                if (run.killed.compare_exchange_strong(first, true)) run.killStatus.store(status);
                                forEach(run, [&](ProgramThread &t) {
                                        // Running a vfork child, it ends once back in the process (leave()).
                                        if (t.depth > 0 && !hard) return;
                                        handshake(t);
                                });
                                return;
                        }

                        // Ends thread @p t: in program code it is ended once the locks are gone;
                        // in a call it ends as the call returns. Its interrupt is raised (an
                        // exit() or an exec has no signal that would).
                        void handshake(ProgramThread &t) {
                                if (t.kill.exchange(true)) return; // ended, or being ended, already
                                if (&t == _self) return;           // it ends as its call returns
                                t.signals->interrupt.raise();
                                (void)t.task.suspend();
                                if (t.inKernel.load()) {
                                        if (t.depth > 0) t.current->signals().interrupt().raise();
                                        (void)t.task.resume();
                                } else {
                                        t.nextFrozen = _frozen;
                                        _frozen = &t;
                                }
                                return;
                        }

                        // Process @p pid (@p p) of @p run stopped (its threads' interrupts are held):
                        // a thread in program code is suspended there, one in a call parks.
                        void stopThreads(Running &run, Process &p, int32_t pid, int sig) {
                                noteChange(pid, sig);
                                forEach(run, [&](ProgramThread &t) {
                                        if (t.current != &p || &t == _self || t.kill.load() || t.suspendedForStop)
                                                return;
                                        (void)t.task.suspend();
                                        if (t.inKernel.load()) {
                                                (void)t.task.resume();
                                        } else {
                                                t.suspendedForStop = true;
                                        }
                                });
                                return;
                        }

                        // Process @p pid (@p p) of @p run continued (the holds are released).
                        void continueThreads(Running &run, Process &p, int32_t pid) {
                                noteChange(pid, 0);
                                forEach(run, [&](ProgramThread &t) {
                                        if (t.current != &p || !t.suspendedForStop) return;
                                        t.suspendedForStop = false;
                                        (void)t.task.resume();
                                });
                                return;
                        }

                        void noteChange(int32_t pid, int sig) {
                                if (_changeCount < ProcessTable::Capacity) _changes[_changeCount++] = Change{pid, sig};
                                return;
                        }

                        // Suspended in program code: nothing else touches them now.
                        void endFrozen() {
                                while (_frozen != nullptr) {
                                        ProgramThread &t = *_frozen;
                                        _frozen = t.nextFrozen;
                                        Running      &run = *t.run;
                                        const int32_t status = run.killStatus.load();
                                        t.endChildren(status);
                                        run.threadEnded(t, status);
                                        reap(t);
                                }
                                return;
                        }

                        ProgramThread *_self;
                        ProgramThread *_frozen = nullptr;
                        Change         _changes[ProcessTable::Capacity];
                        int            _changeCount = 0;
        };

        // Sends @p sig to process @p pid from the kernel.
        void signalProcess(int32_t pid, int sig) {
                Sender sender;
                (void)processes->withProcess(pid, [&](void *runtime) {
                        if (runtime != nullptr)
                                (void)sender.post(*static_cast<Running *>(runtime), pid, sig, BMDS_SI_KERNEL);
                });
                return;
        }

        // A child of @p parent ended, stopped or continued: SIGCHLD.
        void tellParent(int32_t parent) {
                signalProcess(parent, BMDS_SIGCHLD);
                return;
        }

        // Sends @p sig to every process of group @p group; false if none took it.
        bool signalGroup(int32_t group, int sig, int code) {
                Sender sender;
                bool   taken = false;
                processes->forEachInGroup(group, [&](int32_t pid, void *runtime) {
                        if (runtime != nullptr && sender.post(*static_cast<Running *>(runtime), pid, sig, code))
                                taken = true;
                });
                return taken;
        }

        // Returns true if a process of group @p group has @p sig pending.
        bool groupHasPending(int32_t group, int sig) {
                bool pending = false;
                processes->forEachInGroup(group, [&](int32_t pid, void *runtime) {
                        Process *p = runtime != nullptr ? static_cast<Running *>(runtime)->processOf(pid) : nullptr;
                        if (p != nullptr && (p->signals().pending() & Signals::bit(sig)) != 0) pending = true;
                });
                return pending;
        }

        // The console's signals, to its foreground group. A hangup goes to the session's leader (the
        // shell, which hangs up its own jobs) and the foreground group, as Linux's: a job its shell
        // disowned, or one run by nohup or setsid, lives on. A Ctrl-C after one no process took, or
        // that is still pending, ends the group (SIGKILL).
        void deliverConsoleSignals(Console &console) {
                const uint32_t set = console.takeSignals();
                for (int sig = 1; sig < Signals::Count; ++sig) {
                        if ((set & Signals::bit(sig)) == 0) continue;
                        if (sig == BMDS_SIGHUP) {
                                // Continued after it, or a stopped process would never see it.
                                const int32_t session = console.session();
                                const int32_t foreground = console.foreground();
                                if (session > 0) {
                                        signalProcess(session, BMDS_SIGHUP);
                                        signalProcess(session, BMDS_SIGCONT);
                                }
                                if (foreground > 0 && foreground != session) {
                                        (void)signalGroup(foreground, BMDS_SIGHUP, BMDS_SI_KERNEL);
                                        (void)signalGroup(foreground, BMDS_SIGCONT, BMDS_SI_KERNEL);
                                }
                                continue;
                        }
                        const int32_t group = console.foreground();
                        if (group <= 0) continue;
                        if (sig != BMDS_SIGINT) {
                                (void)signalGroup(group, sig, BMDS_SI_KERNEL);
                                continue;
                        }
                        if (console.unansweredGroup() == group || groupHasPending(group, BMDS_SIGINT)) {
                                (void)signalGroup(group, BMDS_SIGKILL, BMDS_SI_KERNEL);
                                console.setUnansweredGroup(0);
                        } else {
                                const bool taken = signalGroup(group, BMDS_SIGINT, BMDS_SI_KERNEL);
                                console.setUnansweredGroup(taken ? 0 : group);
                        }
                }
                return;
        }

        // Wakes the signal task to look at the timers again (a full queue wakes it anyway).
        void timersChanged() {
                Console *none = nullptr;
                (void)xQueueSend(signalQueue, &none, 0);
                return;
        }

        // Process @p pid ended: its timer goes, before the pid can be used again.
        void dropTimer(int32_t pid) {
                Mutex::Locker locker(*timerLock, WaitForever);
                timers.drop(pid);
                return;
        }

        // Sends SIGALRM for the timers due now; returns the next deadline, or Never.
        int64_t runTimers() {
                int32_t due[ProcessTable::Capacity];
                size_t  dueCount = 0;
                int64_t next = ProcessTimers::Never;
                {
                        Mutex::Locker locker(*timerLock, WaitForever);
                        dueCount = timers.takeDue(Clock::monotonicNs(), due, next);
                }
                for (size_t i = 0; i < dueCount; ++i) signalProcess(due[i], BMDS_SIGALRM);
                return next;
        }

        void signalTaskMain(void *) {
                constexpr int64_t TickNs = 1000000000 / configTICK_RATE_HZ;
                for (;;) {
                        const int64_t next = runTimers();
                        TickType_t    wait = portMAX_DELAY;
                        if (next != ProcessTimers::Never) {
                                // A tick more: the first tick may come at once. Woken early, the
                                // timers are looked at again, so none fires early.
                                const int64_t left = next - Clock::monotonicNs();
                                const int64_t ticks = left <= 0 ? 0 : (left + TickNs - 1) / TickNs + 1;
                                wait = ticks < int64_t(portMAX_DELAY) ? static_cast<TickType_t>(ticks)
                                                                      : portMAX_DELAY - 1;
                        }
                        Console *console = nullptr;
                        if (xQueueReceive(signalQueue, &console, wait) == pdTRUE && console != nullptr)
                                deliverConsoleSignals(*console);
                }
        }

        // The consoles' hook: queues the console for the signal task (a full queue leaves the
        // set for the next signal's delivery).
        void queueConsoleSignals(Console &console, bool fromIsr) {
                Console *c = &console;
                if (!fromIsr) {
                        (void)xQueueSend(signalQueue, &c, 0);
                        return;
                }
                BaseType_t woken = pdFALSE;
                (void)xQueueSendFromISR(signalQueue, &c, &woken);
                portYIELD_FROM_ISR(woken);
                return;
        }

        // The consoles' read hook: a process of the console's session outside its foreground group
        // gets SIGTTIN (its group) and parks while stopped, then looks again; with SIGTTIN ignored or
        // blocked the read fails (EIO), caught it is interrupted (devplan/processes.md §3.7.1).
        Error admitReader(Console &console) {
                ProgramThread *const thread = ProgramThread::ofCurrentTask();
                Running *const       run = Running::ofCurrentTask();
                if (run == nullptr) return Error::Ok;
                // A kernel Thread of a built-in program acts as its first thread.
                ProgramThread &acting = thread != nullptr ? *thread : run->main;
                for (;;) {
                        Signals         &s = acting.current->signals();
                        Signals::Thread &mine = acting.currentSignals();
                        // A signal due (one that came while it was stopped, say) comes first.
                        if (mine.interrupt.isRaised()) return Error::Interrupted;
                        const int32_t session = console.session();
                        const int32_t pid = acting.currentPid;
                        const int32_t group = processes->groupOf(pid);
                        if (session == 0 || processes->sessionOf(pid) != session || console.foreground() == group)
                                return Error::Ok;
                        if (s.action(BMDS_SIGTTIN) == Signals::Action::Ignore ||
                            (Signals::blocked(mine) & Signals::bit(BMDS_SIGTTIN)) != 0)
                                return Error::IoError;
                        (void)signalGroup(group, BMDS_SIGTTIN, BMDS_SI_KERNEL);
                        if (!s.isStopped()) return Error::Interrupted;
                        mine.interrupt.park();
                }
        }

        // Process @p pid (of @p p) is ending: a terminal's foreground it held goes back to its
        // parent's group (or the session's) once no other process of its group runs; a session
        // leader's terminal has no session any more, and its foreground group gets SIGHUP.
        void leaveTerminal(const Process &p, int32_t pid) {
                Console *console = p.console();
                if (console == nullptr) return;
                int32_t hangUp = 0; // signalled after the lock (an end it causes comes back here)
                {
                        Mutex::Locker locker(*groupLock, WaitForever);
                        const int32_t session = processes->sessionOf(pid);
                        const int32_t group = processes->groupOf(pid);
                        if (session == 0 || console->session() != session) return;
                        if (pid == session) {
                                const int32_t foreground = console->foreground();
                                console->setSession(0);
                                console->setForeground(0);
                                if (foreground > 0 && foreground != group) hangUp = foreground;
                        } else if (console->foreground() == group && !processes->hasRunningInGroup(group, pid)) {
                                const int32_t parent = processes->parentOf(pid);
                                const bool    inSession = parent > 0 && processes->sessionOf(parent) == session;
                                console->setForeground(inSession ? processes->groupOf(parent) : session);
                        }
                }
                if (hangUp != 0) {
                        (void)signalGroup(hangUp, BMDS_SIGHUP, BMDS_SI_KERNEL);
                        (void)signalGroup(hangUp, BMDS_SIGCONT, BMDS_SI_KERNEL);
                }
                return;
        }

        // Records that @p pid ended with @p status, and tells its parent (SIGCHLD). Its stopped
        // children, which nobody would continue now, are hung up and continued (Linux's rule for
        // orphaned process groups, per process).
        void recordEnd(int32_t pid, int32_t status, uint64_t cpuUs = 0) {
                int32_t       stopped[ProcessTable::Capacity];
                const size_t  count = processes->stoppedChildren(pid, stopped, ProcessTable::Capacity);
                const int32_t parent = processes->parentOf(pid);
                dropTimer(pid);
                {
                        Mutex::Locker locker(*endLock, WaitForever);
                        processes->exited(pid, status, cpuUs);
                        if (parent > 0) tellParent(parent);
                }
                for (size_t i = 0; i < count && i < ProcessTable::Capacity; ++i) {
                        signalProcess(stopped[i], BMDS_SIGHUP);
                        signalProcess(stopped[i], BMDS_SIGCONT);
                }
                return;
        }

        // A wait reaped a child: its end's SIGCHLD is pending once this returns (endLock).
        void endRecorded() {
                Mutex::Locker locker(*endLock, WaitForever);
                return;
        }

        // The argument or environment vector @p v (NULL-terminated; NULL is empty) as views.
        int32_t viewsOf(char *const *v, List<StringView> &out) {
                if (v == nullptr) return 0;
                size_t n = 0;
                while (v[n] != nullptr) ++n;
                if (out.reserve(n).isError()) return -BMDS_ENOMEM;
                for (size_t i = 0; i < n; ++i) (void)out.append(StringView(v[i], std::strlen(v[i])));
                return 0;
        }

        int32_t execErrno(Error err) {
                switch (err.code()) {
                        case Error::NotSupported: return -BMDS_ENOEXEC;
                        case Error::IsDir: return -BMDS_EACCES;
                        case Error::Invalid: return -BMDS_E2BIG;
                        default: return Process::errnoFor(err);
                }
        }

} // namespace

// --- Program threads ------------------------------------------------------------

void ProgramThread::leave() {
        for (;;) {
                Signals         &s = current->signals();
                Signals::Thread &mine = currentSignals();
                // A stop signal the call unblocked stops it now; a stopped process parks here.
                if (const int sig = s.stopSignal(mine); sig != 0 && s.stopWith(mine, sig))
                        recordChange(currentPid, sig);
                mine.interrupt.park();
                // A vfork child is ended by the SDK (the hint); its parent once it is back.
                const int terminate = s.terminateSignal();
                if (depth == 0 && terminate != 0) endProcess(ProcessTable::killedStatus(terminate));
                if (hint != nullptr) *hint = s.isDue(mine, terminate) ? 1 : 0;
                Signals::clearWake(mine);
                inKernel.store(false);
                // Killed, or (running a vfork child meanwhile) its process ended while it was away.
                if (kill.load() || (depth == 0 && run->killed.load())) finish(run->killStatus.load());
                // Stopped meanwhile: a stopper that saw it in the kernel left it to park (the
                // handshake); one that saw it out suspended it before this.
                if (!s.isStopped()) return;
                inKernel.store(true);
        }
}

void ProgramThread::finish(int32_t status) {
        inKernel.store(true);
        endChildren(status);
        run->threadEnded(*this, status);
        queueStop(*this);
}

void ProgramThread::endProcess(int32_t status) {
        inKernel.store(true);
        {
                Sender sender;
                (void)processes->withProcess(run->pid, [&](void *) { sender.killTask(*run, status, false); });
        }
        finish(run->killStatus.load());
}

void ProgramThread::endChildren(int32_t status) {
        // Innermost first; each releases before it is recorded (its pipes are at EOF when its
        // parent wakes), and is freed after (the record no longer points its children at it).
        while (depth > 0) {
                Child c;
                {
                        Mutex::Locker locker(run->threadsLock, WaitForever);
                        c = children[--depth];
                        current = depth > 0 ? children[depth - 1].process : run->process;
                        currentPid = depth > 0 ? children[depth - 1].pid : run->pid;
                }
                leaveTerminal(*c.process, c.pid);
                c.process->release();
                recordEnd(c.pid, status);
                Process::destroy(c.process);
        }
        return;
}

Error ProgramThread::attach(Member &member) {
        return run->attach(member);
}

void ProgramThread::detach(Member &member) {
        run->detach(member);
        return;
}

TaskOwner *ProgramThread::threadOwner() {
        return run;
}

void ProgramThread::setCurrent(Process *p, int32_t id) {
        {
                Mutex::Locker locker(run->threadsLock, WaitForever);
                current = p;
                currentPid = id;
        }
        Interrupt::setCurrent(&currentSignals().interrupt);
        return;
}

int32_t ProgramThread::vfork() {
        if (depth >= MaxVforkDepth) return -BMDS_EAGAIN;
        Process *child = current->createChild(currentSignals());
        if (child == nullptr) return -BMDS_ENOMEM;
        // Its runner is this task's, until it execs (signals find it there).
        const Result<int32_t> added =
                processes->add(currentPid, ProcessTable::Inherit, ProcessTable::Inherit, &current->childEvent(), run);
        if (added.isError()) {
                Process::destroy(child);
                return -BMDS_EAGAIN;
        }
        {
                Mutex::Locker locker(run->threadsLock, WaitForever);
                children[depth++] = Child{child, added.value()};
        }
        setCurrent(child, added.value());
        return 0;
}

int32_t ProgramThread::endChild(int32_t waitStatus) {
        if (depth == 0) return -BMDS_EINVAL;
        Child c;
        {
                Mutex::Locker locker(run->threadsLock, WaitForever);
                c = children[--depth];
        }
        if (depth > 0) {
                setCurrent(children[depth - 1].process, children[depth - 1].pid);
        } else {
                setCurrent(run->process, run->pid);
        }
        leaveTerminal(*c.process, c.pid);
        c.process->release();
        recordEnd(c.pid, waitStatus);
        Process::destroy(c.process);
        return c.pid;
}

int32_t ProgramThread::vforkExit(int32_t status) {
        return endChild(ProcessTable::exitStatus(status));
}

int32_t ProgramThread::exec(const char *path, char *const *argv, char *const *envp) {
        StringView p;
        if (path == nullptr) return -BMDS_EFAULT;
        p = StringView(path, strnlen(path, BMDS_PATH_MAX + 1));
        if (p.size() > BMDS_PATH_MAX) return -BMDS_ENAMETOOLONG;
        if (p.isEmpty()) return -BMDS_ENOENT;
        Image         loaded;
        Process      *target = current;
        const int32_t targetPid = currentPid;
        Error         err;
        {
                // Scoped: the replacing path below never returns, so nothing
                // that owns memory may still be alive there.
                List<StringView> argViews;
                List<StringView> env;
                if (int32_t r = viewsOf(argv, argViews); r < 0) return r;
                if (int32_t r = viewsOf(envp, env); r < 0) return r;
                // Owned: a script's interpreter goes in front.
                List<String> args;
                if (args.reserve(argViews.size()).isError()) return -BMDS_ENOMEM;
                for (StringView a : argViews) {
                        auto [copy, cerr] = String::fromView(a);
                        if (cerr.isError()) return -BMDS_ENOMEM;
                        (void)args.append(std::move(copy));
                }
                if (args.isEmpty()) {
                        auto [copy, cerr] = String::fromView(p);
                        if (cerr.isError() || args.append(std::move(copy)).isError()) return -BMDS_ENOMEM;
                }
                // Looked up once (devplan/programs.md §19).
                ProgramFile program;
                err = lookUpProgram(p, current->cwd(), program);
                if (err.isError()) return execErrno(err);
                // BMDS_APP_DIR and BMDS_APP_ID are the new program's own (devplan/apps.md §3).
                String appDir;
                String appId;
                if (appVariablesFor(program, appDir, appId).isError()) return -BMDS_ENOMEM;
                for (size_t i = env.size(); i > 0; --i)
                        if (isAppVariable(env[i - 1])) env.removeAt(i - 1);
                if (!appDir.isEmpty() && env.append(appDir.view()).isError()) return -BMDS_ENOMEM;
                if (!appId.isEmpty() && env.append(appId.view()).isError()) return -BMDS_ENOMEM;
                LoadFailure failure;
                setImagePath(loaded, program.path.view());
                err = resolveExec(&program, p, current->cwd(), Span<const StringView>(env.data(), env.size()), args,
                                  loaded, failure, 0);
                if (err.isError()) return execErrno(err);
                argViews.clear();
                for (const String &a : args) {
                        if (argViews.append(a.view()).isError()) {
                                releaseImage(loaded);
                                return -BMDS_ENOMEM;
                        }
                }
                // The program is replaced: its other threads end first (Linux's de_thread).
                if (depth == 0) endOtherThreads();
                err = target->prepareExec(Span<const StringView>(argViews.data(), argViews.size()),
                                          Span<const StringView>(env.data(), env.size()));
                if (err.isError()) {
                        releaseImage(loaded);
                        return execErrno(err);
                }
        }
        // From here on the old program's memory is gone (in place) or its
        // child's process belongs to the new task (a vfork child).
        if (depth == 0 && signals != &target->signals().mainThread()) {
                // This thread goes on as the new program's first: its mask is that one's.
                Signals &s = target->signals();
                s.setBlocked(s.mainThread(), Signals::blocked(*signals));
                s.attach(s.mainThread());
                s.detach(*signals);
        }
        Running *replacement = createRunning(loaded, target, targetPid, p, current->cwd());
        if (replacement != nullptr && depth == 0) {
                // The process goes on: its time so far and its watch with it (a vfork child's time
                // was its parent's).
                replacement->cpuBaseUs = run->cpuUs();
                replacement->watchingStats.store(run->watchingStats.exchange(false));
        }
        if (replacement != nullptr) {
                processes->setRuntime(targetPid, replacement);
                int core = -1;
                err = startRunning(*replacement, p, loaded.stackBytes, loaded.display, depth == 0, core);
                if (err.isError()) {
                        processes->setRuntime(targetPid, nullptr);
                        destroyRunning(replacement);
                        replacement = nullptr;
                }
        }
        if (depth > 0) {
                // The child runs on its own (or failed past return: it ends);
                // this thread is the parent again.
                {
                        Mutex::Locker locker(run->threadsLock, WaitForever);
                        --depth;
                }
                if (depth > 0) {
                        setCurrent(children[depth - 1].process, children[depth - 1].pid);
                } else {
                        setCurrent(run->process, run->pid);
                }
                if (replacement == nullptr) {
                        leaveTerminal(*target, targetPid);
                        target->release();
                        recordEnd(targetPid, ProcessTable::exitStatus(127));
                        Process::destroy(target);
                        releaseImage(loaded);
                }
                return targetPid;
        }
        if (replacement == nullptr) {
                // The old program can't go on: its memory is gone.
                releaseImage(loaded);
                finish(ProcessTable::exitStatus(127));
        }
        // Replaced: the process goes on in the new task.
        inKernel.store(true);
        {
                Mutex::Locker locker(run->threadsLock, WaitForever);
                ended = true;
        }
        run->process = nullptr;
        current = nullptr;
        releaseImage(run->image, run->mapping);
        run->imageBytes = 0;
        queueStop(*this);
}

void ProgramThread::endOtherThreads() {
        {
                Sender sender;
                (void)processes->withProcess(run->pid, [&](void *) { sender.killOthers(*run); });
        }
        bool wait = false;
        {
                Mutex::Locker locker(run->threadsLock, WaitForever);
                wait = run->live > 1;
                run->execWaiting = wait;
        }
        // Those in a call end as it returns (their interrupts are raised).
        if (wait) (void)run->othersGone.acquire(WaitForever);
        return;
}

int32_t ProgramThread::wait(int32_t child, int32_t *status, uint32_t flags) {
        if ((flags & ~(BMDS_WAIT_NOHANG | BMDS_WAIT_UNTRACED | BMDS_WAIT_CONTINUED)) != 0) return -BMDS_EINVAL;
        uint32_t reap = 0;
        if ((flags & BMDS_WAIT_UNTRACED) != 0) reap |= ProcessTable::ReapStopped;
        if ((flags & BMDS_WAIT_CONTINUED) != 0) reap |= ProcessTable::ReapContinued;
        for (;;) {
                const Result<ProcessTable::Ended> got = processes->reap(currentPid, child, reap);
                if (got.isOk()) {
                        endRecorded();
                        if (status != nullptr) *status = got.value().status;
                        return got.value().pid;
                }
                if (got.error() == Error::NotExist) return -BMDS_ECHILD;
                if ((flags & BMDS_WAIT_NOHANG) != 0) return 0;
                const Error err = currentSignals().interrupt.wait(current->childEvent(), Deadline(WaitForever));
                if (err != Error::Interrupted) continue;
                // A child that ended meanwhile (its SIGCHLD may be the signal) still counts.
                const Result<ProcessTable::Ended> late = processes->reap(currentPid, child, reap);
                if (late.isOk()) {
                        endRecorded();
                        if (status != nullptr) *status = late.value().status;
                        return late.value().pid;
                }
                return -BMDS_EINTR;
        }
}

int32_t ProgramThread::id(uint32_t which) {
        switch (which) {
                case BMDS_PROC_SELF: return currentPid;
                case BMDS_PROC_PARENT: {
                        // An orphan's parent is 1, as Linux reports it.
                        const int32_t parent = processes->parentOf(currentPid);
                        return parent > 0 ? parent : ProcessTable::InitPid;
                }
                case BMDS_PROC_JOB: return processes->groupOf(currentPid);
                case BMDS_PROC_THREAD: return depth > 0 ? currentPid : tid;
                default: return -BMDS_EINVAL;
        }
}

int32_t ProgramThread::sigSetup(volatile uint32_t *word) {
        hint = word;
        return 0;
}

int32_t ProgramThread::sigTake(bmds_siginfo_t *info) {
        const Signals::Taken taken = current->signals().take(currentSignals());
        if (info != nullptr && info->size > sizeof(uint32_t)) {
                const bmds_siginfo_t out = {sizeof(bmds_siginfo_t), taken.signal, taken.code,
                                            taken.terminate ? BMDS_SIG_TERMINATE : 0u};
                // The fields that fit; the caller's size stays.
                const size_t n = info->size < sizeof(out) ? info->size : sizeof(out);
                std::memcpy(reinterpret_cast<uint8_t *>(info) + sizeof(uint32_t),
                            reinterpret_cast<const uint8_t *>(&out) + sizeof(uint32_t), n - sizeof(uint32_t));
        }
        return taken.signal;
}

int32_t ProgramThread::sigSuspend(uint32_t mask) {
        Signals         &s = current->signals();
        Signals::Thread &mine = currentSignals();
        s.setBlocked(mine, mask);
        while (!s.isDue(mine)) (void)mine.interrupt.sleep(WaitForever);
        return -BMDS_EINTR;
}

int32_t ProgramThread::startThread(const bmds_thread_t *spec) {
        if (spec == nullptr) return -BMDS_EFAULT;
        if (spec->size < sizeof(bmds_thread_t) || spec->entry == nullptr) return -BMDS_EINVAL;
        // A vfork child is its parent's thread for now, not a process that can have more.
        if (depth > 0) return -BMDS_EINVAL;
        uint32_t stackBytes = spec->stack_size != 0 ? spec->stack_size : BMDS_DEFAULT_STACK_SIZE;
        if (stackBytes > BMDS_MAX_STACK_SIZE) return -BMDS_EINVAL;
        if (stackBytes < BMDS_THREAD_STACK_MIN) stackBytes = BMDS_THREAD_STACK_MIN;
        stackBytes = (stackBytes + 15u) & ~15u;
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(ProgramThread));
        if (memory == nullptr) return -BMDS_ENOMEM;
        auto *t = new (memory) ProgramThread;
        t->run = run;
        t->thread = t;
        t->tid = newThreadId();
        t->current = run->process;
        t->currentPid = run->pid;
        t->signals = &t->own;
        t->own.blocked.store(Signals::blocked(*signals)); // the caller's mask
        t->hint = spec->hint;
        t->clear = spec->clear;
        t->entry = spec->entry;
        t->arg = spec->arg;
        t->tls = spec->tls;
        t->inKernel.store(true); // until it reaches the program's code
        Signals &s = run->process->signals();
        {
                Mutex::Locker locker(run->threadsLock, WaitForever);
                if (run->killed.load() || run->live == 0) {
                        destroyThread(t);
                        return -BMDS_EAGAIN;
                }
                t->next = run->others;
                run->others = t;
                ++run->live;
                ++run->records;
                s.attach(t->own); // held if the process is stopped, raised if it is ending
        }
        if (spec->tid != nullptr) *spec->tid = t->tid;
        Task::Config config;
        config.name = pcTaskGetName(nullptr); // the program's name, as its first task's
        config.stackBytes = stackBytes;
        config.stackSpace = stackSpaceFor(stackBytes, false);
        config.priority = RealTimeThreads::inherit(*t, this, normalPriority, false);
        const Error err = t->task.start(config, threadMain, t);
        if (err.isError()) {
                {
                        Mutex::Locker locker(run->threadsLock, WaitForever);
                        for (ProgramThread **at = &run->others; *at != nullptr; at = &(*at)->next) {
                                if (*at != t) continue;
                                *at = t->next;
                                break;
                        }
                        --run->live;
                        --run->records;
                        s.detach(t->own);
                }
                destroyThread(t);
                return -BMDS_ENOMEM;
        }
        bmdsDebug("Loader: thread %d of pid %d, stack %u in %s", static_cast<int>(t->tid), static_cast<int>(run->pid),
                  static_cast<unsigned>(stackBytes), memSpaceName(config.stackSpace));
        return t->tid;
}

void ProgramThread::exitThread(int32_t status, void *release) {
        inKernel.store(true);
        // A detached thread's own TLS: no kernel code uses it.
        if (release != nullptr && depth == 0 && run->process != nullptr) run->process->memFree(release);
        finish(ProcessTable::exitStatus(status));
}

int32_t ProgramThread::killThread(int32_t target, int32_t sig, uint32_t flags) {
        if (sig < 0 || sig >= Signals::Count || (flags & ~BMDS_THREAD_WAKE) != 0) return -BMDS_EINVAL;
        int32_t r = -BMDS_ESRCH;
        {
                Sender sender;
                (void)processes->withProcess(run->pid, [&](void *) {
                        r = sender.postToThread(*run, target, sig, (flags & BMDS_THREAD_WAKE) != 0);
                });
        }
        return r;
}

int32_t ProgramThread::setClear(volatile int32_t *word) {
        clear = word;
        return depth > 0 ? currentPid : tid;
}

int32_t ProgramThread::sched(uint32_t op, int32_t target, bmds_sched_t *param) {
        if (op == BMDS_SCHED_YIELD) {
                if (target != 0 || param != nullptr) return -BMDS_EINVAL;
                taskYIELD();
                return 0;
        }
        if (op != BMDS_SCHED_GET && op != BMDS_SCHED_SET && op != BMDS_SCHED_PIN) return -BMDS_EINVAL;
        bmds_sched_t p;
        if (const int32_t r = detail::copyIn(param, p); r < 0) return r;
        if (op == BMDS_SCHED_PIN) { // the caller only: its task is the running one
                if (target != 0 && target != (depth > 0 ? currentPid : tid)) return -BMDS_EINVAL;
                const uint32_t all = (1u << portNUM_PROCESSORS) - 1u, cores = p.cores & all;
                if (cores == all) return 0;
                if (cores == 0 || (cores & (cores - 1u)) != 0) return -BMDS_EINVAL; // one core
                return Thread::pinCurrent(__builtin_ctz(cores)).isOk() ? 0 : -BMDS_EINVAL;
        }
        if (op == BMDS_SCHED_SET && p.flags != 0) return -BMDS_EINVAL;
        auto act = [&](ProgramThread &t) {
                if (op == BMDS_SCHED_SET) return RealTimeThreads::set(t, p.policy, p.priority);
                RealTimeThreads::get(t, &t == this, p);
                return detail::copyOut(p, param);
        };
        const int32_t self = depth > 0 ? currentPid : tid;
        if (target == 0 || target == self) return act(*this);
        if (target < 0) return -BMDS_ESRCH;
        // Any program thread (a vfork child: the thread it runs on), its lists locked while
        // it is acted on: one listed has not ended, so its task is there.
        int32_t r = -BMDS_ESRCH;
        processes->forEachRunning([&](int32_t, void *runtime) {
                if (r != -BMDS_ESRCH || runtime == nullptr) return;
                Running      &other = *static_cast<Running *>(runtime);
                Mutex::Locker threads(other.threadsLock, WaitForever);
                auto          matches = [&](ProgramThread &t) {
                        return t.tid == target || (t.depth > 0 && t.currentPid == target);
                };
                ProgramThread *to = !other.main.ended && matches(other.main) ? &other.main : nullptr;
                for (ProgramThread *t = other.others; to == nullptr && t != nullptr; t = t->next)
                        if (matches(*t)) to = t;
                if (to != nullptr) r = act(*to);
        });
        return r;
}

namespace {

        // kill(): @p sig (0: only a check) to @p target as `proc_kill` takes it, from process
        // @p self (0: from the kernel, which has no group).
        int32_t sendSignalFrom(int32_t self, int32_t target, int32_t sig) {
                if (sig < 0 || sig >= Signals::Count) return -BMDS_EINVAL;
                if (target == ProcessTable::InitPid && (sig == BMDS_SIGKILL || sig == BMDS_SIGSTOP)) return -BMDS_EPERM;
                const int32_t selfGroup = self != 0 ? processes->groupOf(self) : 0;
                if (target == 0 && selfGroup == 0) return -BMDS_EINVAL;
                bool found = false;
                {
                        Sender sender;
                        auto   send = [&](int32_t id, void *runtime) {
                                found = true;
                                if (sig != 0 && runtime != nullptr)
                                        (void)sender.post(*static_cast<Running *>(runtime), id, sig, BMDS_SI_USER);
                        };
                        if (target > 0) {
                                found = processes->withProcess(target, [&](void *runtime) { send(target, runtime); }) !=
                                        ProcessTable::Found::None;
                        } else if (target == -1) {
                                processes->forEachRunning([&](int32_t id, void *runtime) {
                                        if (id != self) send(id, runtime);
                                });
                        } else {
                                processes->forEachInGroup(target == 0 ? selfGroup : -target, send);
                        }
                }
                return found ? 0 : -BMDS_ESRCH;
        }

        int32_t errnoOf(Error err) {
                switch (err.code()) {
                        case Error::Ok: return 0;
                        case Error::NotExist: return -BMDS_ESRCH;
                        case Error::PermissionDenied: return -BMDS_EPERM;
                        default: return -BMDS_EINVAL;
                }
        }

} // namespace

int32_t ProgramThread::sendSignal(int32_t target, int32_t sig) {
        return sendSignalFrom(currentPid, target, sig);
}

int32_t ProgramThread::exitSignal(int32_t sig) {
        if (!Signals::isValid(sig)) return -BMDS_EINVAL;
        const int32_t status = ProcessTable::killedStatus(sig);
        if (depth > 0) return endChild(status);
        endProcess(status);
}

int32_t ProgramThread::timer(uint32_t which, const bmds_timer_t *set, bmds_timer_t *old) {
        if (which != BMDS_TIMER_REAL) return -BMDS_EINVAL;
        constexpr uint64_t MaxUs = ProcessTimers::MaxNs / 1000;
        bmds_timer_t       wanted{};
        if (set != nullptr) {
                if (const int32_t r = detail::copyIn(set, wanted); r < 0) return r;
        }
        ProcessTimers::Setting was;
        {
                Mutex::Locker locker(*timerLock, WaitForever);
                const int64_t now = Clock::monotonicNs();
                was = timers.setting(currentPid, now);
                if (set != nullptr) {
                        ProcessTimers::Setting s;
                        s.valueNs = (wanted.value_us < MaxUs ? wanted.value_us : MaxUs) * 1000;
                        s.intervalNs = (wanted.interval_us < MaxUs ? wanted.interval_us : MaxUs) * 1000;
                        // Full can't be: an entry per process.
                        if (timers.set(currentPid, now, s).isError()) return -BMDS_EAGAIN;
                }
        }
        if (set != nullptr) timersChanged();
        if (old == nullptr) return 0;
        bmds_timer_t out{};
        out.value_us = (was.valueNs + 999) / 1000; // rounded up: an armed timer never reads 0
        out.interval_us = was.intervalNs / 1000;
        return detail::copyOut(out, old);
}

int32_t ProgramThread::group(uint32_t op, int32_t target, int32_t pgid) {
        if (target < 0) return -BMDS_EINVAL;
        const int32_t who = target != 0 ? target : currentPid;
        switch (op) {
                case BMDS_GROUP_GET:
                case BMDS_GROUP_SESSION: {
                        const int32_t id = op == BMDS_GROUP_GET ? processes->groupOf(who) : processes->sessionOf(who);
                        return id != 0 ? id : -BMDS_ESRCH;
                }
                case BMDS_GROUP_SET: {
                        Mutex::Locker locker(*groupLock, WaitForever);
                        return errnoOf(processes->setGroup(currentPid, target, pgid));
                }
                case BMDS_GROUP_NEW_SESSION: {
                        Mutex::Locker         locker(*groupLock, WaitForever);
                        const Result<int32_t> made = processes->newSession(currentPid);
                        return made.isOk() ? made.value() : errnoOf(made.error());
                }
                default: return -BMDS_EINVAL;
        }
}

int32_t ProgramThread::termForeground(int32_t fd, int32_t pgid) {
        if (pgid < 0) return -BMDS_EINVAL;
        Console      *console = nullptr;
        const int32_t r = current->terminalOf(fd, console);
        if (r < 0) return r;
        Mutex::Locker locker(*groupLock, WaitForever);
        const int32_t session = processes->sessionOf(currentPid);
        if (console == nullptr || session == 0 || console->session() != session) return -BMDS_ENOTTY;
        if (pgid == 0) return console->foreground();
        if (!processes->hasGroupInSession(pgid, session)) return -BMDS_EPERM;
        console->setForeground(pgid);
        console->setUnansweredGroup(0);
        return pgid;
}

// Pseudo-terminals and the controlling terminal (devplan/network.md §24).
int32_t ProgramThread::termControl(int32_t fd, uint32_t op, uint32_t value) {
        Process *const        p = current;
        const FileHandle::Ptr h = p->handle(fd);
        if (!h) return -BMDS_EBADF;
        Stream &stream = h->stream();
        switch (op) {
                case BMDS_TERM_PTY_NUMBER: return stream.pty() != nullptr ? stream.pty()->number() : -BMDS_ENOTTY;
                case BMDS_TERM_PTY_UNLOCK: return stream.pty() != nullptr ? 0 : -BMDS_ENOTTY;
                case BMDS_TERM_PTS_NUMBER: {
                        const int n = stream.isTerminal() ? static_cast<Console &>(stream).ptyNumber() : -1;
                        return n >= 0 ? n : -BMDS_ENOTTY;
                }
                case BMDS_TERM_SET_SIZE: {
                        const Console::Size size{static_cast<uint16_t>(value & 0xffffu),
                                                 static_cast<uint16_t>(value >> 16)};
                        if (size.columns == 0 || size.rows == 0) return -BMDS_EINVAL;
                        if (Pty *pty = stream.pty(); pty != nullptr) {
                                pty->setSize(size);
                                return 0;
                        }
                        if (!stream.isTerminal()) return -BMDS_ENOTTY;
                        static_cast<Console &>(stream).setSize(size); // every terminal is a Console
                        return 0;
                }
                case BMDS_TERM_CONTROL: {
                        if (!stream.isTerminal()) return -BMDS_ENOTTY;
                        Console &console = static_cast<Console &>(stream);
                        {
                                Mutex::Locker locker(*groupLock, WaitForever);
                                const int32_t session = processes->sessionOf(currentPid);
                                if (session != currentPid) return -BMDS_EPERM;
                                const Console *mine = p->console();
                                if (mine == &console && console.session() == session) return 0;
                                if (mine != nullptr && mine->session() == session) return -BMDS_EPERM;
                                if (console.session() != 0 && console.session() != session) return -BMDS_EPERM;
                                console.setSession(session);
                                console.setForeground(processes->groupOf(currentPid));
                                console.setUnansweredGroup(0);
                        }
                        p->setConsole(&console);
                        return 0;
                }
                case BMDS_TERM_RELEASE: {
                        Console *const mine = p->console();
                        if (mine == nullptr) return -BMDS_ENOTTY;
                        {
                                Mutex::Locker locker(*groupLock, WaitForever);
                                const int32_t session = processes->sessionOf(currentPid);
                                if (session == currentPid && mine->session() == session) {
                                        mine->setSession(0);
                                        mine->setForeground(0);
                                }
                        }
                        p->setConsole(nullptr);
                        return 0;
                }
                default: return -BMDS_EINVAL;
        }
}

FutexTable &programFutexes() {
        return *futexes;
}

// --- Running ------------------------------------------------------------------

Process *Running::processOf(int32_t id) {
        if (id == pid) return process;
        Process *found = nullptr;
        forEachThread([&](ProgramThread &t) {
                for (int i = 0; i < t.depth; ++i) {
                        if (t.children[i].pid == id) found = t.children[i].process;
                }
        });
        return found;
}

int32_t ProcessOwner::sessionId() {
        return run != nullptr && processes.isCreated() ? processes->sessionOf(run->pid) : 0;
}

Error Running::attach(Member &member) {
        Mutex::Locker locker(threadsLock, WaitForever);
        if (ending) return Error::InvalidState;
        member.next = threads;
        threads = &member;
        ++threadCount;
        return Error::Ok;
}

void Running::detach(Member &member) {
        Mutex::Locker locker(threadsLock, WaitForever);
        endedCpuUs += TaskList::cpuUs(member.task); // the thread itself, ending
        for (Member **m = &threads; *m != nullptr; m = &(*m)->next) {
                if (*m != &member) continue;
                *m = member.next;
                break;
        }
        if (--threadCount == 0 && ending) threadsGone.release();
        return;
}

void Running::endThreads() {
        {
                Mutex::Locker locker(threadsLock, WaitForever);
                ending = true;
                if (threadCount == 0) return;
        }
        // A built-in program that didn't join its threads: they run kernel code, so they are
        // asked to stop (the process's interrupt) and waited for, never stopped by force.
        bmdsWarn("Loader: %s (pid %d) ended with threads running: interrupting them", command, static_cast<int>(pid));
        if (process != nullptr) process->signals().interrupt().raise();
        (void)threadsGone.acquire(WaitForever);
        return;
}

void Running::threadEnded(ProgramThread &gone, int32_t status) {
        bool last = false;
        {
                Mutex::Locker locker(threadsLock, WaitForever);
                if (gone.ended) return;
                gone.ended = true;
                if (gone.task.isRunning()) endedCpuUs += TaskList::cpuUs(gone.task.handle());
                // Gone from program code: a joiner may free its memory now. The program's memory is
                // there while this thread is still counted.
                if (gone.clear != nullptr) {
                        *gone.clear = 0;
                        (void)futexes->wake(gone.clear, 1);
                }
                for (ProgramThread **at = &others; *at != nullptr; at = &(*at)->next) {
                        if (*at != &gone) continue;
                        *at = gone.next;
                        break;
                }
                if (process != nullptr) process->signals().detach(*gone.signals);
                last = --live == 0;
                if (execWaiting && live == 1) {
                        execWaiting = false;
                        othersGone.release();
                }
        }
        if (last) end(killed.load() ? killStatus.load() : status);
        return;
}

uint64_t Running::cpuUs() {
        Mutex::Locker locker(threadsLock, WaitForever);
        return cpuUsLocked();
}

uint64_t Running::cpuUsLocked() {
        uint64_t total = cpuBaseUs + endedCpuUs;
        auto     program = [&](const ProgramThread &t) {
                if (!t.ended && t.task.isRunning()) total += TaskList::cpuUs(t.task.handle());
        };
        program(main);
        for (const ProgramThread *t = others; t != nullptr; t = t->next) program(*t);
        for (const Member *m = threads; m != nullptr; m = m->next) total += TaskList::cpuUs(m->task);
        return total;
}

void Running::releaseStats() {
        if (watchingStats.exchange(false)) Stats::unwatch();
        return;
}

void Running::end(int32_t status) {
        endThreads();
        releaseStats();
        if (process != nullptr) {
                leaveTerminal(*process, pid);
                process->release();
                recordEnd(pid, status, cpuUs());
                Process::destroy(process);
                process = nullptr;
        }
        releaseImage(image, mapping);
        imageBytes = 0;
        return;
}

Result<int32_t> Running::waitChild(int32_t child, uint32_t flags, int32_t &status) {
        if (child <= 0) return makeError<int32_t>(Error::NotExist);
        const uint32_t how = ((flags & WaitBlock) == 0 ? BMDS_WAIT_NOHANG : 0u) |
                             ((flags & WaitStopped) != 0 ? BMDS_WAIT_UNTRACED : 0u) |
                             ((flags & WaitContinued) != 0 ? BMDS_WAIT_CONTINUED : 0u);
        const int32_t r = main.wait(child, &status, how);
        if (r >= 0) return makeResult(r);
        return makeError<int32_t>(r == -BMDS_EINTR ? Error::Interrupted : Error::NotExist);
}

Error Running::signal(int32_t target, int sig) {
        switch (sendSignalFrom(pid, target, sig)) {
                case 0: return Error::Ok;
                case -BMDS_ESRCH: return Error::NotExist;
                case -BMDS_EPERM: return Error::PermissionDenied;
                default: return Error::Invalid;
        }
}

Error Running::setForeground(int32_t group) {
        Console *console = process != nullptr ? process->console() : nullptr;
        if (console == nullptr) return Error::NotSupported;
        Mutex::Locker locker(*groupLock, WaitForever);
        const int32_t session = processes->sessionOf(pid);
        if (session == 0 || console->session() != session) return Error::NotSupported;
        const int32_t to = group != 0 ? group : processes->groupOf(pid);
        if (!processes->hasGroupInSession(to, session)) return Error::PermissionDenied;
        console->setForeground(to);
        console->setUnansweredGroup(0);
        return Error::Ok;
}

void Running::disownChild(int32_t child) {
        if (child <= 0 || processes->parentOf(child) != pid) return;
        const bool stopped = processes->isStopped(child);
        processes->detach(child);
        // An orphan now: if stopped, nobody would continue it (the rule of recordEnd()).
        if (stopped) {
                signalProcess(child, BMDS_SIGHUP);
                signalProcess(child, BMDS_SIGCONT);
        }
        return;
}

Error Running::setSignalAction(int sig, SignalAction action) {
        if (!Signals::isValid(sig) || process == nullptr) return Error::Invalid;
        Signals::Action to = Signals::Action::Default;
        if (action == SignalAction::Ignore) to = Signals::Action::Ignore;
        if (action == SignalAction::Interrupt) to = Signals::Action::Interrupt;
        return process->signals().setAction(sig, to) ? Error::Ok : Error::Invalid;
}

// --- Loader -------------------------------------------------------------------

namespace {

        // The process table's counters (devplan/sysmon.md §6): names are an interface once shipped.
        constexpr Stat::Desc ProcessStats[] = {
                {"count",
                 Stat::Unit::Count,
                 Stat::Kind::Level,
                 Stat::KeepHistory,
                 {Stat::Rule::Above, Stat::Health::Warning, 0, ProcessTable::Capacity * 3 / 4,
                  ProcessTable::Capacity - 2},
                 "",
                 "Processes, zombies included (the table holds 32)"},
                {"zombies", Stat::Unit::Count, Stat::Kind::Level, 0, {}, "", "Ended processes not yet waited for"},
                {"stopped", Stat::Unit::Count, Stat::Kind::Level, 0, {}, "", "Stopped processes"},
        };

        void readProcesses(void *, Span<int64_t> v) {
                int64_t count = 0, zombies = 0, stopped = 0;
                processes->forEach([&](const ProcessTable::View &p) {
                        ++count;
                        zombies += p.ended ? 1 : 0;
                        stopped += p.stopped ? 1 : 0;
                });
                v[0] = count;
                v[1] = zombies;
                v[2] = stopped;
                return;
        }

        constinit StaticObject<Stats::Group> processCounters;

        // Programs' shared pages (devplan/programs.md §14.2): names are an interface once shipped.
        constexpr Stat::Desc ImageStats[] = {
                {"mapped", Stat::Unit::Count, Stat::Kind::Level, 0, {}, "", "Program instances sharing pages now"},
                {"shared",
                 Stat::Unit::Bytes,
                 Stat::Kind::Level,
                 0,
                 {},
                 "",
                 "Their pages mapped from the program's cache entry, not copied: the memory saved"},
                {"window", Stat::Unit::Bytes, Stat::Kind::Level, 0, {}, "", "Addresses in use for them"},
                {"maps", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Instances mapped since boot"},
                {"copies",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {},
                 "",
                 "Programs with shared pages run as plain copies instead (no room in the window)"},
        };

        void readImages(void *, Span<int64_t> v) {
                const ImageWindow::Usage u = ImageWindow::usage();
                v[0] = u.instances;
                v[1] = int64_t(u.shared) * ProgramImage::SharePageBytes;
                v[2] = int64_t(u.used) * ProgramImage::SharePageBytes;
                v[3] = u.mapped;
                v[4] = u.fallbacks;
                return;
        }

        constinit StaticObject<Stats::Group> imageCounters;

} // namespace

namespace {

        // The pseudo-terminals programs open (devplan/network.md §24): /dev/ptmx makes a counted
        // pair, its descriptor the emulator's end; /dev/pts/N is pair N's program end.
        uint8_t handleMode(uint8_t flags) {
                return static_cast<uint8_t>(((flags & Vfs::Read) ? FileHandle::Read : 0) |
                                            ((flags & Vfs::Write) ? FileHandle::Write : 0));
        }

        Result<FileHandle::Ptr> openPtmx(void *, uint8_t flags) {
                const Result<Pty *> pty = Pty::openCounted(MemSpace::Psram);
                if (pty.isError())
                        return makeError<FileHandle::Ptr>(pty.error() == Error::Full ? Error::Busy : pty.error());
                Result<FileHandle::Ptr> handle = FileHandle::open(*pty.value(), handleMode(flags));
                if (handle.isError()) pty.value()->release();
                return handle;
        }

        bool ptsPresent(void *, size_t index) {
                return Pty::isCountedOpen(static_cast<uint8_t>(index));
        }

        Result<FileHandle::Ptr> openPts(void *, size_t index, uint8_t flags) {
                return Pty::openProgramEnd(static_cast<uint8_t>(index), handleMode(flags));
        }

        // The program end's handles have the console's identity, whoever opened them.
        uint64_t ptsIdentity(void *, size_t index) {
                return reinterpret_cast<uintptr_t>(Pty::programEnd(static_cast<uint8_t>(index)));
        }

        Error addPtyFiles() {
                static constexpr Vfs::DeviceFile      Ptmx{"ptmx", openPtmx, nullptr, nullptr, nullptr, true};
                static constexpr Vfs::DeviceDirectory Pts{"pts",   Pty::MaxPtys, ptsPresent,
                                                          openPts, nullptr,      ptsIdentity};
                Error                                 err = Vfs::addDeviceFile(Ptmx);
                if (err.isOk()) err = Vfs::addDeviceDirectory(Pts);
                return err == Error::Exists ? Error::Ok : err; // a second init()
        }

        // The profiler's view of a task: its program's image (devplan/sysmon.md §9). Called from
        // the tick hook, for the task it interrupted, which is running and so its process alive.
        bool IRAM_ATTR locateProgram(void *task, uint32_t &base, uint32_t &bytes, const char *&name) {
                auto          *owner = static_cast<ProcessOwner *>(TaskOwner::of(task));
                const Running *run = owner != nullptr ? owner->run : nullptr;
                if (run == nullptr || run->image == nullptr) return false;
                base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(run->image));
                bytes = static_cast<uint32_t>(run->imageBytes);
                name = run->command;
                return true;
        }

} // namespace

Error Loader::init() {
        if (!stopLock.isCreated()) stopLock.create();
        if (!groupLock.isCreated()) groupLock.create();
        if (!timerLock.isCreated()) timerLock.create();
        if (!endLock.isCreated()) endLock.create();
        if (!processes.isCreated()) processes.create();
        if (!futexes.isCreated()) futexes.create();
        if (!imageCache.isCreated()) {
                // Programs' instances share their pages through the window (§14.2): an entry of a
                // program with shared pages then has its bytes page-aligned, to be mapped in place.
                (void)ImageWindow::init(size_t(CONFIG_BMDS_IMAGE_WINDOW_MB) * 1024 * 1024);
                imageCache.create(size_t(CONFIG_BMDS_IMAGE_CACHE_KB) * 1024,
                                  ImageWindow::isEnabled() ? ProgramImage::SharePageBytes : 0);
                Vfs::setChangeHook(fileChanged);
                (void)Memory::addReclaimer(
                        &*imageCache); // before the fonts' (a later boot stage): programs reload cheaply
        }
        Profiler::setLocator(&locateProgram);
        Error err = processes->init();
        if (err.isOk()) err = RealTimeThreads::init();
        if (err.isOk() && signalQueue == nullptr) {
                signalQueue = xQueueCreate(SignalQueueLength, sizeof(Console *));
                if (signalQueue == nullptr) err = Error::NoMem;
        }
        if (err.isOk() && !signalTask.isRunning()) {
                Task::Config config;
                config.name = "sig";
                config.stackBytes = SignalTaskStackBytes;
                config.stackSpace = MemSpace::Psram;
                config.priority = CONFIG_BMDS_SHELL_PRIORITY + 1;
                err = signalTask.start(config, signalTaskMain, nullptr);
        }
        if (err.isOk()) {
                Console::setSignalHook(queueConsoleSignals);
                Console::setReadHook(admitReader);
        }
        if (err.isOk()) err = addPtyFiles();
        if (err.isOk() && !processCounters.isCreated()) {
                processCounters.create(ProcessStats, sizeof(ProcessStats) / sizeof(ProcessStats[0]), readProcesses,
                                       nullptr);
                processCounters->setName("proc");
                (void)Stats::add(*processCounters);
        }
        if (err.isOk() && ImageWindow::isEnabled() && !imageCounters.isCreated()) {
                imageCounters.create(ImageStats, sizeof(ImageStats) / sizeof(ImageStats[0]), readImages, nullptr);
                imageCounters->setName("images");
                (void)Stats::add(*imageCounters);
        }
        return err;
}

namespace {

        // Under the table's lock a running entry's Running and Process are alive: both are freed
        // only after the entry ends (Running::end()).
        void describeProcess(const ProcessTable::View &v, Loader::ProcessInfo &p) {
                using State = Loader::ProcessInfo::State;
                p = Loader::ProcessInfo{};
                p.pid = v.pid;
                p.parent = v.parent;
                p.group = v.group;
                p.session = v.session;
                p.status = v.status;
                p.startedUs = v.startedUs;
                p.cpuUs = v.cpuUs;
                p.childrenCpuUs = v.childrenCpuUs;
                p.state = v.ended ? State::Zombie : v.stopped ? State::Stopped : State::Running;
                auto *run = static_cast<Running *>(v.runtime);
                if (run == nullptr) return;
                std::memcpy(p.command, run->command, sizeof(p.command));
                const Process *process = run->processOf(v.pid);
                p.memory = process != nullptr ? process->allocatedBytes() : 0;
                if (v.pid != run->pid) {
                        p.state = State::Vfork; // its time is its parent's: it runs on the parent's task
                        return;
                }
                Mutex::Locker locker(run->threadsLock, WaitForever);
                const int     tasks = run->live + run->threadCount;
                p.threads = static_cast<uint8_t>(tasks < 255 ? tasks : 255);
                p.cpuUs = run->cpuUsLocked();
                if (run->ending) p.state = State::Ending;
                return;
        }

} // namespace

size_t Loader::listProcesses(Span<ProcessInfo> out) {
        size_t n = 0;
        if (!processes.isCreated()) return 0;
        processes->forEach([&](const ProcessTable::View &v) {
                if (n < out.size()) describeProcess(v, out[n]);
                ++n;
        });
        return n;
}

Error Loader::processInfo(int32_t pid, bool next, ProcessInfo &out) {
        if (!processes.isCreated()) return Error::NotExist;
        bool found = false;
        processes->forEach([&](const ProcessTable::View &v) { // in pid order
                if (found || (next ? v.pid <= pid : v.pid != pid)) return;
                describeProcess(v, out);
                found = true;
        });
        return found ? Error::Ok : Error::NotExist;
}

void Loader::findTaskOwners(Span<TaskOwnership> tasks) {
        for (TaskOwnership &t : tasks) {
                t.pid = 0;
                t.tid = 0;
                t.main = false;
        }
        if (!processes.isCreated()) return;
        auto mark = [&](void *task, int32_t pid, int32_t tid, bool main) {
                for (TaskOwnership &t : tasks) {
                        if (t.task != task) continue;
                        t.pid = pid;
                        t.tid = tid;
                        t.main = main;
                }
        };
        processes->forEach([&](const ProcessTable::View &v) {
                auto *run = static_cast<Running *>(v.runtime);
                if (run == nullptr || v.pid != run->pid) return;
                Mutex::Locker locker(run->threadsLock, WaitForever);
                auto          program = [&](const ProgramThread &t) {
                        if (!t.ended && t.task.isRunning()) mark(t.task.handle(), v.pid, t.tid, &t == &run->main);
                };
                program(run->main);
                for (const ProgramThread *t = run->others; t != nullptr; t = t->next) program(*t);
                for (const TaskOwner::Member *m = run->threads; m != nullptr; m = m->next)
                        mark(m->task, v.pid, 0, false);
        });
        return;
}

namespace {

        void describeTask(TaskHandle_t task, int32_t pid, bool main, Loader::ThreadInfo &out) {
                out.pid = pid;
                out.tid = 0;
                out.main = main;
                std::snprintf(out.name, sizeof(out.name), "%s", pcTaskGetName(task));
                switch (eTaskGetState(task)) {
                        case eRunning:
                        case eReady: out.state = 'R'; break;
                        case eBlocked: out.state = 'S'; break;
                        default: out.state = 'T'; break;
                }
                out.stackFree = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task));
                return;
        }

} // namespace

size_t Loader::listThreads(Span<ThreadInfo> out) {
        size_t n = 0;
        if (!processes.isCreated()) return 0;
        processes->forEach([&](const ProcessTable::View &v) {
                auto *run = static_cast<Running *>(v.runtime);
                if (run == nullptr || v.pid != run->pid) return;
                // A thread's task is stopped only after it has ended, which takes this lock; a
                // Thread's is deleted only after it detaches, which takes it too.
                Mutex::Locker locker(run->threadsLock, WaitForever);
                auto          program = [&](const ProgramThread &t) {
                        if (t.ended || !t.task.isRunning()) return;
                        if (n < out.size()) {
                                describeTask(t.task.handle(), v.pid, &t == &run->main, out[n]);
                                out[n].tid = t.tid;
                        }
                        ++n;
                };
                program(run->main);
                for (const ProgramThread *t = run->others; t != nullptr; t = t->next) program(*t);
                for (const TaskOwner::Member *m = run->threads; m != nullptr; m = m->next) {
                        if (n < out.size()) describeTask(static_cast<TaskHandle_t>(m->task), v.pid, false, out[n]);
                        ++n;
                }
        });
        return n;
}

Error Loader::signal(int32_t pid, int sig) {
        if (!processes.isCreated()) return Error::NotExist;
        switch (const int32_t r = sendSignalFrom(0, pid, sig); r) {
                case 0: return Error::Ok;
                case -BMDS_ESRCH: return Error::NotExist;
                case -BMDS_EPERM: return Error::PermissionDenied;
                default: return Error::Invalid;
        }
}

namespace {

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


        // The calling task's process as it acts now (a vfork child's while one runs on it), or null.
        Process *callerProcess() {
                if (ProgramThread *t = ProgramThread::ofCurrentTask(); t != nullptr) return t->current;
                Running *run = Running::ofCurrentTask();
                return run != nullptr ? run->process : nullptr;
        }

        // Its pid, or the kernel's (what kernel code starts has the kernel as parent).
        int32_t callerPid() {
                if (ProgramThread *t = ProgramThread::ofCurrentTask(); t != nullptr) return t->currentPid;
                Running *run = Running::ofCurrentTask();
                return run != nullptr ? run->pid : ProcessTable::KernelParent;
        }

        // A job (group @p group) that stopped, or (not @p stopped) ended leaving nobody in its group,
        // had @p console's foreground: it goes back to the group of @p parent, the shell that waited
        // for it (devplan/processes.md §3.7.1).
        void giveTerminalBack(Console *console, int32_t group, int32_t parent, bool stopped) {
                if (console == nullptr || parent <= 0 || group <= 0) return;
                Mutex::Locker locker(*groupLock, WaitForever);
                const int32_t session = processes->sessionOf(parent);
                if (session == 0 || console->session() != session || console->foreground() != group) return;
                if (!stopped && processes->hasRunningInGroup(group, 0)) return;
                console->setForeground(processes->groupOf(parent));
                console->setUnansweredGroup(0);
                return;
        }

} // namespace

// Loads and starts the program in @p request, its end to release @p ended: its pid. The terminal's
// modes as they were, in @p savedMode (the program may change them); @p parent its parent.
Result<int32_t> Loader::begin(const Request &request, Semaphore &ended, Console::TermMode &savedMode, int32_t &parent) {
        if (+_thread_local_bss_end != +_thread_local_data_start) {
                report(request, "cannot run programs: the kernel has thread-local storage");
                return makeError<int32_t>(Error::NotSupported);
        }
        if (!processes.isCreated()) {
                report(request, "cannot run programs: no process table");
                return makeError<int32_t>(Error::InvalidState);
        }
        drainStops();
        const StringView cwd = request.cwd.isEmpty() ? StringView("/") : request.cwd;
        Image            image;
        LoadFailure      failure;
        // Called by a process (a shell, a built-in program's thread): its child.
        parent = callerPid();
        Error       err = Error::Ok;
        ProgramFile program; // looked up once (devplan/programs.md §19)
        if (request.builtin != nullptr) {
                image.builtin = request.builtin;
                image.stackBytes = static_cast<uint32_t>(request.builtin->program.stackBytes);
        } else {
                err = lookUpProgram(request.path, cwd, program);
                setImagePath(image, program.path.view());
                if (err.isOk()) err = loadImage(program, image, failure);
        }
        if (err.isError()) {
                const ProgramImage::Problem &problem = failure.problem;
                if (failure.stage == nullptr || problem.what == nullptr) {
                        if (failure.stage != nullptr)
                                report(request, "%s: %s", failure.stage, errorText(err));
                        else
                                report(request, "%s", errorText(err));
                } else if (problem.hasValue) {
                        report(request, "%s: %s (0x%x)", failure.stage, problem.what,
                               static_cast<unsigned>(problem.value));
                } else {
                        report(request, "%s: %s", failure.stage, problem.what);
                }
                return makeError<int32_t>(err);
        }

        // Its resources. The environment gets the kernel's time zone as TZ
        // unless it has one (the C library's localtime reads TZ).
        Process *process = Process::create(*request.fds, request.console);
        if (process == nullptr) {
                report(request, "out of memory");
                releaseImage(image);
                return makeError<int32_t>(Error::NoMem);
        }
        // What a job-control shell runs without job control can't be stopped (devplan/processes.md §3.7.1).
        if (request.ignoreStops) {
                for (int sig : {BMDS_SIGTSTP, BMDS_SIGTTIN, BMDS_SIGTTOU})
                        (void)process->signals().setAction(sig, Signals::Action::Ignore);
        }
        List<StringView> environment;
        String           tz;
        String           appDir;
        String           appId;
        err = process->setCwd(cwd);
        if (err.isOk()) err = environment.reserve(request.environment.size() + 3);
        if (err.isOk() && request.builtin == nullptr) err = appVariablesFor(program, appDir, appId);
        bool hasTz = false;
        for (StringView e : request.environment) {
                hasTz = hasTz || e.startsWith("TZ=");
                if (isAppVariable(e)) continue; // the program's own, below
                if (err.isOk()) err = environment.append(e);
        }
        if (err.isOk() && !appDir.isEmpty()) err = environment.append(appDir.view());
        if (err.isOk() && !appId.isEmpty()) err = environment.append(appId.view());
        // A program from a file (musl reads TZ) gets the kernel zone, as it is now, by its name
        // when it has one, so musl reads the database's file and dates before today's rules come
        // out right. Built-in programs ask the kernel themselves, so they follow a change of it
        // (the system configuration's clock/zone, devplan/sysconf.md §8): no snapshot for them,
        // nor for what they start in turn.
        const Clock::ZoneName name = Clock::zoneName();
        const TimeZone        zone = Clock::timeZone();
        const StringView      value = !name.view().isEmpty() ? name.view() : zone.spec();
        if (err.isOk() && !hasTz && image.builtin == nullptr && !value.isEmpty()) {
                err = tz.append("TZ=");
                if (err.isOk()) err = tz.append(value);
                if (err.isOk()) err = environment.append(tz.view());
        }
        if (err.isOk())
                err = process->setArguments(request.args,
                                            Span<const StringView>(environment.data(), environment.size()));
        if (err.isError()) {
                report(request, "%s", err == Error::NoMem ? "out of memory" : "argument list too long");
                Process::destroy(process);
                releaseImage(image);
                return makeError<int32_t>(err);
        }

        // Its process and its task.
        Running *run = createRunning(image, process, 0, request.path, cwd);
        if (run == nullptr) {
                report(request, "out of memory");
                Process::destroy(process);
                releaseImage(image);
                return makeError<int32_t>(Error::NoMem);
        }
        run->data = request.data;
        // Its group and session, and the terminal's foreground (devplan/processes.md §3.6.1).
        Result<int32_t> added = makeError<int32_t>(Error::Busy);
        {
                Mutex::Locker              groups(*groupLock, WaitForever);
                ProgramLauncher::JobGroup *job = request.session ? nullptr : request.group;
                int32_t                    group = ProcessTable::Inherit;
                if (request.session) group = ProcessTable::New;
                if (job != nullptr) group = job->id != 0 ? job->id : ProcessTable::New;
                added = processes->add(parent, group, request.session ? ProcessTable::New : ProcessTable::Inherit,
                                       &ended, run);
                Console *console = request.console;
                if (added.isOk()) {
                        const int32_t pid = added.value();
                        if (job != nullptr && job->id == 0) job->id = pid;
                        if (console != nullptr && request.session) {
                                console->setSession(pid);
                                console->setForeground(pid);
                                console->setUnansweredGroup(0);
                        } else if (console != nullptr && job != nullptr && job->foreground &&
                                   console->session() == processes->sessionOf(pid)) {
                                console->setForeground(job->id);
                                console->setUnansweredGroup(0);
                        }
                }
        }
        if (added.isError()) {
                report(request, "cannot start: %s",
                       added.error() == Error::Full ? "too many processes" : "not in that process group");
                destroyRunning(run);
                Process::destroy(process);
                releaseImage(image);
                return makeError<int32_t>(added.error() == Error::Full ? Error::Busy : added.error());
        }
        const int32_t pid = added.value();
        run->pid = run->main.currentPid = run->main.tid = pid;
        // The program may change the terminal's modes: the shell gets its own back.
        savedMode = request.console != nullptr ? request.console->mode() : Console::TermMode();
        int core = -1;
        err = startRunning(*run, request.path, image.stackBytes, image.display, false, core);
        if (err.isError()) {
                report(request, "cannot start: %s", errorText(err));
                processes->setRuntime(pid, nullptr);
                leaveTerminal(*process, pid);
                processes->exited(pid, ProcessTable::exitStatus(127));
                (void)processes->reap(parent, pid);
                destroyRunning(run);
                Process::destroy(process);
                releaseImage(image);
                return makeError<int32_t>(err);
        }
        if (request.core != nullptr) *request.core = static_cast<int8_t>(core);
        return makeResult(pid);
}

Result<int> Loader::run(const Request &request) {
        Semaphore             ended{1, 0};
        Console::TermMode     savedMode;
        int32_t               parent = 0;
        const Result<int32_t> started = begin(request, ended, savedMode, parent);
        if (started.isError()) return makeError<int>(started.error());
        const int32_t pid = started.value();
        const int32_t group = processes->groupOf(pid);
        // Signals reach the program from its terminal (its foreground group). One the caller
        // catches (an interactive shell's SIGINT) leaves it waiting; one that ends the caller
        // leaves the child an orphan (it won't release `ended`, about to go).
        Running *const              self = Running::ofCurrentTask();
        const bool                  untraced = request.stopped != nullptr && self != nullptr;
        Result<ProcessTable::Ended> result = makeError<ProcessTable::Ended>(Error::NotExist);
        for (Interrupt *interrupt = request.interrupt;;) {
                const uint32_t seen = interrupt != nullptr ? interrupt->raises() : 0;
                const Error    err = interrupt != nullptr ? interrupt->wait(ended, Deadline(WaitForever), seen)
                                                          : ended.acquire(WaitForever);
                if (err == Error::Interrupted) {
                        if (self != nullptr && !self->isEnding()) continue;
                        processes->detach(pid);
                        if (request.console != nullptr) request.console->setMode(savedMode);
                        return makeError<int>(Error::Interrupted);
                }
                // Ended, or stopped or continued (`ended` is released for each).
                result = processes->reap(parent, pid, untraced ? uint32_t(ProcessTable::ReapStopped) : 0u);
                if (result.isError() && result.error() == Error::WouldBlock) continue;
                if (result.isOk() && ProcessTable::isStoppedStatus(result.value().status)) {
                        // Stopped: the caller's child from now on, and the terminal is the caller's again.
                        processes->setNotify(pid, &callerProcess()->childEvent());
                        giveTerminalBack(request.console, group, parent, true);
                        if (request.console != nullptr) request.console->setMode(savedMode);
                        *request.stopped = pid;
                        return makeResult(static_cast<int>(128 + ((result.value().status >> 8) & 0x7f)));
                }
                break;
        }
        // Its group may outlive it with nobody in it (its last member made a session of its own
        // after it ended: setsid): the terminal is the caller's again all the same.
        giveTerminalBack(request.console, group, parent, false);
        if (request.console != nullptr) request.console->setMode(savedMode);
        drainStops();
        const int32_t status = result.isOk() ? result.value().status : ProcessTable::exitStatus(127);
        // Exited: its status; killed: 128 + the signal, as in bash.
        if ((status & 0x7f) != 0) return makeResult(static_cast<int>(128 + (status & 0x7f)));
        return makeResult(static_cast<int>((status >> 8) & 0xff));
}

Result<int32_t> Loader::start(const Request &request) {
        Semaphore         ended{1, 0};
        Console::TermMode savedMode;
        int32_t           parent = 0;
        // A waited-for child tells its parent's process as it ends, as a vfork child does.
        Process *const caller = callerProcess();
        if (request.waited && caller == nullptr) return makeError<int32_t>(Error::Invalid);
        Semaphore &notify = request.waited ? caller->childEvent() : ended;
        // A group of its own, not in the foreground (unless asked otherwise).
        ProgramLauncher::JobGroup background{0, false};
        Request                   own = request;
        if (own.group == nullptr && !own.session) own.group = &background;
        const Result<int32_t> started = begin(own, notify, savedMode, parent);
        if (started.isError()) return started;
        // Nobody waits: an orphan, freed when it ends (at once if it already has); its end
        // released to whoever asked.
        if (!request.waited) processes->detach(started.value(), request.ended);
        return started;
}

BMDS_NAMESPACE_END
