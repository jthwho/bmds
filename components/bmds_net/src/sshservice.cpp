/**
 * @file      sshservice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SSH server as a kernel service (devplan/network.md §24).
 */

#include <atomic>
#include <cstdio>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/filehandle.h>
#include <bmds/log.h>
#include <bmds/logstream.h>
#include <bmds/memorystream.h>
#include <bmds/mutex.h>
#include <bmds/programlauncher.h>
#include <bmds/semaphore.h>
#include <bmds/sshservice.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/texttemplate.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>

#include "netmdns.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *ServerKey = "net/ssh/server";
        constexpr const char *PortKey = "net/ssh/port";
        constexpr const char *PasswordKey = "net/ssh/password-login";
        constexpr const char *HostKeysKey = "net/ssh/host-keys";
        constexpr const char *AuthorizedKey = "net/ssh/authorized-keys-dir";

        constexpr const char *HostKeyName = "dropbear_ed25519_host_key";
        constexpr const char *DefaultPath = "/bin:/rom/bin:/sd0/p0/bin"; // as the shell's, when shell/path is unset

        constexpr unsigned int FirstRetryMs = 5'000;
        constexpr unsigned int MaxRetryMs = 300'000;
        constexpr unsigned int SteadyMs = 60'000;  // a run this long: the next failure retries soon again
        constexpr unsigned int StopWaitMs = 5'000; // SIGTERM, then this long before SIGKILL
        constexpr int          SigKill = 9;
        constexpr int          SigTerm = 15;

        Error checkTemplate(StringView value) {
                return TextTemplate::check(value);
        }

        struct State {
                        ProgramLauncher  *launcher = nullptr;
                        SysConf::Watcher  watcher;
                        Semaphore         wake{1, 0}; // a setting changed, or the server ended
                        Mutex             lock;       // the thread's start and end
                        Thread            thread;
                        bool              running = false; // the thread runs (lock)
                        std::atomic<bool> changed{false};
                        LogStream         log{"ssh"};
        };

        constinit StaticObject<State> state;

        bool enabled() {
                auto [on, err] = SysConf::getBool(StringView(ServerKey));
                return err.isOk() && on;
        }

        // @p name in the directories of shell/path (the first that has it), into @p out.
        bool findProgram(const char *name, String &out) {
                String path(MemSpace::Psram);
                if (SysConf::get(StringView("shell/path"), path).isError() || path.isEmpty())
                        (void)path.assign(StringView(DefaultPath));
                StringView rest = path.view();
                while (!rest.isEmpty()) {
                        const size_t     colon = rest.indexOf(':');
                        const StringView dir = colon == StringView::NPos ? rest : rest.left(colon);
                        rest = colon == StringView::NPos ? StringView() : rest.mid(colon + 1);
                        if (dir.isEmpty()) continue;
                        out.clear();
                        if (out.appendf("%.*s/%s", int(dir.size()), dir.data(), name).isError()) return false;
                        const Result<FileInfo> info = Vfs::stat(out.view(), Vfs::Context{"/", nullptr});
                        if (info.isOk() && !info.value().isDirectory()) return true;
                }
                return false;
        }

        // A request with no input, its output to the log.
        struct Launch {
                        FdTable                  fds;
                        ProgramLauncher::Request request;
                        StringView               args[12];
                        size_t                   count = 0;

                        Error prepare(StringView path) {
                                auto [in, ierr] = FileHandle::open(NullStream::instance(), FileHandle::Read);
                                auto [out, oerr] = FileHandle::open(state->log, FileHandle::Write);
                                if (ierr.isError() || oerr.isError()) return Error::NoMem;
                                (void)fds.set(FdTable::Stdin, in);
                                (void)fds.set(FdTable::Stdout, out);
                                (void)fds.set(FdTable::Stderr, out);
                                request.path = path;
                                request.fds = &fds;
                                request.cwd = "/";
                                return Error::Ok;
                        }

                        void add(StringView arg) {
                                if (count < sizeof(args) / sizeof(args[0])) args[count++] = arg;
                                return;
                        }

                        void finish() {
                                request.args = Span<const StringView>(args, count);
                                return;
                        }
        };

        // The host key, made the first time (dropbearkey, next to dropbear).
        Error ensureHostKey(StringView dropbear, const String &key, const String &dir) {
                const Vfs::Context root{"/", nullptr};
                if (Vfs::stat(key.view(), root).isOk()) return Error::Ok;
                const Error made = Vfs::mkdir(dir.view(), root);
                if (made.isError() && made != Error::Exists) return made;
                String       keygen(MemSpace::Psram);
                const size_t slash = dropbear.lastIndexOf('/');
                if (keygen.appendf("%.*sdropbearkey", int(slash + 1), dropbear.data()).isError()) return Error::NoMem;
                Launch l;
                Error  err = l.prepare(keygen.view());
                if (err.isError()) return err;
                for (StringView a :
                     {StringView("dropbearkey"), StringView("-t"), StringView("ed25519"), StringView("-f"), key.view()})
                        l.add(a);
                l.finish();
                bmdsInfo("ssh: making the host key %s", key.cStr());
                const Result<int> status = state->launcher->run(l.request);
                if (status.isError()) return status.error();
                return status.value() == 0 ? Error::Ok : Error::IoError;
        }

        // Starts the server with the settings now: its pid, or 0.
        int32_t startServer() {
                String dropbear(MemSpace::Psram);
                if (!findProgram("dropbear", dropbear)) {
                        bmdsWarn("ssh: no dropbear on shell/path (install the dropbear package)");
                        return 0;
                }
                String dir(MemSpace::Psram), key(MemSpace::Psram), keysTemplate(MemSpace::Psram), keys(MemSpace::Psram),
                        port(MemSpace::Psram);
                const Result<int32_t> portValue = SysConf::getInt(StringView(PortKey));
                const Result<bool>    password = SysConf::getBool(StringView(PasswordKey));
                Error                 err = SysConf::get(StringView(HostKeysKey), dir);
                if (err.isOk()) err = key.appendf("%s/%s", dir.cStr(), HostKeyName);
                if (err.isOk()) err = SysConf::get(StringView(AuthorizedKey), keysTemplate);
                if (err.isOk()) err = SysConf::expand(keysTemplate.view(), keys);
                if (err.isOk()) err = port.appendf("%ld", long(portValue.isOk() ? portValue.value() : 22));
                if (err.isOk()) err = ensureHostKey(dropbear.view(), key, dir);
                if (err.isError()) {
                        bmdsWarn("ssh: no host key %s: %s", key.cStr(), err.desc());
                        return 0;
                }
                Launch l;
                if (l.prepare(dropbear.view()).isError()) return 0;
                // argv[0] the full path: the server runs itself again for each connection.
                for (StringView a : {dropbear.view(), StringView("-F"), StringView("-p"), port.view(), StringView("-r"),
                                     key.view(), StringView("-D"), keys.view()})
                        l.add(a);
                if (password.isOk() && !password.value()) l.add(StringView("-s"));
                l.finish();
                l.request.ended = &state->wake;
                const Result<int32_t> pid = state->launcher->start(l.request);
                if (pid.isError()) {
                        bmdsWarn("ssh: %s: %s", dropbear.cStr(), pid.error().desc());
                        return 0;
                }
                bmdsInfo("ssh: the server runs on port %s (pid %ld)", port.cStr(), long(pid.value()));
                NetMdns::setSshPort(static_cast<uint16_t>(portValue.isOk() ? portValue.value() : 22));
                return pid.value();
        }

        bool isRunning(int32_t pid) {
                return pid > 0 && state->launcher->sendSignal(pid, 0).isOk();
        }

        // Ends the server (SIGTERM, then SIGKILL); its sessions go on.
        void stopServer(int32_t pid) {
                NetMdns::setSshPort(0);
                if (state->launcher->sendSignal(pid, SigTerm).isError()) return;
                for (int round = 0; round < 2; ++round) {
                        const Deadline deadline(StopWaitMs);
                        while (isRunning(pid)) {
                                if (state->wake.acquire(deadline).isError()) break;
                        }
                        if (!isRunning(pid)) break;
                        (void)state->launcher->sendSignal(pid, SigKill);
                }
                bmdsInfo("ssh: the server stopped");
                return;
        }

        void drainWatcher() {
                SysConf::Id id;
                while (state->watcher.next(id)) {}
                (void)state->watcher.overflowed();
                return;
        }

        void serve(void *) {
                int32_t      pid = 0;
                unsigned int retryMs = 0; // waiting this long before starting again (0: start now)
                int64_t      startedUs = 0;
                for (;;) {
                        drainWatcher();
                        state->changed.store(false);
                        if (!enabled()) break;
                        if (pid == 0 && retryMs == 0) {
                                pid = startServer();
                                startedUs = (Clock::monotonicNs() / 1000);
                                if (pid == 0) retryMs = FirstRetryMs;
                        }
                        const Error woke = state->wake.acquire(retryMs != 0 && pid == 0 ? retryMs : WaitForever);
                        if (woke == Error::Timeout) {
                                retryMs = retryMs * 2 > MaxRetryMs ? MaxRetryMs : retryMs * 2;
                                if (pid == 0) {
                                        pid = startServer();
                                        startedUs = (Clock::monotonicNs() / 1000);
                                        if (pid != 0) retryMs = 0;
                                }
                                continue;
                        }
                        if (state->changed.load()) {
                                if (pid != 0) stopServer(pid);
                                pid = 0;
                                retryMs = 0;
                                continue;
                        }
                        if (pid != 0 && !isRunning(pid)) {
                                const bool steady =
                                        (Clock::monotonicNs() / 1000) - startedUs >= int64_t(SteadyMs) * 1000;
                                bmdsWarn("ssh: the server ended; starting it again");
                                pid = 0;
                                retryMs = steady || retryMs == 0 ? FirstRetryMs : retryMs;
                        }
                }
                if (pid != 0) stopServer(pid);
                {
                        Mutex::Locker lock(state->lock, WaitForever);
                        state->running = false;
                        state->thread.detach();
                }
                return;
        }

        // On and not running: the thread (none while off).
        void startThread() {
                Mutex::Locker lock(state->lock, WaitForever);
                if (state->running || !enabled()) return;
                Thread::Config config;
                config.name = "sshd";
                config.stackBytes = 6144;
                config.stackSpace = MemSpace::Internal; // it may make the host key's directory on /boot (flash)
                config.kernel = true;
                const Error err = state->thread.start(config, &serve, nullptr);
                if (err.isError()) {
                        bmdsErr("ssh: no thread: %s", err.desc());
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

} // namespace

Error SshService::init(ProgramLauncher &launcher) {
        state.create();
        state->launcher = &launcher;
        static constexpr Config::Param Params[] = {
                {"net/ssh/server", Config::Type::Bool, "false", "Run the SSH server (dropbear, from the card)", 0, 0,
                 nullptr, nullptr},
                {"net/ssh/port", Config::Type::Int, "22", "The SSH server's port", 1, 65535, nullptr, nullptr},
                {"net/ssh/password-login", Config::Type::Bool, "true",
                 "Allow logins with system/password (never without one)", 0, 0, nullptr, nullptr},
                {"net/ssh/host-keys", Config::Type::Path, "/boot/ssh", "Where the SSH server's host keys are", 0, 255,
                 nullptr, nullptr},
                {"net/ssh/authorized-keys-dir", Config::Type::String, "${HOME}/.ssh",
                 "The directory with authorized_keys (OpenSSH's format): a template (${HOME})", 0, 255, nullptr,
                 checkTemplate},
        };
        for (const Config::Param &p : Params) {
                const Result<SysConf::Id> id = SysConf::declareKernel(p);
                if (id.isError()) {
                        bmdsErr("net: setting %s not declared: %s", p.key, id.error().desc());
                        return id.error();
                }
        }
        const Error err = state->watcher.start(StringView("net/ssh"), &settingsChanged, nullptr);
        if (err.isError()) return err;
        startThread();
        return Error::Ok;
}

BMDS_NAMESPACE_END
