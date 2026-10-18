/**
 * @file      task.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memspace.h>
#include <bmds/tasklist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A kernel-lifetime FreeRTOS task.
 *
 * The task control block is allocated from `MemSpace::Internal` (the
 * scheduler requires it) and the stack from the space given in `Config`.
 * Both are allocated once by `start()`. Tasks started this way run until
 * reboot; stopping and deleting tasks will arrive with the program model.
 *
 * @par Thread Safety
 * Not thread-safe: start a `Task` from one task, once.
 */
class Task {
        public:
                /** @brief Task entry point. Must not return (it may suspend itself for `stop()`). */
                using Entry = void (*)(void *arg);

                /** @brief Any core (no affinity). */
                static constexpr int AnyCore = -1;

                /** @brief Task parameters. */
                struct Config {
                                const char  *name = "task";                   ///< Name (copied by FreeRTOS).
                                size_t       stackBytes = 4096;               ///< Stack size in bytes.
                                unsigned int priority = 1;                    ///< FreeRTOS priority.
                                int          core = AnyCore;                  ///< Core affinity, or `AnyCore`.
                                MemSpace     stackSpace = MemSpace::Internal; ///< Where the stack lives.
                };

                constexpr Task() = default;
                Task(const Task &) = delete;
                Task &operator=(const Task &) = delete;

                /**
                 * @brief Allocates the TCB and stack and starts the task.
                 * @param config Task parameters.
                 * @param entry  Entry point.
                 * @param arg    Passed to @p entry.
                 * @return `Error::Ok`, `Error::NoMem`, or `Error::InvalidState`
                 *         if already started.
                 */
                Error start(const Config &config, Entry entry, void *arg);

                /**
                 * @brief Deletes the task and frees its TCB and stack.
                 *
                 * Call from another task. The task is suspended first and deleted
                 * once it is no longer running on either core, so it may be
                 * anywhere (typically suspended in its own code, having signalled
                 * that it's done). It must not hold a lock anyone else needs.
                 *
                 * @return `Ok`, or `InvalidState` if not running or if called by the
                 *         task itself.
                 * @blocking Briefly, while the task leaves the CPU.
                 */
                Error stop();

                /**
                 * @brief Suspends the task and waits until it is off both cores.
                 *
                 * Once this returns, the task's state (its memory, flags it
                 * publishes) holds still until `resume()`.
                 *
                 * @return `Ok`, or `InvalidState` if not running or if called by the
                 *         task itself.
                 * @blocking Briefly, while the task leaves the CPU.
                 */
                Error suspend();

                /**
                 * @brief Resumes a task suspended by `suspend()`.
                 * @return `Ok`, or `InvalidState` if not running.
                 * @nonblocking
                 */
                Error resume();

                /** @brief Returns true once started. */
                bool isRunning() const { return _handle != nullptr; }

                /** @brief Returns the FreeRTOS handle, or null. */
                TaskHandle_t handle() const { return _handle; }

                /**
                 * @brief Blocks the calling task for @p delayMs.
                 * @blocking
                 */
                static void sleep(unsigned int delayMs);

                /** @brief Returns the core the calling task runs on now (it may move unless pinned). @nonblocking */
                static int currentCore();

        private:
                TaskHandle_t   _handle = nullptr;
                StaticTask_t  *_tcb = nullptr;
                StackType_t   *_stack = nullptr;
                TaskList::Node _listed; // in the task list (`TaskList`) while it runs
};

BMDS_NAMESPACE_END
