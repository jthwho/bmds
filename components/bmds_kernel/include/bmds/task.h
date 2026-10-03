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
                /** @brief Task entry point. Must not return. */
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

                /** @brief Returns true once started. */
                bool isRunning() const { return _handle != nullptr; }

                /** @brief Returns the FreeRTOS handle, or null. */
                TaskHandle_t handle() const { return _handle; }

                /**
                 * @brief Blocks the calling task for @p delayMs.
                 * @blocking
                 */
                static void sleep(unsigned int delayMs);

        private:
                TaskHandle_t _handle = nullptr;
};

BMDS_NAMESPACE_END
