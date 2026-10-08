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
