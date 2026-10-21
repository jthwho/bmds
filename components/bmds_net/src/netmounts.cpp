/**
 * @file      netmounts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network filesystems kept mounted from `net/mounts` (devplan/netfs.md step 4): a kernel
 * service as the SSH server's (sshservice.cpp), one program a mount.
 */

#include <atomic>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/filehandle.h>
#include <bmds/log.h>
#include <bmds/logstream.h>
#include <bmds/memorystream.h>
#include <bmds/mutex.h>
#include <bmds/netmounts.h>
#include <bmds/netstatus.h>
#include <bmds/programlauncher.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>
#include "findprogram.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *MountsKey = "net/mounts";

        constexpr unsigned int FirstRetryMs = 5'000;
        constexpr unsigned int MaxRetryMs = 300'000;
        constexpr unsigned int SteadyMs = 60'000;  // a run this long: the next failure retries soon again
        constexpr unsigned int StopWaitMs = 5'000; // SIGTERM, then this long before SIGKILL
        constexpr int          SigKill = 9;
        constexpr int          SigTerm = 15;

        // One entry's program.
        struct Run {
                        String       text{MemSpace::Psram}; // the entry (empty: the slot is free)
                        int32_t      pid = 0;
                        unsigned int retryMs = 0; // the last wait before starting it again
                        int64_t      nextUs = 0;  // not started before (monotonic)
                        int64_t      startedUs = 0;
        };

        struct State {
                        ProgramLauncher   *launcher = nullptr;
                        SysConf::Watcher   watcher;
                        NetStatus::Watcher network;
                        Semaphore          wake{1, 0}; // the setting or the network changed, a program ended
                        Mutex              lock;       // the thread's start and end
                        Thread             thread;
                        bool               running = false; // the thread runs (lock)
                        std::atomic<bool>  changed{true};
                        LogStream          log{"netmounts"};
                        Run                runs[NetMounts::MaxEntries];
        };

        constinit StaticObject<State> state;

        int64_t nowUs() {
                return Clock::monotonicNs() / 1000;
        }

        // The entries now (none if the value is bad: the check refuses that when it is set).
        size_t entries(String &value, NetMounts::Entry (&out)[NetMounts::MaxEntries]) {
                size_t n = 0;
                if (SysConf::get(StringView(MountsKey), value).isError()) return 0;
                if (NetMounts::parse(value.view(), Span<NetMounts::Entry>(out, NetMounts::MaxEntries), n).isError())
                        return 0;
                return n;
        }

        bool hasEntries() {
                String           value(MemSpace::Psram);
                NetMounts::Entry e[NetMounts::MaxEntries];
                return entries(value, e) != 0;
        }

        // An interface other than lo is up with an address.
        bool networkUp() {
                const size_t n = NetStatus::count();
                for (size_t i = 0; i < n; ++i) {
                        NetStatus::Info info;
                        if (NetStatus::info(i, info).isError()) continue;
                        if (!info.loopback && info.up && info.link && info.address != 0) return true;
                }
                return false;
        }

        // The wait after a failure, @p ms the last (0: none yet).
        unsigned int nextRetry(unsigned int ms) {
                if (ms == 0) return FirstRetryMs;
                return ms * 2 > MaxRetryMs ? MaxRetryMs : ms * 2;
        }

        bool isRunning(int32_t pid) {
                return pid > 0 && state->launcher->sendSignal(pid, 0).isOk();
        }

        // Starts @p run's program in the foreground (-f): its pid, or 0.
        int32_t start(const Run &run) {
                NetMounts::Entry e[1];
                size_t           n = 0;
                if (NetMounts::parse(run.text.view(), Span<NetMounts::Entry>(e, 1), n).isError() || n != 1) return 0;
                String program(MemSpace::Psram), path(MemSpace::Psram), pathVar(MemSpace::Psram);
                if (!NetPrograms::find(e[0].words[0], program)) {
                        bmdsWarn("netmounts: no %.*s on shell/path", int(e[0].words[0].size()), e[0].words[0].data());
                        return 0;
                }
                NetPrograms::searchPath(path);
                if (pathVar.appendf("PATH=%s", path.cStr()).isError()) return 0;
                auto [in, ierr] = FileHandle::open(NullStream::instance(), FileHandle::Read);
                auto [out, oerr] = FileHandle::open(state->log, FileHandle::Write);
                if (ierr.isError() || oerr.isError()) return 0;
                FdTable fds;
                (void)fds.set(FdTable::Stdin, in);
                (void)fds.set(FdTable::Stdout, out);
                (void)fds.set(FdTable::Stderr, out);
                // The program, -f (it stays this pid), then the entry's arguments.
                StringView args[NetMounts::MaxWords + 1];
                size_t     count = 0;
                args[count++] = e[0].words[0];
                args[count++] = StringView("-f");
                for (size_t i = 1; i < e[0].count; ++i) args[count++] = e[0].words[i];
                // HOME and PATH as a shell's: the client finds the keys and the known hosts there.
                const StringView         env[] = {StringView("HOME=/home"), pathVar.view()};
                ProgramLauncher::Request request;
                request.path = program.view();
                request.args = Span<const StringView>(args, count);
                request.environment = Span<const StringView>(env, 2);
                request.fds = &fds;
                request.cwd = "/";
                request.ended = &state->wake;
                const Result<int32_t> pid = state->launcher->start(request);
                if (pid.isError()) {
                        bmdsWarn("netmounts: %s: %s", program.cStr(), pid.error().desc());
                        return 0;
                }
                bmdsInfo("netmounts: %s (pid %ld)", run.text.cStr(), long(pid.value()));
                return pid.value();
        }

        // Ends @p run's program (SIGTERM unmounts; SIGKILL if it stays).
        void stop(Run &run) {
                const int32_t pid = run.pid;
                run.pid = 0;
                if (pid == 0 || state->launcher->sendSignal(pid, SigTerm).isError()) return;
                for (int round = 0; round < 2; ++round) {
                        const Deadline deadline(StopWaitMs);
                        while (isRunning(pid)) {
                                if (state->wake.acquire(deadline).isError()) break;
                        }
                        if (!isRunning(pid)) break;
                        (void)state->launcher->sendSignal(pid, SigKill);
                }
                bmdsInfo("netmounts: stopped %s", run.text.cStr());
                return;
        }

        // The runs made to match the setting: those gone or changed stopped, the new ones added.
        // @return true if any are left.
        bool update() {
                String           value(MemSpace::Psram);
                NetMounts::Entry e[NetMounts::MaxEntries];
                const size_t     n = entries(value, e);
                for (Run &run : state->runs) {
                        if (run.text.isEmpty()) continue;
                        bool kept = false;
                        for (size_t i = 0; i < n; ++i) kept |= e[i].text == run.text.view();
                        if (kept) continue;
                        stop(run);
                        run.text.clear();
                }
                for (size_t i = 0; i < n; ++i) {
                        bool there = false;
                        for (const Run &run : state->runs) there |= run.text.view() == e[i].text;
                        if (there) continue;
                        for (Run &run : state->runs) {
                                if (!run.text.isEmpty()) continue;
                                if (run.text.assign(e[i].text).isError()) break;
                                run.pid = 0;
                                run.retryMs = 0;
                                run.nextUs = 0;
                                break;
                        }
                }
                for (const Run &run : state->runs)
                        if (!run.text.isEmpty()) return true;
                return false;
        }

        void serve(void *) {
                for (;;) {
                        if (state->changed.exchange(false)) {
                                SysConf::Id id;
                                while (state->watcher.next(id)) {}
                                (void)state->watcher.overflowed();
                                if (!update()) {
                                        // None left: the thread ends, unless entries came meanwhile.
                                        Mutex::Locker lock(state->lock, WaitForever);
                                        if (state->changed.load()) continue;
                                        state->running = false;
                                        state->thread.detach();
                                        return;
                                }
                        }
                        // Programs that ended: started again after a while.
                        const int64_t now = nowUs();
                        for (Run &run : state->runs) {
                                if (run.pid == 0 || isRunning(run.pid)) continue;
                                // A run of a minute: the next failure is a new one.
                                const bool steady = now - run.startedUs >= int64_t(SteadyMs) * 1000;
                                run.pid = 0;
                                run.retryMs = steady ? FirstRetryMs : nextRetry(run.retryMs);
                                run.nextUs = now + int64_t(run.retryMs) * 1000;
                                bmdsWarn("netmounts: %s ended; again in %u s", run.text.cStr(), run.retryMs / 1000);
                        }
                        // Those due, while the network is up; the wait until the next one is.
                        const bool   up = networkUp();
                        unsigned int waitMs = WaitForever;
                        for (Run &run : state->runs) {
                                if (run.text.isEmpty() || run.pid != 0 || !up) continue;
                                if (now >= run.nextUs) {
                                        run.pid = start(run);
                                        run.startedUs = now;
                                        if (run.pid != 0) continue;
                                        run.retryMs = nextRetry(run.retryMs);
                                        run.nextUs = now + int64_t(run.retryMs) * 1000;
                                }
                                const unsigned int ms = unsigned((run.nextUs - now) / 1000) + 1;
                                if (waitMs == WaitForever || ms < waitMs) waitMs = ms;
                        }
                        (void)state->wake.acquire(waitMs);
                }
        }

        // Entries and not running: the thread (none without entries).
        void startThread() {
                Mutex::Locker lock(state->lock, WaitForever);
                if (state->running || !hasEntries()) return;
                state->changed.store(true);
                Thread::Config config;
                config.name = "netmounts";
                config.stackBytes = 6144;
                config.stackSpace = MemSpace::Psram;
                config.kernel = true;
                const Error err = state->thread.start(config, &serve, nullptr);
                if (err.isError()) {
                        bmdsErr("netmounts: no thread: %s", err.desc());
                        return;
                }
                state->running = true;
                return;
        }

        void settingsChanged(void *) {
                state->changed.store(true);
                state->wake.release();
                startThread();
                return;
        }

        // On the network task: only a wake.
        void networkChanged(void *) {
                state->wake.release();
                return;
        }

} // namespace

Error NetMounts::init(ProgramLauncher &launcher) {
        state.create();
        state->launcher = &launcher;
        static constexpr Config::Param Param = {
                MountsKey,
                Config::Type::String,
                "",
                "Network filesystems kept mounted, ';'-separated: each a mount program and its arguments "
                "(sshfs user@host:dir /net/name); keys without a passphrase, the host already known",
                0,
                1023,
                nullptr,
                NetMounts::check};
        const Result<SysConf::Id> id = SysConf::declareKernel(Param);
        if (id.isError()) {
                bmdsErr("net: setting %s not declared: %s", Param.key, id.error().desc());
                return id.error();
        }
        Error err = state->watcher.start(StringView(MountsKey), &settingsChanged, nullptr);
        if (err.isError()) return err;
        err = state->network.start(&networkChanged, nullptr);
        if (err.isError()) bmdsWarn("netmounts: no network watch: %s", err.desc());
        startThread();
        return Error::Ok;
}

BMDS_NAMESPACE_END
