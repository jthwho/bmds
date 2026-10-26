/**
 * @file      process.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `Process` (devplan/guiprograms.md §14): another program started with its input, output and
 * errors as pipes, read and written as a `Socket` is, waited for, signalled, and its output
 * lines and its end called back in the loop (`app.run()`).
 *
 * Ends are learnt without polling: a `SIGCHLD` handler writes to a pipe the loop polls, and each
 * process kept is asked with `WNOHANG`. Every process started is kept (a reference) until it has
 * ended, its output has ended and its callbacks have been called, so one let go of while running
 * is still reaped; its callbacks are let go of at its end, so a callback naming its process
 * keeps no cycle.
 */

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

extern char **environ;

namespace angel {

        namespace {

                constexpr size_t Chunk = 1024;
                constexpr size_t MaxProcesses = 32;

                class Process;

                // One of a process's outputs (its output, its errors): a pipe and what was read from
                // it and not yet taken.
                struct Stream {
                                LoopSource source{};
                                Text       buffer;
                                size_t     start = 0; // taken so far from buffer

                                bool   open() const { return source.fd >= 0; }
                                size_t buffered() const { return buffer.length() - start; }
                                bool   ended() const { return !open() && buffered() == 0; }

                                // Bytes read (non-blocking), 0 at the end (closed), -1 if none now.
                                int pull() {
                                        if (!open()) return 0;
                                        char          chunk[Chunk];
                                        const ssize_t n = read(source.fd, chunk, sizeof(chunk));
                                        if (n > 0) {
                                                buffer.append(chunk, size_t(n));
                                                return int(n);
                                        }
                                        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return -1;
                                        close();
                                        return 0;
                                }

                                void close() {
                                        if (!open()) return;
                                        removeLoopSource(&source);
                                        ::close(source.fd);
                                        source.fd = -1;
                                        return;
                                }

                                // Up to @p most bytes taken from the buffer into @p out.
                                void take(Text &out, size_t most) {
                                        const size_t n = buffered() < most ? buffered() : most;
                                        out.append(buffer.data() + start, n);
                                        start += n;
                                        if (start == buffer.length()) {
                                                buffer.clear();
                                                start = 0;
                                        } else if (start >= 4096 && start * 2 >= buffer.length()) {
                                                char *old = buffer.release();
                                                buffer.append(old + start, strlen(old + start));
                                                free(old);
                                                start = 0;
                                        }
                                        return;
                                }

                                // The next line taken (without its newline) into @p out: false if no
                                // whole line is buffered (the rest, at the end, is a line).
                                bool takeLine(Text &out) {
                                        const char *from = buffer.data() + start;
                                        const char *nl = static_cast<const char *>(memchr(from, '\n', buffered()));
                                        if (nl == nullptr) {
                                                if (open() || buffered() == 0) return false;
                                                take(out, buffered());
                                                return true;
                                        }
                                        const size_t length = size_t(nl - from);
                                        take(out, length);
                                        start += 1; // the newline
                                        if (start == buffer.length()) {
                                                buffer.clear();
                                                start = 0;
                                        }
                                        return true;
                                }
                };

                class Process {
                        public:
                                pid_t              pid = -1;
                                int                input = -1;
                                int                raw = 0; // its wait status, once ended
                                bool               ended = false;
                                bool               kept = false; // in `processes` (a reference)
                                Stream             out, err;
                                asIScriptFunction *onOutput = nullptr;
                                asIScriptFunction *onErrors = nullptr;
                                asIScriptFunction *onExit = nullptr;

                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                        return;
                                }
                                int  refs() const { return _refs; }
                                bool hasCallbacks() const {
                                        return onOutput != nullptr || onErrors != nullptr || onExit != nullptr;
                                }
                                void dropCallbacks() {
                                        asIScriptFunction **all[] = {&onOutput, &onErrors, &onExit};
                                        for (asIScriptFunction **fn : all) {
                                                if (*fn != nullptr) (*fn)->Release();
                                                *fn = nullptr;
                                        }
                                        return;
                                }
                                // Ended, and its output read to the end.
                                bool done() const { return ended && !out.open() && !err.open(); }

