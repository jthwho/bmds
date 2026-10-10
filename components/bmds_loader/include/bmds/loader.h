/**
 * @file      loader.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/console.h>
#include <bmds/programlauncher.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Runs BMDS programs from files (devplan/programs.md): checks the
 *        file (`ProgramImage`), loads it into PSRAM at a 4096-byte-aligned
 *        address, relocates it, syncs the caches and runs it in a task of its
 *        own, with the caller's file descriptors, arguments and environment
 *        (`Process`), then frees everything it had.
 *
 * Every program is a process (`ProcessTable`); programs start others with
 * vfork and exec (devplan/programs.md §13). Processes are in process groups
 * and sessions; a console signals its foreground group (Ctrl-C, Ctrl-\, a
 * resize; a hangup its session), which the loader delivers from a task of
 * its own: a Ctrl-C after one that had no effect ends the group
 * (devplan/processes.md §3.6).
 *
 * Target only. The object is stateless: one serves every shell, and
 * several programs may run at once (pipeline stages). The processes are
 * the system's, set up once by `init()`.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class Loader final : public ProgramLauncher {
        public:
                constexpr Loader() = default;

                /** @brief Bytes of a process's command line kept for `ps` (cut to fit). */
                static constexpr size_t CommandBytes = 48;

                /** @brief Bytes of a task's name (FreeRTOS's limit). */
                static constexpr size_t TaskNameBytes = 16;

                /** @brief A process, as `ps` shows it. */
                struct ProcessInfo {
                                /** @brief What it is doing. */
                                enum class State : uint8_t {
                                        Running, ///< Its threads run.
                                        Vfork,   ///< A vfork child, running in its parent's task.
                                        Ending,  ///< Its program returned; its threads are ending.
                                        Zombie,  ///< Ended: its parent hasn't waited for it.
                                        Stopped, ///< Stopped by a signal, until continued.
                                };

                                int32_t pid;                   ///< Its id.
                                int32_t parent;                ///< Its parent's (0: none; -1: the kernel).
                                int32_t group;                 ///< Its process group.
                                int32_t session;               ///< Its session.
                                int32_t status;                ///< How it ended (`Zombie`).
                                State   state;                 ///< What it is doing.
                                uint8_t threads;               ///< Its tasks (its own and its `Thread`s).
                                size_t   memory;                ///< Bytes it has allocated.
                                char     command[CommandBytes]; ///< Its command line, cut.
                                uint64_t cpuUs;     ///< CPU time (`devplan/sysmon.md` §3.6; 0: not counted).
                                int64_t  startedUs; ///< When it started (monotonic).
                                uint64_t childrenCpuUs; ///< Its children's it waited for, and theirs.
                };

                /** @brief Whose a task is (`findTaskOwners()`). */
                struct TaskOwnership {
                                void   *task; ///< Its handle (set by the caller).
                                int32_t pid;  ///< Its process (0: the kernel's).
                                int32_t tid;  ///< A program thread's id (0: a kernel task or `Thread`).
                                bool    main; ///< The process's first thread.
                };

                /** @brief A task of a process, as `ps -T` shows it. */
                struct ThreadInfo {
                                int32_t  pid;                 ///< Its process.
                                int32_t  tid;                 ///< A program thread's id (0: a kernel `Thread`).
                                char     name[TaskNameBytes]; ///< The task's name.
                                char     state;               ///< R (ready or running), S (blocked), T (suspended).
                                bool     main;                ///< The process's own task (else a `Thread`).
                                uint32_t stackFree;           ///< Bytes of stack never used.
                };

                /** @brief Sets up the process table and the console signals' delivery (a boot stage). */
                static Error init();

                /**
                 * @brief Fills @p out with the processes, zombies included, in pid order.
                 * @return How many there are (more than `out.size()`: those past it are left out).
                 */
                static size_t listProcesses(Span<ProcessInfo> out);

                /**
                 * @brief Describes process @p pid, or with @p next the first after it
                 *        (pid order; 0: the first).
                 * @return `Ok`, or `NotExist` (none).
                 */
                static Error processInfo(int32_t pid, bool next, ProcessInfo &out);

                /** @brief Fills in whose each of @p tasks is (`TaskOwnership::task` set). */
                static void findTaskOwners(Span<TaskOwnership> tasks);

                /**
                 * @brief Fills @p out with the tasks of every running process.
                 * @return How many there are (more than `out.size()`: those past it are left out).
                 */
                static size_t listThreads(Span<ThreadInfo> out);

                /**
                 * @brief Sends signal @p sig to process @p pid (> 0), or to group
                 *        `-pid` (< -1), as `kill()` does (0 only checks).
                 * @return `Ok`, `NotExist` (no such process), `Invalid` (the signal
                 *         or pid), or `PermissionDenied` (`SIGKILL`, `SIGSTOP` to pid 1).
                 */
                static Error signal(int32_t pid, int sig);

                /** @copydoc ProgramLauncher::run */
                Result<int> run(const Request &request) override;

                /** @copydoc ProgramLauncher::start */
                Result<int32_t> start(const Request &request) override;

        private:
                Result<int32_t> begin(const Request &request, Semaphore &ended, Console::TermMode &savedMode,
                                      int32_t &parent);
};

BMDS_NAMESPACE_END
