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
 * vfork and exec (devplan/programs.md §13). A program the shell starts is
 * a job: a second Ctrl-C kills it and every process it started.
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
                                };

                                int32_t pid;                   ///< Its id.
                                int32_t parent;                ///< Its parent's (1: the shell; 0: none).
                                int32_t job;                   ///< Its job.
                                int32_t status;                ///< How it ended (`Zombie`).
                                State   state;                 ///< What it is doing.
                                uint8_t threads;               ///< Its tasks (its own and its `Thread`s).
                                size_t  memory;                ///< Bytes it has allocated.
                                char    command[CommandBytes]; ///< Its command line, cut.
                };

                /** @brief A task of a process, as `ps -T` shows it. */
                struct ThreadInfo {
                                int32_t  pid;                 ///< Its process.
                                char     name[TaskNameBytes]; ///< The task's name.
                                char     state;               ///< R (ready or running), S (blocked), T (suspended).
                                bool     main;                ///< The process's own task (else a `Thread`).
                                uint32_t stackFree;           ///< Bytes of stack never used.
                };

                /** @brief Sets up the process table (a boot stage). */
                static Error init();

                /**
                 * @brief Fills @p out with the processes, zombies included, in pid order.
                 * @return How many there are (more than `out.size()`: those past it are left out).
                 */
                static size_t listProcesses(Span<ProcessInfo> out);

                /**
                 * @brief Fills @p out with the tasks of every running process.
                 * @return How many there are (more than `out.size()`: those past it are left out).
                 */
                static size_t listThreads(Span<ThreadInfo> out);

                /**
                 * @brief Sends signal @p sig to process @p pid (> 1), or to job
                 *        `-pid` (< -1), as `kill()` does (0 only checks).
                 * @return `Ok`, `NotExist` (no such process), `Invalid` (the signal
                 *         or pid), or `PermissionDenied` (pid 1, the shell).
                 */
                static Error signal(int32_t pid, int sig);

                /** @copydoc ProgramLauncher::run */
                Result<int> run(const Request &request) override;

                /** @copydoc ProgramLauncher::start */
                Result<int32_t> start(const Request &request) override;

        private:
                Result<int32_t> begin(const Request &request, Semaphore &ended, Console::TermMode &savedMode,
                                      int32_t &parent, int32_t &parentJob, bool &fromProcess);
};

BMDS_NAMESPACE_END
