/**
 * @file      running.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program while it runs (target only): its process's record and its
 * threads' records, what its system calls reach through the task's owner
 * pointer, and how they end (devplan/processes.md §3.10).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <bmds/config.h>
#include <bmds/process.h>
#include <bmds/builtinprograms.h>
#include <bmds/futex.h>
#include <bmds/loader.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/signals.h>
#include <bmds/task.h>
#include <bmds/taskowner.h>

BMDS_NAMESPACE_BEGIN

struct Running;
struct ProgramThread;

/**
 * @brief The loader's task owners: a program thread's record, or a
 *        process's (the owner of its kernel `Thread`s). FreeRTOS pointer 1.
 */
struct ProcessOwner : TaskOwner {
                Running       *run = nullptr;    ///< The process.
                ProgramThread *thread = nullptr; ///< This task's record; null for a kernel `Thread`.

                /** @brief The calling task's owner, or null (a kernel task). */
                static ProcessOwner *current() { return static_cast<ProcessOwner *>(TaskOwner::current()); }

        protected:
                ProcessOwner() = default;
                ~ProcessOwner() = default;
};

/**
 * @brief One program thread: a task of a process that runs program code
 *        (or a built-in program's own task), with what is its own: the
 *        kill/stop handshake, its signal part and hint word, its clear word,
 *        and the vfork children running on it (devplan/programs.md §13).
 *
 * Killing (a signal that ends the process) must never stop the task inside
 * a system call, where it may hold a kernel lock: every slot is bracketed by
 * `enter()` and `leave()` (a `KernelCall`). The killer sets `kill`, suspends
 * the task and looks at `inKernel`: clear, the task is in program code, and
 * the killer ends it; set, it is resumed (its interrupt wakes the call) and
 * ends itself in `leave()`. The two flags are a Dekker handshake
 * (sequentially consistent): if the killer reads `inKernel` clear, the task
 * can only be past its last kernel work. Ending sets `inKernel` for good.
 *
 * Stopping (devplan/processes.md §3.7.1) uses the same handshake: the
 * stopper holds the thread's interrupt, suspends the task and reads
 * `inKernel`: clear, the task stays suspended (`suspendedForStop`) until
 * continued; set, it is resumed and parks in its waits or in `leave()`.
 *
 * The first thread's record is part of the `Running`; the others' are in
 * `Psram`, their stacks the kernel's (no kernel code runs on program
 * memory). A record lives until the stop queue has stopped its task.
 */
struct ProgramThread final : ProcessOwner {
                /** @brief How deep vfork children may nest in one thread. */
                static constexpr int MaxVforkDepth = 2;

                struct Child {
                                Process *process;
                                int32_t  pid;
                };

                Task                  task;
                int32_t               tid = 0;
                Process              *current = nullptr; // the process, or the innermost vfork child
                int32_t               currentPid = 0;
                Child                 children[MaxVforkDepth] = {};
                int                   depth = 0;
                Signals::Thread      *signals = nullptr; // its part of the process's signals
                Signals::Thread       own;               // that part, but for the first thread's
                volatile uint32_t    *hint = nullptr;    // the program's (sig_setup): a signal is due
                volatile int32_t     *clear = nullptr;   // thread_clear(): 0 and a wake once it has ended
                int32_t (*entry)(void *) = nullptr;      // a started thread's (thread_start)
                void                 *arg = nullptr;
                void                 *tls = nullptr;
                ProgramThread        *next = nullptr;       // the process's other threads
                ProgramThread        *nextFrozen = nullptr; // a sender's frozen list
                ProgramThread        *nextStop = nullptr;   // the stop queue
                std::atomic<bool>     inKernel{false};
                std::atomic<bool>     kill{false};
                bool                  suspendedForStop = false; // under the process table's lock
                bool                  ended = false;            // under the Running's threadsLock
                bool realtime = false; // raised by an audio device (audio_cycle): forgotten there before its task goes
                // Its scheduling (sched; devplan/processes.md §3.12), under the real-time lock:
                uint8_t  policy = BMDS_SCHED_OTHER;
                uint8_t  rtPriority = 0;      // 1-99 with FIFO or RR
                bool     resetOnFork = false; // what it starts begins as OTHER
                uint8_t  normalPriority = 0;  // its kernel priority without a real-time policy
                uint32_t throttled = 0;       // times held back by the real-time budget
                bool     begun = false;       // it has run: its task exists, a real-time one is counted