                        private:
                                ~Process() {
                                        dropCallbacks();
                                        out.close();
                                        err.close();
                                        if (input >= 0) close(input);
                                }
                                int _refs = 1;
                };

                Process   *processes[MaxProcesses];
                size_t     processCount = 0;
                int        wakeFds[2] = {-1, -1}; // SIGCHLD's pipe: the loop reads, the handler writes
                LoopSource childSource{};

                int statusOf(int raw) {
                        if (WIFEXITED(raw)) return WEXITSTATUS(raw);
                        if (WIFSIGNALED(raw)) return 128 + WTERMSIG(raw);
                        return -1;
                }

                void onChild(int) {
                        const int saved = errno;
                        if (wakeFds[1] >= 0) (void)!write(wakeFds[1], "c", 1);
                        errno = saved;
                }

                // The loop woken (a byte in SIGCHLD's pipe): to call back what happened meanwhile.
                void wakeLoop() {
                        if (wakeFds[1] >= 0) (void)!write(wakeFds[1], "w", 1);
                        return;
                }

                // @p p asked whether it has ended (no wait).
                void reap(Process *p) {
                        if (p->ended) return;
                        int         raw = 0;
                        const pid_t r = waitpid(p->pid, &raw, WNOHANG);
                        if (r == p->pid) {
                                p->raw = raw;
                                p->ended = true;
                        } else if (r < 0 && errno == ECHILD) {
                                p->raw = 0xff00; // someone else reaped it: 255
                                p->ended = true;
                        }
                        // Nothing reads its input now: the descriptor back (a program has 16).
                        if (p->ended && p->input >= 0) {
                                close(p->input);
                                p->input = -1;
                        }
                        return;
                }

                // A process with callbacks holds the loop (and runs it at the script's end).
                void updateHold() {
                        childSource.hold = LoopHold::Nothing;
                        for (size_t i = 0; i < processCount; ++i)
                                if (processes[i]->hasCallbacks()) childSource.hold = LoopHold::RunAndEnd;
                        return;
                }

                // No longer kept: its outputs and callbacks let go of, and the list's reference.
                void unkeep(Process *p) {
                        for (size_t i = 0; i < processCount; ++i) {
                                if (processes[i] != p) continue;
                                processes[i] = processes[--processCount];
                                break;
                        }
                        p->kept = false;
                        p->out.close();
                        p->err.close();
                        p->dropCallbacks();
                        if (processCount == 0) removeLoopSource(&childSource);
                        updateHold();
                        p->release();
                        return;
                }

                void drainWake() {
                        char bytes[16];
                        while (read(wakeFds[0], bytes, sizeof(bytes)) > 0) {}
                        return;
                }

                // In the loop: @p p's whole lines called back (the rest at the end), then its end
                // once its output has ended; let go of once done, unless the script waits on it.
                void deliver(Process *p) {
                        p->addRef(); // a callback may let go of it
                        for (int which = 0; which < 2; ++which) {
                                Stream            &s = which == 0 ? p->out : p->err;
                                asIScriptFunction *fn = which == 0 ? p->onOutput : p->onErrors;
                                if (fn == nullptr) {
                                        // Nobody can read it (only the list and this hold it): not kept.
                                        if (p->refs() <= 2) {
                                                s.buffer.clear();
                                                s.start = 0;
                                        }
                                        continue;
                                }
                                Text line;
                                while (fn == (which == 0 ? p->onOutput : p->onErrors) && s.takeLine(line)) {
                                        const CallArg arg{line.data(), line.length(), 0};
                                        fn->AddRef();
                                        callScript(fn, &arg, 1);
                                        fn->Release();
                                        line.clear();
                                }
                        }
                        if (p->kept && p->done()) {
                                asIScriptFunction *fn = p->onExit;
                                p->onExit = nullptr;
                                if (fn != nullptr) {
                                        const CallArg arg{nullptr, 0, statusOf(p->raw)};
                                        callScript(fn, &arg, 1);
                                        fn->Release();
                                }
                                if (p->kept) unkeep(p);
                        }
                        p->release();
                        return;
                }

