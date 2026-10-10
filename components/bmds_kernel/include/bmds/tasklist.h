/**
 * @file      tasklist.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tasks BMDS started (`Task`, `Thread`: they register themselves)
 *        and the system's own, listed without masking interrupts
 *        (`devplan/sysmon.md` §3.6).
 *
 * FreeRTOS's `uxTaskGetSystemState()` holds the kernel's spinlock, with
 * interrupts masked, while it scans every task's stack (about 3 ms on the
 * board). Here each task's state comes from `vTaskGetInfo()` without the scan,
 * and the stack is scanned afterwards with interrupts on; the list's mutex
 * keeps a listed task from being deleted meanwhile.
 *
 * Target only. The lock is made by the first `add()`, which is the boot
 * sequence's first `Task` (before any other BMDS task runs).
 *
 * @par Thread Safety
 * Thread-safe; not from an ISR.
 */
namespace TaskList {

        /** @brief Bytes of a task's name with its NUL (FreeRTOS's limit). */
        constexpr size_t NameBytes = 16;

        /** @brief A task's place in the list, embedded in its owner. */
        struct Node {
                        void *task = nullptr; ///< Its handle.
                        Node *next = nullptr;
                        bool  live = false; ///< Created: listed.
        };

        /** @brief What a task is doing (FreeRTOS's `eTaskState`). */
        enum class State : uint8_t {
                Running = 0,
                Ready = 1,
                Blocked = 2,
                Suspended = 3,
                Deleted = 4,
        };

        /** @brief A task, as listed. */
        struct Info {
                        void    *task = nullptr;       ///< Its handle.
                        char     name[NameBytes] = {}; ///< Its name.
                        State    state = State::Ready; ///< What it is doing.
                        uint8_t  priority = 0;         ///< Its priority now.
                        uint8_t  basePriority = 0;     ///< Without inheritance.
                        int8_t   core = -1;            ///< Its core (-1: either).
                        uint32_t stackFree = 0;        ///< Bytes of stack never used.
                        uint32_t number = 0;           ///< FreeRTOS's task number.
                        uint64_t cpuUs = 0;            ///< Time it has run (0: not counted).
        };

        /** @brief Adds @p task (a static task's handle, known before it exists), not yet listed. */
        void add(Node &node, void *task);

        /** @brief @p node's task exists now: listed from here on. */
        void setLive(Node &node);

        /** @brief Removes @p node, before its task is deleted (waits for a listing in progress). */
        void remove(Node &node);

        /**
         * @brief Fills @p out with the tasks: BMDS's, then the system's.
         * @return How many there are (more than `out.size()`: those past it are left out).
         * @blocking While it scans the stacks (interrupts on).
         */
        size_t list(Span<Info> out);

        /**
         * @brief Returns the time @p task has run, in µs, its stretch running now
         *        included (0: not counted). @p task must exist.
         */
        uint64_t cpuUs(void *task);

        /**
         * @brief When each core last switched tasks, by the run-time clock (µs; 0:
         *        not counted): set by the clock's reader (`CpuTime`), which FreeRTOS
         *        calls at every switch. FreeRTOS adds a stretch to a task's counter
         *        only when it ends, so a running task's counter lacks the time since.
         */
        extern volatile int64_t switchedInUs[];

} // namespace TaskList

BMDS_NAMESPACE_END
