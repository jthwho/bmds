/**
 * @file      realtimethreads.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Program threads' scheduling (target only; devplan/processes.md §3.12): their
 * policies, the kernel priorities those run at, and the real-time threads'
 * budget (`RealTimeBudget`), checked by a timer while any thread is real-time.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/sys.h>

BMDS_NAMESPACE_BEGIN

struct ProgramThread;

/**
 * @brief The real-time program threads: at most `MaxThreads` at a time.
 * @par Thread Safety
 * Thread-safe (one lock, taken after the process table's and a process's thread lock).
 */
namespace RealTimeThreads {

        /** @brief Real-time threads at a time; more are refused (`-BMDS_EAGAIN`). */
        constexpr size_t MaxThreads = 16;

        /** @brief Creates the lock (the loader's start). */
        Error init();

        /**
         * @brief Gives a thread about to start (not yet running) its scheduling: its
         *        creator's @p parent (null: a kernel task's), unless that resets on fork, and
         *        @p normalPriority; with @p exec (the program replaced in place) all of it.
         * @return The kernel priority to start its task at.
         */
        unsigned inherit(ProgramThread &child, const ProgramThread *parent, unsigned normalPriority, bool exec);

        /** @brief A thread starts, on its own task, before program code: a real-time one is counted. */
        void started(ProgramThread &thread);

        /** @brief A thread has ended: before its task is stopped. */
        void forget(ProgramThread &thread);

        /**
         * @brief Sets @p thread's policy (`BMDS_SCHED_...`, with `BMDS_SCHED_RESET_ON_FORK`)
         *        and priority.
         * @return 0, `-BMDS_EINVAL` or `-BMDS_EAGAIN` (`MaxThreads` real-time already).
         */
        int32_t set(ProgramThread &thread, uint32_t policy, uint32_t priority);

        /** @brief Fills @p out with @p thread's scheduling; @p self: it is the caller. */
        void get(ProgramThread &thread, bool self, bmds_sched_t &out);

} // namespace RealTimeThreads

BMDS_NAMESPACE_END
