/**
 * @file      thread.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memspace.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A task that ends: started, runs its entry function, then joined.
 *
 * Unlike `Task` (kernel-lifetime), a Thread's entry function returns, and
 * `join()` waits for that and frees the stack and control block. Pipeline
 * workers are Threads.
 *
 * A Thread started by a task that belongs to a process (`TaskOwner`) is one
 * of that process's threads; started by a kernel task it is a kernel
 * thread (`devplan/processes.md` §3.1).
 *
 * The control block comes from `MemSpace::Internal` (the scheduler requires
 * it) and the stack from `Config::stackSpace`, PSRAM by default. A thread
 * with a PSRAM stack must not write to SPI flash (NVS, OTA): flash writes
 * disable the cache that PSRAM is reached through.
 *
 * The header is FreeRTOS-free; the host tests run Threads on `std::thread`.
 *
 * @par Thread Safety
 * Not thread-safe: one owner starts and joins a Thread.
 */
class Thread {
        public:
                /** @brief Entry function; the thread ends when it returns. */
                using Entry = void (*)(void *arg);

                /** @brief Thread parameters. */
                struct Config {
                                const char  *name = "thread";              ///< Name (copied).
                                size_t       stackBytes = 8192;            ///< Stack size in bytes.
                                unsigned int priority = 1;                 ///< FreeRTOS priority.
                                MemSpace     stackSpace = MemSpace::Psram; ///< Where the stack lives.
                                /** A kernel thread even if a process's task starts it (a subsystem's own, started on demand). */
                                bool kernel = false;
                };

                /** @brief Constructs a thread that is not started. */
                constexpr Thread() = default;

                /** @brief The thread must have been joined (or never started). */
                ~Thread();

                Thread(const Thread &) = delete;
                Thread &operator=(const Thread &) = delete;

                /**
                 * @brief Starts running @p entry(@p arg).
                 * @return `Ok`, `InvalidState` if already started (or the process is
                 *         ending), or `NoMem`.
                 */
                Error start(const Config &config, Entry entry, void *arg);

                /**
                 * @brief Waits for the entry function to return, then frees the thread.
                 *
                 * No effect if not started. Afterwards the Thread may be
                 * started again.
                 * @blocking
                 */
                void join();

                /**
                 * @brief Lets the thread end on its own: called by the thread itself,
                 *        its stack and record are freed once its entry returns (by
                 *        the system, not a `join()`), and this object is no longer
                 *        tied to it (it may be destroyed at once).
                 */
                void detach();

                /**
                 * @brief Runs @p entry(@p arg) on a new kernel thread with an internal-RAM stack of
                 *        @p stackBytes, at the caller's priority, and waits for it to return: for
                 *        what turns the caches off (a flash erase or write, `esp_ota_*`), which
                 *        asserts on a stack in PSRAM, as programs' and the shell's are.
                 * @return `Ok`, or `NoMem` (it didn't run).
                 * @blocking
                 */
                static Error runOnInternalStack(const char *name, size_t stackBytes, Entry entry, void *arg);

                /**
                 * @brief Keeps the calling thread (or task) on @p core from now on, moving it there
                 *        if it runs on the other (`devplan/processes.md` §3.12a).
                 *
                 * ESP-IDF pins a task to its core the first time it uses the FPU or PIE (their
                 * registers are saved lazily, so its state stays in that core's); before that it
                 * may run on either, and may be pinned anywhere. After: only its own core.
                 *
                 * @return `Ok`, `Invalid` (no such core), or `InvalidState` (pinned to the
                 *         other already).
                 * @blocking Moving: until the other core runs it (its next switch, a tick at most).
                 */
                static Error pinCurrent(int core);

                /**
                 * @brief Sets the calling thread's (or task's) FreeRTOS priority to @p priority (at
                 *        most `configMAX_PRIORITIES - 1`): for a thread that raises itself while it
                 *        falls behind and goes back after (`devplan/audio.md` §27). Target only.
                 * @nonblocking It may switch to another thread at once (lowered).
                 */
                static void setCurrentPriority(unsigned int priority);

                /**
                 * @brief Blocks the calling thread (or task) for @p delayMs.
                 * @blocking
                 */
                static void sleep(unsigned int delayMs);

                /** @brief Returns true between `start()` and `join()`. */
                bool isStarted() const { return _impl != nullptr; }

        private:
                void *_impl = nullptr; // platform record
};

BMDS_NAMESPACE_END
