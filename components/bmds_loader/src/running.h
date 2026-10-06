/**
 * @file      running.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program while it runs (target only): what its system calls reach
 * through the task's thread-local pointer, and how it ends.
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
#include <bmds/loader.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/task.h>
#include <bmds/taskowner.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A running program: its image, its task, its process, and the
 *        vfork children running in its task (devplan/programs.md §13).
 *
 * Lives in `Psram` from its start until its task has been stopped (by the
 * loader's stop queue, after it ends).
 *
 * Killing (a signal that ends the program) must never stop the task inside
 * a system call, where it may hold a kernel lock: every slot is bracketed by
 * `enter()` and `leave()` (a `KernelCall`). The killer sets `killStatus` and
 * `kill`, suspends the task and looks at `inKernel`: clear, the task is in
 * program code, and the killer ends it; set, it is resumed (its interrupt
 * wakes the call) and ends itself in `leave()`. The two flags are a Dekker
 * handshake (sequentially consistent): if the killer reads `inKernel`
 * clear, the task can only be past its last kernel work. Ending sets
 * `inKernel` for good.
 *
 * Signals (devplan/programs.md §15) are each Process's; `leave()` ends the
 * program when one that ends it is due, and sets the program's hint word
 * while one is due for the current process (the SDK then takes it).
 *
 * Stopping (devplan/processes.md §3.7.1) uses the same handshake: the
 * stopper holds the process's interrupt, suspends the task and reads
 * `inKernel`: clear, the task stays suspended (`suspendedForStop`) until
 * continued; set, it is resumed and parks in its waits or in `leave()`.
 *
 * It is the owner of the process's tasks (`TaskOwner`, devplan/processes.md
 * §3.1): its own task, and the `Thread`s they start, which run kernel code
 * (a built-in program's) and share the process's interrupt. The process
 * ends once they have: a built-in program joins its threads before it
 * returns; any left are interrupted and waited for. A built-in program
 * reaches its process through it (`ProcessControl`).
 */
struct Running final : TaskOwner, ProcessControl {
                /** @brief How deep vfork children may nest in one task. */
                static constexpr int MaxVforkDepth = 2;

                struct Child {
                                Process *process;
                                int32_t  pid;
                };

                Process              *process = nullptr; // the program's own (memory is always its)
                int32_t               pid = 0;
                Process              *current = nullptr; // process, or the innermost vfork child
                int32_t               currentPid = 0;
                Child                 children[MaxVforkDepth] = {};
                int                   depth = 0;
                uint8_t              *image = nullptr;
                const ProgramEntry   *builtin = nullptr; // a program built in (/bin/NAME): no image
                void                 *data = nullptr;    // the builtin's (Context::data)
                uintptr_t             entry = 0;         // the program's _start
                Task                  task;
                Running              *nextStop = nullptr; // in the stop queue
                volatile uint32_t    *hint = nullptr;     // the program's (sig_setup): a signal is due
                std::atomic<int32_t>  killStatus{0};      // what a kill ends it with
                std::atomic<bool>     inKernel{false};
                std::atomic<bool>     kill{false};
                bool                  suspendedForStop = false;           // under the process table's lock
                char                  command[Loader::CommandBytes] = {}; // argv joined, cut (ps)
                Mutex                 threadsLock;                        // the fields below
                Member               *threads = nullptr;                  // its Threads (not its own task)
                int                   threadCount = 0;
                bool                  ending = false; // no more Threads; the last one releases:
                Semaphore             threadsGone{1, 0};

                /** @brief The running program of the calling task. */
                static Running &self() { return *static_cast<Running *>(TaskOwner::current()); }

                /** @brief The running program of the calling task, or null (a kernel task). */
                static Running *ofCurrentTask() { return static_cast<Running *>(TaskOwner::current()); }

                Error attach(Member &member) override;
                void  detach(Member &member) override;

                int32_t processId() const override { return pid; }
                Error   setSignalAction(int sig, SignalAction action) override;
                bool    isEnding() const override {
                        return kill.load() || (process != nullptr && process->signals().terminateSignal() != 0);
                }
                Result<int32_t> waitChild(int32_t child, uint32_t flags, int32_t &status) override;
                void            disownChild(int32_t child) override;
                Error           signal(int32_t target, int sig) override;
                Error           setForeground(int32_t group) override;

                /** @brief No more threads; waits for those left, interrupting them first. */
                void endThreads();

                /**
                 * @brief Ends the calling program with wait status @p status: its
                 *        children in the task, its process and its image go, its
                 *        task is queued to be stopped.
                 */
                [[noreturn]] void finish(int32_t status);

                /** @brief Frees what the program holds and records its end (lock-free of the table). */
                void end(int32_t status);

                /** @brief A system call starts. */
                void enter() {
                        inKernel.store(true);
                        current->signals().prepare();
                        return;
                }

                /**
                 * @brief A system call ends: back to program code, unless a signal
                 *        or a kill ends the program.
                 */
                void leave();

                /** @brief The Process of @p pid in this task (its own, a vfork child's), or null. */
                Process *processOf(int32_t pid) const;

                // The process and signal calls (loader.cpp); results as the slots'.
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
                int32_t group(uint32_t op, int32_t target, int32_t pgid);
                int32_t termForeground(int32_t fd, int32_t pgid);

        private:
                int32_t endChild(int32_t waitStatus);
                void    setCurrent(Process *p, int32_t id);
};

/**
 * @brief Brackets one system call (see `Running`); gives the Process it
 *        acts on (a vfork child's while one runs).
 * @par Thread Safety
 * The program's own task only.
 */
class KernelCall {
        public:
                KernelCall() : _run(Running::self()) { _run.enter(); }
                ~KernelCall() { _run.leave(); }
                KernelCall(const KernelCall &) = delete;
                KernelCall &operator=(const KernelCall &) = delete;

                /** @brief The Process the call acts on. */
                Process *operator->() const { return _run.current; }

                /** @brief The program's own Process: memory calls act on it (a vfork child shares it). */
                Process *owner() const { return _run.process; }

                /** @brief The running program. */
                Running &running() const { return _run; }

        private:
                Running &_run;
};

BMDS_NAMESPACE_END
