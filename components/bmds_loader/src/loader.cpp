/**
 * @file      loader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>
#include <esp_cache.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <sdkconfig.h>
#include <bmds/clock.h>
#include <bmds/console.h>
#include <bmds/deadline.h>
#include <bmds/errno.h>
#include <bmds/interrupt.h>
#include <bmds/list.h>
#include <bmds/loader.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/processtable.h>
#include <bmds/program.h>
#include <bmds/programimage.h>
#include <bmds/staticobject.h>
#include <bmds/shebang.h>
#include <bmds/sys.h>
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

        constinit StaticObject<ProcessTable> processes;

        // `/bin/sh` (Loader::setShell()), and what runs the programs it starts.
        constinit BuiltinProgram shellProgram;
        constinit Loader         builtinLauncher;

        // Deepest nesting of `#!` interpreters in an exec (the shell's limit).
        constexpr int MaxInterpreterDepth = 4;

        // Ended programs whose tasks wait to be stopped (see queueStop()).
        constinit StaticObject<Mutex> stopLock;
        constinit Running            *stopQueue = nullptr;

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
                        uint8_t              *memory = nullptr;
                        size_t                bytes = 0;
                        uint32_t              entry = 0;
                        uint32_t              stackBytes = 0;
                        const BuiltinProgram *builtin = nullptr;
        };

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

        // Opens the file at @p path (relative to @p cwd) to run it.
        Error openProgram(StringView path, StringView cwd, FileHandle::Ptr &file, uint32_t &size) {
                const Vfs::Context     context = contextFor(cwd);
                const Result<FileInfo> info = Vfs::stat(path, context);
                Error                  err = info.error();
                if (err.isOk() && info.value().isDirectory()) err = Error::IsDir;
                if (err.isOk() && info.value().size > UINT32_MAX) err = Error::NotSupported;
                if (err.isError()) return err;
                auto [opened, oerr] = Vfs::open(path, Vfs::Read, context);
                if (oerr.isError()) return oerr;
                file = std::move(opened);
                size = static_cast<uint32_t>(info.value().size);
                return Error::Ok;
        }

        // Loads the program in @p file (@p fileSize bytes).
        Error loadFrom(FileHandle &file, uint32_t fileSize, Image &image, LoadFailure &failure) {
                FileSource         source(file);
                ProgramImage::Info header;
                Error              err;
                err = ProgramImage::inspect(source, fileSize, header, failure.problem);
                if (err.isError()) {
                        if (failure.problem.what != nullptr) failure.stage = "cannot execute";
                        return err == Error::Invalid ? Error::NotSupported : err;
                }
                // Its memory: the image, 4096-aligned and whole pages (so the
                // cache sync covers whole lines), then loaded and relocated in place.
                image.bytes = (header.imageSize + BMDS_LOAD_ALIGN - 1) & ~size_t(BMDS_LOAD_ALIGN - 1);
                image.memory =
                        static_cast<uint8_t *>(Memory::allocAligned(MemSpace::Psram, image.bytes, BMDS_LOAD_ALIGN));
                if (image.memory == nullptr) return Error::NoMem;
                err = ProgramImage::load(source, fileSize, header, image.memory,
                                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(image.memory)),
                                         failure.problem);
                if (err.isOk()) {
                        // Write the image back from the D-cache and drop stale lines
                        // everywhere, including the I-cache (fence.i isn't enough here).
                        err = Error::fromEspErr(
                                esp_cache_msync(image.memory, image.bytes,
                                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
                }
                if (err.isError()) {
                        failure.stage = "cannot load";
                        Memory::free(image.memory);
                        image.memory = nullptr;
                        return err;
                }
                image.entry = header.entry;
                image.stackBytes = header.stackSize != 0 ? header.stackSize : BMDS_DEFAULT_STACK_SIZE;
                return Error::Ok;
        }

        // Loads the program at @p path (relative to @p cwd).
        Error loadImage(StringView path, StringView cwd, Image &image, LoadFailure &failure) {
                FileHandle::Ptr file;
                uint32_t        size = 0;
                const Error     err = openProgram(path, cwd, file, size);
                return err.isError() ? err : loadFrom(*file, size, image, failure);
        }

        StringView baseName(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        bool isFile(StringView path, StringView cwd) {
                const Result<FileInfo> info = Vfs::stat(path, contextFor(cwd));
                return info.isOk() && info.value().type == FileType::File;
        }

        // Finds @p name as the shell would on the `PATH` in @p env: in each
        // directory `name`, `name.elf`, `name.sh`.
        Error findInPath(StringView name, Span<const StringView> env, StringView cwd, String &out) {
                StringView dirs;
                bool       found = false;
                for (StringView e : env) {
                        if (e.startsWith("PATH=")) {
                                dirs = e.mid(5);
                                found = true;
                        }
                }
                if (!found || name.isEmpty() || name.contains('/')) return Error::NotExist;
                static const char *const suffixes[] = {"", ".elf", ".sh"};
                size_t                   at = 0;
                for (;;) {
                        size_t colon = dirs.indexOf(':', at);
                        if (colon == StringView::NPos) colon = dirs.size();
                        const StringView dir = colon > at ? dirs.mid(at, colon - at) : StringView(".");
                        for (const char *suffix : suffixes) {
                                out.clear();
                                Error err = out.append(dir);
                                if (err.isOk() && !dir.endsWith('/')) err = out.append('/');
                                if (err.isOk()) err = out.append(name);
                                if (err.isOk()) err = out.append(suffix);
                                if (err.isError()) return err;
                                if (isFile(out.view(), cwd)) return Error::Ok;
                        }
                        if (colon == dirs.size()) break;
                        at = colon + 1;
                }
                return Error::NotExist;
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

        // What runs @p path for an exec (devplan/programs.md §13, 8b): a
        // program; a `#!` script's interpreter (an existing file, `/bin/sh`
        // or `/bin/bash` the shell, `/usr/bin/env name` looked up on `PATH`),
        // in front of the script in @p args; the shell for a `.sh` file and
        // for `/bin/sh`.
        Error resolveExec(StringView path, StringView cwd, Span<const StringView> env, List<String> &args, Image &image,
                          LoadFailure &failure, int depth) {
                if (depth > MaxInterpreterDepth) return Error::TooManyLinks;
                const bool isShell = path == "/bin/sh" || path == "/bin/bash";
                if (isShell && !isFile(path, cwd)) {
                        if (shellProgram.main == nullptr) return Error::NotExist;
                        image.builtin = &shellProgram;
                        image.stackBytes = static_cast<uint32_t>(shellProgram.stackBytes);
                        return Error::Ok;
                }
                FileHandle::Ptr file;
                uint32_t        size = 0;
                Error           err = openProgram(path, cwd, file, size);
                if (err.isError()) return err;
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
                        return loadFrom(*file, size, image, failure);
                file = FileHandle::Ptr();
                if (Shebang::isScript(headView)) {
                        Shebang line;
                        if (!Shebang::parse(headView, size <= got, line) || line.interpreter.isEmpty())
                                return Error::NotSupported;
                        // Copies: the views point into head, and path may be an element of args.
                        auto [script, cerr] = String::fromView(path);
                        auto [interp, ierr] = String::fromView(line.interpreter);
                        auto [arg, aerr] = String::fromView(line.argument);
                        if (cerr.isError() || ierr.isError() || aerr.isError()) return Error::NoMem;
                        if (!isFile(interp.view(), cwd) && baseName(interp.view()) == "env" && !arg.isEmpty()) {
                                String found;
                                err = findInPath(arg.view(), env, cwd, found);
                                if (err.isOk()) err = withFront(args, {arg.view(), script.view()});
                                if (err.isError()) return err;
                                return resolveExec(found.view(), cwd, env, args, image, failure, depth + 1);
                        }
                        err = arg.isEmpty() ? withFront(args, {interp.view(), script.view()})
                                            : withFront(args, {interp.view(), arg.view(), script.view()});
                        if (err.isError()) return err;
                        return resolveExec(interp.view(), cwd, env, args, image, failure, depth + 1);
                }
                if (path.endsWith(StringView(".sh"))) {
                        auto [script, cerr] = String::fromView(path);
                        if (cerr.isError()) return Error::NoMem;
                        err = withFront(args, {StringView("sh"), script.view()});
                        if (err.isError()) return err;
                        return resolveExec(StringView("/bin/sh"), cwd, env, args, image, failure, depth + 1);
                }
                return Error::NotSupported;
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
                run.finish(ProcessTable::exitStatus(status));
        }

        // A built-in program's task (the shell as /bin/sh): all of it is
        // kernel code, so a kill leaves it to end itself (it sees Ctrl-C).
        void builtinMain(void *arg) {
                Running &run = *static_cast<Running *>(arg);
                vTaskSetThreadLocalStoragePointer(nullptr, RunningTlsIndex, &run);
                run.inKernel.store(true);
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
                        context.interrupt = p.interrupt();
                        context.launcher = &builtinLauncher;
                        status = err.isOk() ? run.builtin->main(context) : 126;
                }
                run.finish(ProcessTable::exitStatus(status));
        }

        // A Running for @p image and @p process (owned once it starts), not yet started.
        Running *createRunning(const Image &image, Process *process, int32_t pid, int32_t job) {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(Running));
                if (memory == nullptr) return nullptr;
                auto *run = new (memory) Running;
                run->process = run->current = process;
                run->pid = run->currentPid = pid;
                run->job = job;
                run->image = image.memory;
                run->builtin = image.builtin;
                run->entry = image.builtin != nullptr ? 0 : reinterpret_cast<uintptr_t>(image.memory) + image.entry;
                return run;
        }

        void destroyRunning(Running *run) {
                run->~Running();
                Memory::free(run);
                return;
        }

        // Starts @p run's task, named after @p path's last component.
        Error startRunning(Running &run, StringView path, size_t stackBytes) {
                Task::Config config;
                const size_t slash = path.lastIndexOf('/');
                char         name[configMAX_TASK_NAME_LEN];
                std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(path.size() - slash - 1),
                              path.data() + slash + 1);
                config.name = name;
                config.stackBytes = stackBytes;
                // Stacks are hot: internal RAM if there is room to spare. Not
                // a built-in's (the shell's recursion budget is large).
                config.stackSpace = run.builtin == nullptr && Memory::stats(MemSpace::Internal).largestFreeBlock >=
                                                                      config.stackBytes + InternalReserveBytes
                                            ? MemSpace::Internal
                                            : MemSpace::Psram;
                config.priority = uxTaskPriorityGet(nullptr);
                const Error err = run.task.start(config, run.builtin != nullptr ? builtinMain : programMain, &run);
                if (err.isOk())
                        bmdsDebug("Loader: %s (pid %d) at %p, stack %u in %s", name, static_cast<int>(run.pid),
                                  run.image, static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace));
                return err;
        }

        // Stops the queued tasks and frees their Runnings: from the timer
        // service task, and at every start (in case its queue was full).
        void drainStops() {
                for (;;) {
                        Running *run = nullptr;
                        {
                                Mutex::Locker locker(*stopLock, WaitForever);
                                run = stopQueue;
                                if (run != nullptr) stopQueue = run->nextStop;
                        }
                        if (run == nullptr) return;
                        (void)run->task.stop();
                        destroyRunning(run);
                }
        }

        void drainStopsLater(void *, uint32_t) {
                drainStops();
                return;
        }

        // Queues the calling program's task to be stopped; it then suspends
        // itself (or is suspended by the stop, whichever comes first).
        [[noreturn]] void queueStop(Running &run) {
                {
                        Mutex::Locker locker(*stopLock, WaitForever);
                        run.nextStop = stopQueue;
                        stopQueue = &run;
                }
                (void)xTimerPendFunctionCall(drainStopsLater, nullptr, 0, 0);
                for (;;) vTaskSuspend(nullptr);
        }

        // Kills every process of job @p job outside a system call; those
        // inside one end themselves when it returns (see Running).
        void killJob(int32_t job) {
                Running *frozen[ProcessTable::Capacity];
                int      count = 0;
                processes->forEachInJob(job, [&](int32_t, void *runtime) {
                        auto *run = static_cast<Running *>(runtime);
                        if (run == nullptr || run->kill.load()) return;
                        run->kill.store(true);
                        (void)run->task.suspend();
                        if (run->inKernel.load()) {
                                (void)run->task.resume();
                        } else {
                                frozen[count++] = run;
                        }
                });
                // Suspended in program code: nothing else touches them now.
                for (int i = 0; i < count; ++i) {
                        frozen[i]->end(ProcessTable::killedStatus(Running::KillSignal));
                        (void)frozen[i]->task.stop();
                        destroyRunning(frozen[i]);
                }
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

// --- Running ------------------------------------------------------------------

void Running::leave() {
        inKernel.store(false);
        if (kill.load()) finish(ProcessTable::killedStatus(KillSignal));
        return;
}

void Running::end(int32_t status) {
        // Innermost first; each releases before it is recorded (its pipes are
        // at EOF when its parent wakes), and is freed after (the record no
        // longer points its children at it).
        while (depth > 0) {
                const Child c = children[--depth];
                c.process->release();
                processes->exited(c.pid, status);
                Process::destroy(c.process);
        }
        current = process;
        currentPid = pid;
        if (process != nullptr) {
                process->release();
                processes->exited(pid, status);
                Process::destroy(process);
                process = current = nullptr;
        }
        Memory::free(image);
        image = nullptr;
        return;
}

void Running::finish(int32_t status) {
        inKernel.store(true);
        end(status);
        queueStop(*this);
}

int32_t Running::vfork() {
        if (depth >= MaxVforkDepth) return -BMDS_EAGAIN;
        Process *child = current->createChild();
        if (child == nullptr) return -BMDS_ENOMEM;
        // Its record has no runner: it dies with this task.
        const Result<int32_t> added = processes->add(currentPid, job, &current->childEvent(), nullptr);
        if (added.isError()) {
                Process::destroy(child);
                return -BMDS_EAGAIN;
        }
        children[depth++] = Child{child, added.value()};
        current = child;
        currentPid = added.value();
        return 0;
}

int32_t Running::vforkExit(int32_t status) {
        if (depth == 0) return -BMDS_EINVAL;
        const Child c = children[--depth];
        current = depth > 0 ? children[depth - 1].process : process;
        currentPid = depth > 0 ? children[depth - 1].pid : pid;
        c.process->release();
        processes->exited(c.pid, ProcessTable::exitStatus(status));
        Process::destroy(c.process);
        return c.pid;
}

int32_t Running::exec(const char *path, char *const *argv, char *const *envp) {
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
                LoadFailure failure;
                err = resolveExec(p, current->cwd(), Span<const StringView>(env.data(), env.size()), args, loaded,
                                  failure, 0);
                if (err.isError()) return execErrno(err);
                argViews.clear();
                for (const String &a : args) {
                        if (argViews.append(a.view()).isError()) {
                                Memory::free(loaded.memory);
                                return -BMDS_ENOMEM;
                        }
                }
                err = target->prepareExec(Span<const StringView>(argViews.data(), argViews.size()),
                                          Span<const StringView>(env.data(), env.size()));
                if (err.isError()) {
                        Memory::free(loaded.memory);
                        return execErrno(err);
                }
        }
        // From here on the old program's memory is gone (in place) or its
        // child's process belongs to the new task (a vfork child).
        Running *run = createRunning(loaded, target, targetPid, job);
        if (run != nullptr) {
                processes->setRuntime(targetPid, run);
                err = startRunning(*run, p, loaded.stackBytes);
                if (err.isError()) {
                        processes->setRuntime(targetPid, nullptr);
                        destroyRunning(run);
                        run = nullptr;
                }
        }
        if (depth > 0) {
                // The child runs on its own (or failed past return: it ends);
                // this task is the parent again.
                --depth;
                current = depth > 0 ? children[depth - 1].process : process;
                currentPid = depth > 0 ? children[depth - 1].pid : pid;
                if (run == nullptr) {
                        target->release();
                        processes->exited(targetPid, ProcessTable::exitStatus(127));
                        Process::destroy(target);
                        Memory::free(loaded.memory);
                }
                return targetPid;
        }
        if (run == nullptr) {
                // The old program can't go on: its memory is gone.
                Memory::free(loaded.memory);
                finish(ProcessTable::exitStatus(127));
        }
        // Replaced: the process goes on in the new task.
        inKernel.store(true);
        process = current = nullptr;
        Memory::free(image);
        image = nullptr;
        queueStop(*this);
}

int32_t Running::wait(int32_t child, int32_t *status, uint32_t flags) {
        if ((flags & ~BMDS_WAIT_NOHANG) != 0) return -BMDS_EINVAL;
        for (;;) {
                const Result<ProcessTable::Ended> ended = processes->reap(currentPid, child);
                if (ended.isOk()) {
                        if (status != nullptr) *status = ended.value().status;
                        return ended.value().pid;
                }
                if (ended.error() == Error::NotExist) return -BMDS_ECHILD;
                if ((flags & BMDS_WAIT_NOHANG) != 0) return 0;
                Semaphore &event = current->childEvent();
                if (current->interrupt() != nullptr) {
                        const Error err = current->interrupt()->wait(event, Deadline(WaitForever));
                        if (err == Error::Interrupted) return -BMDS_EINTR;
                } else {
                        (void)event.acquire(WaitForever);
                }
        }
}

int32_t Running::id(uint32_t which) {
        switch (which) {
                case BMDS_PROC_SELF: return currentPid;
                case BMDS_PROC_PARENT: {
                        const int32_t parent = processes->parentOf(currentPid);
                        return parent != 0 ? parent : ProcessTable::ShellPid;
                }
                default: return -BMDS_EINVAL;
        }
}

// --- Loader -------------------------------------------------------------------

void Loader::setShell(const BuiltinProgram &shell) {
        shellProgram = shell;
        return;
}

Error Loader::init() {
        if (!stopLock.isCreated()) stopLock.create();
        if (!processes.isCreated()) processes.create();
        return processes->init();
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

        // Waits for a program to end (@p ended is released). The first Ctrl-C
        // is the program's to handle (its calls return EINTR); one more
        // while it runs kills its job, @p job.
        void waitOrKill(int32_t job, Semaphore &ended, Interrupt *interrupt) {
                if (interrupt == nullptr) {
                        (void)ended.acquire(WaitForever);
                        return;
                }
                const Deadline forever(WaitForever);
                uint32_t       seen = interrupt->raises();
                const uint32_t atStart = seen;
                bool           killed = false;
                for (;;) {
                        if (interrupt->wait(ended, forever, seen).isOk()) return;
                        seen = interrupt->raises();
                        if (seen < 2 || seen == atStart || killed) continue;
                        killed = true;
                        killJob(job);
                }
        }

} // namespace

Result<int> Loader::run(const Request &request) {
        if (+_thread_local_bss_end != +_thread_local_data_start) {
                report(request, "cannot run programs: the kernel has thread-local storage");
                return makeError<int>(Error::NotSupported);
        }
        if (!processes.isCreated()) {
                report(request, "cannot run programs: no process table");
                return makeError<int>(Error::InvalidState);
        }
        drainStops();
        const StringView cwd = request.cwd.isEmpty() ? StringView("/") : request.cwd;
        Image            image;
        LoadFailure      failure;
        // Called by a process (the shell as /bin/sh): its child, in its job.
        const auto *caller = static_cast<const Running *>(pvTaskGetThreadLocalStoragePointer(nullptr, RunningTlsIndex));
        const int32_t parent = caller != nullptr ? caller->currentPid : ProcessTable::ShellPid;
        const int32_t parentJob = caller != nullptr ? caller->job : 0;
        Error         err = loadImage(request.path, cwd, image, failure);
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
                return makeError<int>(err);
        }

        // Its resources. The environment gets the kernel's time zone as TZ
        // unless it has one (the C library's localtime reads TZ).
        Process *process = Process::create(*request.fds, request.console, request.interrupt);
        if (process == nullptr) {
                report(request, "out of memory");
                Memory::free(image.memory);
                return makeError<int>(Error::NoMem);
        }
        List<StringView> environment;
        String           tz;
        err = process->setCwd(cwd);
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
                err = process->setArguments(request.args,
                                            Span<const StringView>(environment.data(), environment.size()));
        if (err.isError()) {
                report(request, "%s", err == Error::NoMem ? "out of memory" : "argument list too long");
                Process::destroy(process);
                Memory::free(image.memory);
                return makeError<int>(err);
        }

        // Its process and its task.
        Semaphore ended{1, 0};
        Running  *run = createRunning(image, process, 0, 0);
        if (run == nullptr) {
                report(request, "out of memory");
                Process::destroy(process);
                Memory::free(image.memory);
                return makeError<int>(Error::NoMem);
        }
        const Result<int32_t> added = processes->add(parent, parentJob, &ended, run);
        if (added.isError()) {
                report(request, "cannot start: too many processes");
                destroyRunning(run);
                Process::destroy(process);
                Memory::free(image.memory);
                return makeError<int>(Error::Busy);
        }
        const int32_t pid = added.value();
        run->pid = run->currentPid = pid;
        run->job = parentJob != 0 ? parentJob : pid;
        // The program may change the terminal's modes: the shell gets its own back.
        const Console::TermMode savedMode = request.console != nullptr ? request.console->mode() : Console::TermMode();
        err = startRunning(*run, request.path, image.stackBytes);
        if (err.isError()) {
                report(request, "cannot start: %s", errorText(err));
                processes->setRuntime(pid, nullptr);
                processes->exited(pid, ProcessTable::exitStatus(127));
                (void)processes->reap(parent, pid);
                destroyRunning(run);
                Process::destroy(process);
                Memory::free(image.memory);
                return makeError<int>(err);
        }
        const int32_t  job = run->job;
        const uint32_t raisesAtStart = request.interrupt != nullptr ? request.interrupt->raises() : 0;
        waitOrKill(job, ended, request.interrupt);
        // Interrupted, the job ends with it (until signals: its other processes
        // got EINTR too, and those ignoring it couldn't be killed any more).
        if (request.interrupt != nullptr && request.interrupt->raises() != raisesAtStart && job == pid) killJob(job);
        const Result<ProcessTable::Ended> result = processes->reap(parent, pid);
        if (request.console != nullptr) request.console->setMode(savedMode);
        drainStops();
        const int32_t status = result.isOk() ? result.value().status : ProcessTable::exitStatus(127);
        // Exited: its status; killed: 128 + the signal, as in bash.
        if ((status & 0x7f) != 0) return makeResult(static_cast<int>(128 + (status & 0x7f)));
        return makeResult(static_cast<int>((status >> 8) & 0xff));
}

BMDS_NAMESPACE_END