                void streamReady(LoopSource *source) {
                        Process *p = static_cast<Process *>(source->context);
                        Stream  &s = source == &p->out.source ? p->out : p->err;
                        while (s.pull() > 0) {}
                        deliver(p);
                        return;
                }

                void childReady(LoopSource *) {
                        drainWake();
                        Process     *now[MaxProcesses];
                        const size_t count = processCount;
                        for (size_t i = 0; i < count; ++i) {
                                now[i] = processes[i];
                                now[i]->addRef();
                                reap(now[i]);
                        }
                        for (size_t i = 0; i < count; ++i) {
                                if (now[i]->kept) deliver(now[i]);
                                now[i]->release();
                        }
                        return;
                }

                // SIGCHLD heard from now on (once).
                bool watchChildren() {
                        if (wakeFds[0] >= 0) return true;
                        if (pipe2(wakeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
                                wakeFds[0] = wakeFds[1] = -1;
                                return false;
                        }
                        struct sigaction a;
                        memset(&a, 0, sizeof(a));
                        a.sa_handler = onChild;
                        a.sa_flags = SA_RESTART | SA_NOCLDSTOP;
                        sigemptyset(&a.sa_mask);
                        sigaction(SIGCHLD, &a, nullptr);
                        childSource.fd = wakeFds[0];
                        childSource.ready = childReady;
                        childSource.context = nullptr;
                        childSource.hold = LoopHold::Nothing;
                        return true;
                }

                // Those ended that only the list holds, let go of (a script starting processes in a
                // loop of its own, never running app.run()).
                void sweep() {
                        for (size_t i = processCount; i-- > 0;) {
                                Process *p = processes[i];
                                reap(p);
                                if (p->ended && p->refs() == 1 && !p->hasCallbacks()) unkeep(p);
                        }
                        return;
                }

                // Starts @p argv (@p path found on the PATH when @p search). @return The process, or
                // null with an exception raised.
                Process *start(const char *path, char *const *argv, bool search, const char *what) {
                        sweep();
                        if (processCount == MaxProcesses) {
                                raise("Too many processes at once (%u)", unsigned(MaxProcesses));
                                return nullptr;
                        }
                        if (!watchChildren()) {
                                raise("Can't run '%s': %s", what, strerror(errno));
                                return nullptr;
                        }
                        int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
                        if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
                                const int e = errno;
                                const int all[] = {in[0], in[1], out[0], out[1], err[0], err[1]};
                                for (int fd : all)
                                        if (fd >= 0) close(fd);
                                raise("Can't run '%s': %s", what, strerror(e));
                                return nullptr;
                        }
                        posix_spawn_file_actions_t actions;
                        posix_spawn_file_actions_init(&actions);
                        posix_spawn_file_actions_adddup2(&actions, in[0], 0);
                        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
                        posix_spawn_file_actions_adddup2(&actions, err[1], 2);
                        fflush(stdout);
                        pid_t     pid = -1;
                        const int r = search ? posix_spawnp(&pid, path, &actions, nullptr, argv, environ)
                                             : posix_spawn(&pid, path, &actions, nullptr, argv, environ);
                        posix_spawn_file_actions_destroy(&actions);
                        close(in[0]);
                        close(out[1]);
                        close(err[1]);
                        if (r != 0) {
                                close(in[1]);
                                close(out[0]);
                                close(err[0]);
                                raise("Can't run '%s': %s", what, strerror(r));
                                return nullptr;
                        }
                        fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
                        fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL) | O_NONBLOCK);
                        Process *p = new (std::nothrow) Process();
                        if (p == nullptr) {
                                close(in[1]);
                                close(out[0]);
                                close(err[0]);
                                raise("Out of memory");
                                return nullptr;
                        }
                        p->pid = pid;
                        p->input = in[1];
                        p->out.source = {out[0], streamReady, p, LoopHold::Nothing};
                        p->err.source = {err[0], streamReady, p, LoopHold::Nothing};
                        // Kept until done (its reference); its outputs and its end heard in the loop.
                        p->addRef();
                        p->kept = true;
                        processes[processCount++] = p;
                        (void)addLoopSource(&childSource);
                        (void)addLoopSource(&p->out.source);
                        (void)addLoopSource(&p->err.source);
                        return p;
                }

                // Waits up to @p ms (-1: for ever) for either output, and reads what came (both are
                // read: a full pipe would stop the process). @return False if the time ran out.
                bool pullMore(Process *p, int ms) {
                        struct pollfd fds[2] = {{p->out.source.fd, POLLIN, 0}, {p->err.source.fd, POLLIN, 0}};
                        const int     r = poll(fds, 2, ms);
                        if (r == 0) return false;
                        if (r < 0) return errno == EINTR;
                        if (fds[0].revents != 0) (void)p->out.pull();
                        if (fds[1].revents != 0) (void)p->err.pull();
                        return true;
                }

                // Waits (up to @p ms, -1: for ever) until @p want has something buffered or has
                // ended. @return False if the time ran out.
                bool waitFor(Process *p, Stream &want, int ms) {
                        while (want.buffered() == 0 && want.open())
                                if (!pullMore(p, ms)) return false;
                        return true;
                }

                Process *self(asIScriptGeneric *gen) {
                        return static_cast<Process *>(gen->GetObject());
                }

                void returnText(asIScriptGeneric *gen, const Text &text) {
                        new (gen->GetAddressOfReturnLocation()) String(text.data(), text.length());
                }

                // ---- The script's methods

                void processShell(asIScriptGeneric *gen) {
                        const String *command = static_cast<const String *>(gen->GetArgAddress(0));
                        char         *argv[] = {const_cast<char *>("sh"), const_cast<char *>("-c"),
                                                const_cast<char *>(command->cStr()), nullptr};
                        gen->SetReturnAddress(start("/bin/sh", argv, false, command->cStr()));
                }

                void processProgram(asIScriptGeneric *gen) {
                        const String       *program = static_cast<const String *>(gen->GetArgAddress(0));
                        const CScriptArray *args = static_cast<const CScriptArray *>(gen->GetArgAddress(1));
                        const asUINT        count = args != nullptr ? args->GetSize() : 0;
                        char              **argv = static_cast<char **>(malloc((count + 2) * sizeof(char *)));
                        if (argv == nullptr) {
                                raise("Out of memory");
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        argv[0] = const_cast<char *>(program->cStr());
                        for (asUINT i = 0; i < count; ++i)
                                argv[i + 1] = const_cast<char *>(static_cast<const String *>(args->At(i))->cStr());
                        argv[count + 1] = nullptr;
                        gen->SetReturnAddress(start(program->cStr(), argv, true, program->cStr()));
                        free(argv);
                }

                void processAddRef(asIScriptGeneric *gen) {
                        self(gen)->addRef();
                }

                void processRelease(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        // The script's last: kept no longer if it's done and calls nothing back.
                        if (p->refs() == 2 && p->kept && !p->hasCallbacks()) {
                                reap(p);
                                if (p->ended) unkeep(p);
                        }
                        p->release();
                }

                void processRead(asIScriptGeneric *gen) {
                        Process  *p = self(gen);
                        const int most = int(gen->GetArgDWord(0));
                        Text      text;
                        if (most > 0) {
                                (void)waitFor(p, p->out, -1);
                                p->out.take(text, size_t(most));
                        }
                        returnText(gen, text);
                }

                void processReadLine(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        Text     line;
                        while (!p->out.takeLine(line) && p->out.open() && pullMore(p, -1)) {}
                        returnText(gen, line);
                }

                void processReadAll(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        while (p->out.open() && pullMore(p, -1)) {}
                        Text text;
                        p->out.take(text, p->out.buffered());
                        returnText(gen, text);
                }

                void processReadErrors(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        while (p->err.pull() > 0) {}
                        Text text;
                        p->err.take(text, p->err.buffered());
                        returnText(gen, text);
                }

                void processEof(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        (void)waitFor(p, p->out, -1);
                        gen->SetReturnByte(p->out.ended());
                }

                void processReady(asIScriptGeneric *gen) {
                        Process  *p = self(gen);
                        const int ms = int(gen->GetArgDWord(0));
                        (void)p->out.pull();
                        gen->SetReturnByte(waitFor(p, p->out, ms < 0 ? 0 : ms));
                }

                void writeAll(Process *p, const Text &text) {
                        reap(p);
                        if (p->input < 0) {
                                raise(p->ended ? "The process has ended" : "The process's input is closed");
                                return;
                        }
                        // Its end is an error here, not a signal that ends the script; ignored
                        // only meanwhile, so the processes started don't inherit it ignored.
                        struct sigaction ignore, old;
                        memset(&ignore, 0, sizeof(ignore));
                        ignore.sa_handler = SIG_IGN;
                        sigaction(SIGPIPE, &ignore, &old);
                        const char *data = text.data();
                        size_t      left = text.length();
                        int         e = 0;
                        while (left != 0) {
                                const ssize_t n = write(p->input, data, left);
                                if (n < 0) {
                                        if (errno == EINTR) continue;
                                        e = errno;
                                        break;
                                }
                                data += n;
                                left -= size_t(n);
                        }
                        sigaction(SIGPIPE, &old, nullptr);
                        if (e == EPIPE)
                                raise("The process doesn't read its input any more");
                        else if (e != 0)
                                raise("Can't write to the process: %s", strerror(e));
                        return;
                }

                void processWrite(asIScriptGeneric *gen) {
                        Text text;
                        formatValue(gen->GetEngine(), text, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        writeAll(self(gen), text);
                }

                void processWriteLine(asIScriptGeneric *gen) {
                        Text text;
                        formatValue(gen->GetEngine(), text, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        text.append("\n", 1);
                        writeAll(self(gen), text);
                }

                void processCloseInput(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        if (p->input >= 0) close(p->input);
                        p->input = -1;
                }

                void processWait(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        // Its outputs read meanwhile; its end heard by SIGCHLD's pipe, which the loop
                        // is woken by again afterwards (other processes' ends it took).
                        for (reap(p); !p->ended; reap(p)) {
                                struct pollfd fds[3] = {{p->out.source.fd, POLLIN, 0},
                                                        {p->err.source.fd, POLLIN, 0},
                                                        {wakeFds[0], POLLIN, 0}};
                                if (poll(fds, 3, -1) < 0 && errno != EINTR) break;
                                if (fds[0].revents != 0) (void)p->out.pull();
                                if (fds[1].revents != 0) (void)p->err.pull();
                                if (fds[2].revents != 0) drainWake();
                        }
                        // What it wrote before it ended (not what anything it started still writes).
                        while (p->out.pull() > 0) {}
                        while (p->err.pull() > 0) {}
                        wakeLoop();
                        gen->SetReturnDWord(uint32_t(p->ended ? statusOf(p->raw) : -1));
                }

                void processSignal(asIScriptGeneric *gen) {
                        Process  *p = self(gen);
                        const int sig = int(gen->GetArgDWord(0));
                        reap(p);
                        if (p->ended) return;
                        if (kill(p->pid, sig) != 0) raise("Can't signal the process: %s", strerror(errno));
                }

                void processPid(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(self(gen)->pid));
                }

                void processRunning(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        reap(p);
                        gen->SetReturnByte(!p->ended);
                }

                void processExitStatus(asIScriptGeneric *gen) {
                        Process *p = self(gen);
                        reap(p);
                        gen->SetReturnDWord(uint32_t(p->ended ? statusOf(p->raw) : -1));
                }

                // A callback set (the argument's reference taken), its process's output so far
                // and its end called back at the loop's next turn.
                void setCallback(asIScriptGeneric *gen, asIScriptFunction *Process::*which) {
                        Process           *p = self(gen);
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (!p->kept) { // done already
                                if (fn != nullptr) fn->Release();
                                return;
                        }
                        if (p->*which != nullptr) (p->*which)->Release();
                        p->*which = fn;
                        updateHold();
                        wakeLoop();
                }

                void processOnOutput(asIScriptGeneric *gen) {
                        setCallback(gen, &Process::onOutput);
                }
                void processOnErrors(asIScriptGeneric *gen) {
                        setCallback(gen, &Process::onErrors);
                }
                void processOnExit(asIScriptGeneric *gen) {
                        setCallback(gen, &Process::onExit);
                }

        } // namespace

        void registerProcess(asIScriptEngine *engine) {
                check(engine->RegisterObjectType("Process", 0, asOBJ_REF), "Process");
                behaviour(engine, "Process", asBEHAVE_FACTORY, "Process@ f(const string &in command)", processShell,
                          "Runs a shell command line, its input, output and errors pipes of the script's.");
                behaviour(engine, "Process", asBEHAVE_FACTORY,
                          "Process@ f(const string &in program, const string[] &in arguments)", processProgram,
                          "Runs a program (found on the PATH) with these arguments, no shell between.");
                behaviour(engine, "Process", asBEHAVE_ADDREF, "void f()", processAddRef, nullptr);
                behaviour(engine, "Process", asBEHAVE_RELEASE, "void f()", processRelease, nullptr);
                method(engine, "Process", "string read(int most = 4096)", processRead,
                       "What it has written, up to most bytes (waiting for some); \"\" at the end.");
                method(engine, "Process", "string readLine()", processReadLine,
                       "The next line it writes, without its newline.");
                method(engine, "Process", "string readAll()", processReadAll, "Everything it writes until its end.");
                method(engine, "Process", "string readErrors()", processReadErrors,
                       "What it has written to its errors so far (no wait).");
                method(engine, "Process", "bool eof()", processEof,
                       "Its output ended and everything was read (waits to know).");
                method(engine, "Process", "bool ready(int ms = 0)", processReady,
                       "Something can be read now (or the end), waiting up to ms for it; 0 only looks.");
                method(engine, "Process", "void write(const ?&in)", processWrite,
                       "Writes a value to its input, as print() shows it.");
                method(engine, "Process", "void writeLine(const ?&in)", processWriteLine,
                       "Writes a value to its input, then a newline.");
                method(engine, "Process", "void closeInput()", processCloseInput,
                       "Closes its input: it reads the end.");
                method(engine, "Process", "int wait()", processWait,
                       "Waits for it to end, keeping what it writes meanwhile: its exit status.");
                method(engine, "Process", "void signal(int sig = 15)", processSignal,
                       "Sends it a signal (15, the default: asked to end; 9: killed).");
                method(engine, "Process", "int get_pid() const property", processPid, "Its process id.");
                method(engine, "Process", "bool get_running() const property", processRunning, "It hasn't ended yet.");
                method(engine, "Process", "int get_exitStatus() const property", processExitStatus,
                       "Its exit status once it ended (0: it worked; 128 + N: killed by signal N), else -1.");
                method(engine, "Process", "void onOutput(StringCallback@ callback)", processOnOutput,
                       "Calls back with each line it writes (without its newline) while app.run() runs.");
                method(engine, "Process", "void onErrors(StringCallback@ callback)", processOnErrors,
                       "Calls back with each line it writes to its errors while app.run() runs.");
                method(engine, "Process", "void onExit(IntCallback@ callback)", processOnExit,
                       "Calls back with its exit status once it has ended and its output has been called back.");
                return;
        }

        void releaseProcesses() {
                while (processCount != 0) unkeep(processes[processCount - 1]);
                return;
        }

} // namespace angel