                /** @brief The calling task's record (a program thread's: system calls only). */
                static ProgramThread &self() { return *ProcessOwner::current()->thread; }

                /** @brief The calling task's record, or null (a kernel task or `Thread`). */
                static ProgramThread *ofCurrentTask() {
                        ProcessOwner *owner = ProcessOwner::current();
                        return owner != nullptr ? owner->thread : nullptr;
                }

                Error      attach(Member &member) override;
                void       detach(Member &member) override;
                TaskOwner *threadOwner() override;

                /** @brief Its part of what it acts on now: its own, or a vfork child's. */
                Signals::Thread &currentSignals() { return depth > 0 ? current->signals().mainThread() : *signals; }

                /** @brief A system call starts. */
                void enter() {
                        inKernel.store(true);
                        current->signals().prepare(currentSignals());
                        return;
                }

                /**
                 * @brief A system call ends: back to program code, unless a signal
                 *        or a kill ends the thread (parks while stopped).
                 */
                void leave();

                /**
                 * @brief Ends the calling thread: its vfork children, then itself; the
                 *        last thread's end ends the process with @p status (or the
                 *        kill's). Its task is queued to be stopped.
                 */
                [[noreturn]] void finish(int32_t status);

                /** @brief Ends the calling thread's process with @p status (`exit()`), and so the thread. */
                [[noreturn]] void endProcess(int32_t status);

                /** @brief Ends its vfork children with @p status (innermost first). */
                void endChildren(int32_t status);

                // The process, signal and thread calls (loader.cpp); results as the slots'.
                int32_t vfork();
                int32_t exec(const char *path, char *const *argv, char *const *envp);
                int32_t vforkExit(int32_t status);
                int32_t wait(int32_t child, int32_t *status, uint32_t flags);
                int32_t id(uint32_t which);
                int32_t sigSetup(volatile uint32_t *word);
                int32_t sigTake(bmds_siginfo_t *info);
                int32_t sigSuspend(uint32_t mask);
                int32_t sendSignal(int32_t target, int32_t sig);
                int32_t exitSignal(int32_t sig);
                int32_t           timer(uint32_t which, const bmds_timer_t *set, bmds_timer_t *old);
                int32_t group(uint32_t op, int32_t target, int32_t pgid);
                int32_t termForeground(int32_t fd, int32_t pgid);
                int32_t           termControl(int32_t fd, uint32_t op, uint32_t value);
                int32_t           startThread(const bmds_thread_t *spec);
                [[noreturn]] void exitThread(int32_t status, void *release);
                int32_t           killThread(int32_t target, int32_t sig, uint32_t flags);
                int32_t           setClear(volatile int32_t *word);
                int32_t           sched(uint32_t op, int32_t target, bmds_sched_t *param);

        private:
                int32_t endChild(int32_t waitStatus);
                void    setCurrent(Process *p, int32_t id);
                void    endOtherThreads(); // an exec's: the other threads end, and it waits for them
};

/**
 * @brief A running program's process: its image, its `Process`, its program
 *        threads (`ProgramThread`; the first one's record is part of it), and
 *        the kernel `Thread`s its tasks start (a built-in program's: they run
 *        kernel code and share the first thread's interrupt).
 *
 * Lives in `Psram` from its start until the stop queue has stopped its last
 * thread's task. The process ends once all its program threads have (the
 * last one ends it); a built-in program joins its kernel threads before it
 * returns, and any left are interrupted and waited for. A built-in program
 * reaches its process through it (`ProcessControl`).
 *
 * `threadsLock` covers the thread lists and counts; it is taken after the
 * process table's lock, never before it.
 */
