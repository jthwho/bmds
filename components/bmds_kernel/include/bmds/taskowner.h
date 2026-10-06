/**
 * @file      taskowner.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a task belongs to: a process, opaque to the kernel
 *        (`devplan/processes.md` §3.1).
 *
 * Each task has an owner pointer (null: a kernel task). The loader, which
 * owns processes, sets it on a process's tasks. A `Thread` started by a
 * task with an owner joins that owner: `attach()` before it starts,
 * `detach()` once its entry function has returned. A `Task` never does:
 * tasks are the kernel's.
 *
 * @par Thread Safety
 * `current()` and `setCurrent()` act on the calling task. Implementations
 * of `attach()` and `detach()` must be thread-safe: they are called from the
 * starting task and from the thread itself.
 */
class TaskOwner {
        public:
                /** @brief A thread of an owner: kept in the thread's own record (no allocation). */
                struct Member {
                                void   *task = nullptr; ///< Its FreeRTOS handle (null on the host).
                                Member *next = nullptr; ///< The owner's list.
                };

                /** @brief Returns the calling task's owner, or null (a kernel task). */
                static TaskOwner *current();

                /** @brief Sets the calling task's owner (null: none). */
                static void setCurrent(TaskOwner *owner);

                /**
                 * @brief A thread is about to start in this owner (called by the
                 *        starting task, before the thread runs).
                 * @return `Ok`, or an error that stops the start (`InvalidState`:
                 *         the owner is ending).
                 */
                virtual Error attach(Member &member) = 0;

                /** @brief The thread's entry function has returned (called by the thread). */
                virtual void detach(Member &member) = 0;

        protected:
                TaskOwner() = default;
                ~TaskOwner() = default;
                TaskOwner(const TaskOwner &) = delete;
                TaskOwner &operator=(const TaskOwner &) = delete;
};

BMDS_NAMESPACE_END
