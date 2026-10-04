/**
 * @file      running.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program while it runs (target only): what its system calls reach
 * through the task's thread-local pointer, and how it says it's done.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <bmds/config.h>
#include <bmds/process.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/** @brief The FreeRTOS thread-local pointer of a program's task that holds its `Running`. */
inline constexpr BaseType_t RunningTlsIndex = 1;
static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2,
              "programs need thread-local pointer 1 (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2)");

/**
 * @brief A running program: its Process, and how it ends.
 *
 * Killing (a second Ctrl-C) must never stop the task inside a system call,
 * where it may hold a kernel lock: every slot is bracketed by `enter()` and
 * `leave()` (a `KernelCall`). The loader sets `kill`, suspends the task and
 * looks at `inKernel`: clear, the task is in program code and is deleted;
 * set, it is resumed and ends itself in `leave()`. The two flags are a
 * Dekker handshake (sequentially consistent): if the loader reads
 * `inKernel` clear, the task can only be past its last kernel work.
 */
struct Running {
                /** @brief The status of a killed program (128 + SIGINT, as Ctrl-C in bash). */
                static constexpr int32_t KilledStatus = 130;

                Process          *process = nullptr;
                uintptr_t         entry = 0;  // the program's _start
                int32_t           status = 0; // set when it ends
                Semaphore         done{1, 0}; // released when it ends
                std::atomic<bool> inKernel{false};
                std::atomic<bool> kill{false};

                /** @brief The running program of the calling task. */
                static Running &current() {
                        return *static_cast<Running *>(pvTaskGetThreadLocalStoragePointer(nullptr, RunningTlsIndex));
                }

                /**
                 * @brief Ends the calling program with @p status: signals the loader
                 *        and suspends; the loader then deletes the task.
                 */
                [[noreturn]] void finish(int32_t exitStatus) {
                        status = exitStatus;
                        done.release();
                        for (;;) vTaskSuspend(nullptr);
                }

                /** @brief A system call starts. */
                void enter() {
                        inKernel.store(true);
                        return;
                }

                /** @brief A system call ends: back to program code, unless killed meanwhile. */
                void leave() {
                        inKernel.store(false);
                        if (kill.load()) finish(KilledStatus);
                        return;
                }
};

/**
 * @brief Brackets one system call (see `Running`); gives the Process.
 * @par Thread Safety
 * The program's own task only.
 */
class KernelCall {
        public:
                KernelCall() : _run(Running::current()) { _run.enter(); }
                ~KernelCall() { _run.leave(); }
                KernelCall(const KernelCall &) = delete;
                KernelCall &operator=(const KernelCall &) = delete;

                /** @brief The calling program's Process. */
                Process *operator->() const { return _run.process; }

        private:
                Running &_run;
};

BMDS_NAMESPACE_END