struct Running final : ProcessOwner, ProcessControl {
                Process             *process = nullptr; // memory is always its (a vfork child shares it)
                int32_t              pid = 0;
                uint8_t             *image = nullptr;
                size_t               imageBytes = 0;    // its size (the profiler's modules)
                const ProgramEntry  *builtin = nullptr; // a program built in (/bin/NAME): no image
                void                *data = nullptr;    // the builtin's (Context::data)
                uintptr_t            entry = 0;         // the program's _start
                ProgramThread        main;              // the first thread
                std::atomic<int32_t> killStatus{0};     // what a kill ends it with
                std::atomic<bool>    killed{false};
                std::atomic<bool>    watchingStats{false};               // stats_control(WATCH): released with it
                uint64_t             cpuBaseUs = 0;                      // the program's before an exec
                char                 command[Loader::CommandBytes] = {}; // argv joined, cut (ps)
                Mutex                threadsLock;                        // the fields below
                Member              *threads = nullptr;                  // its kernel Threads
                int                  threadCount = 0;
                bool                 ending = false; // no more kernel Threads; the last one releases:
                Semaphore            threadsGone{1, 0};
                ProgramThread       *others = nullptr;    // program threads but the first
                int                  live = 1;            // program threads not ended
                int                  records = 1;         // thread records not yet stopped and freed
                uint64_t             endedCpuUs = 0;      // the time of its tasks that ended
                bool                 execWaiting = false; // an exec waits for the other threads to end:
                Semaphore            othersGone{1, 0};

                Running() {
                        run = this;
                        main.run = this;
                        main.thread = &main;
                }

                /** @brief The calling task's process, or null (a kernel task). */
                static Running *ofCurrentTask() {
                        ProcessOwner *owner = ProcessOwner::current();
                        return owner != nullptr ? owner->run : nullptr;
                }

                Error attach(Member &member) override;
                void  detach(Member &member) override;

                int32_t processId() const override { return pid; }
                Error   setSignalAction(int sig, SignalAction action) override;
                bool    isEnding() const override {
                        return killed.load() || (process != nullptr && process->signals().terminateSignal() != 0);
                }
                Result<int32_t> waitChild(int32_t child, uint32_t flags, int32_t &status) override;
                void            disownChild(int32_t child) override;
                Error           signal(int32_t target, int sig) override;
                Error           setForeground(int32_t group) override;

                /** @brief Calls @p fn with each program thread (under `threadsLock`). */
                template <typename Fn> void forEachThread(Fn &&fn) {
                        Mutex::Locker locker(threadsLock, WaitForever);
                        fn(main);
                        for (ProgramThread *t = others; t != nullptr; t = t->next) fn(*t);
                }

                /** @brief The Process of @p pid on one of its threads (its own, a vfork child's), or null. */
                Process *processOf(int32_t pid);

                /**
                 * @brief Returns the CPU time it has had, in µs: its live tasks', its
                 *        ended ones', and the program's before an exec (0: not counted).
                 */
                uint64_t cpuUs();

                /** @brief `cpuUs()` with `threadsLock` held. */
                uint64_t cpuUsLocked();

                /** @brief Stops watching the counters if it does (`stats_control`). */
                void releaseStats();

                /** @brief No more kernel threads; waits for those left, interrupting them first. */
                void endThreads();

                /**
                 * @brief @p gone has ended (it no longer runs program code): its clear
                 *        word is cleared and woken, it leaves the lists; the last
                 *        thread's end ends the process with @p status (a kill's
                 *        status first).
                 */
                void threadEnded(ProgramThread &gone, int32_t status);

                /** @brief Frees what the program holds and records its end (lock-free of the table). */
                void end(int32_t status);
};

/** @brief The futex table the program threads wait on (loader.cpp). */
FutexTable &programFutexes();

/**
 * @brief Brackets one system call (see `ProgramThread`); gives the Process it
 *        acts on (a vfork child's while one runs).
 * @par Thread Safety
 * The program thread's own task only.
 */
class KernelCall {
        public:
                KernelCall() : _thread(ProgramThread::self()) { _thread.enter(); }
                ~KernelCall() { _thread.leave(); }
                KernelCall(const KernelCall &) = delete;
                KernelCall &operator=(const KernelCall &) = delete;

                /** @brief The Process the call acts on. */
                Process *operator->() const { return _thread.current; }

                /** @brief The program's own Process: memory calls act on it (a vfork child shares it). */
                Process *owner() const { return _thread.run->process; }

                /** @brief The calling thread. */
                ProgramThread &thread() const { return _thread; }

        private:
                ProgramThread &_thread;
};

BMDS_NAMESPACE_END
