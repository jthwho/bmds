/**
 * @file      tasklist.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tasks, listed without masking interrupts (devplan/sysmon.md §3.6).
 */

#include <cstdio>
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <bmds/mutex.h>
#include <bmds/staticobject.h>
#include <bmds/tasklist.h>

BMDS_NAMESPACE_BEGIN

namespace TaskList {

        // Written at every context switch (from IRAM, maybe with the caches off).
        DRAM_ATTR constinit volatile int64_t switchedInUs[portNUM_PROCESSORS] = {};

        namespace {

                // The stretch @p task is running now, if it is (by the run-time clock: 0 if not
                // counted). It may switch out meanwhile: off by at most that one stretch.
                uint64_t runningUs(TaskHandle_t task) {
                        for (BaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
                                if (xTaskGetCurrentTaskHandleForCore(core) != task) continue;
                                const int64_t since = switchedInUs[core];
                                const int64_t now = since != 0 ? esp_timer_get_time() : 0;
                                return since != 0 && now > since ? uint64_t(now - since) : 0;
                        }
                        return 0;
                }

                // The system's tasks by name, found once (they last until reboot); those absent
                // in this configuration stay null.
                constexpr const char *SystemNames[] = {"ipc0", "ipc1", "esp_timer"};
                constexpr size_t      SystemNameCount = sizeof(SystemNames) / sizeof(SystemNames[0]);

                struct List {
                                Mutex        lock;
                                Node        *head = nullptr;
                                TaskHandle_t named[SystemNameCount] = {};
                                bool         namedFound = false;
                };

                // Made by the first add(): the boot sequence's first Task, before any other runs.
                constinit StaticObject<List> list_;

                List &theList() {
                        if (!list_.isCreated()) list_.create();
                        return *list_;
                }

                // Its state without the stack scan (a short critical section), then the scan.
                void describe(TaskHandle_t task, Info &out) {
                        TaskStatus_t s;
                        vTaskGetInfo(task, &s, pdFALSE, eInvalid);
                        out = Info{};
                        out.task = task;
                        std::snprintf(out.name, sizeof(out.name), "%s", s.pcTaskName != nullptr ? s.pcTaskName : "");
                        out.state = State(s.eCurrentState <= eDeleted ? s.eCurrentState : eDeleted);
                        out.priority = uint8_t(s.uxCurrentPriority);
                        out.basePriority = uint8_t(s.uxBasePriority);
                        const BaseType_t core = xTaskGetCoreID(task);
                        out.core = core == tskNO_AFFINITY ? int8_t(-1) : int8_t(core);
                        out.number = uint32_t(s.xTaskNumber);
#if configGENERATE_RUN_TIME_STATS
                        out.cpuUs = uint64_t(s.ulRunTimeCounter) + runningUs(task);
#endif
                        out.stackFree = uint32_t(uxTaskGetStackHighWaterMark(task)); // bytes on ESP-IDF
                        return;
                }

        } // namespace

        void add(Node &node, void *task) {
                List         &l = theList();
                Mutex::Locker locker(l.lock, WaitForever);
                node.task = task;
                node.live = false;
                node.next = l.head;
                l.head = &node;
                return;
        }

        void setLive(Node &node) {
                List         &l = theList();
                Mutex::Locker locker(l.lock, WaitForever);
                for (Node *n = l.head; n != nullptr; n = n->next) {
                        if (n != &node) continue;
                        node.live = true; // only while still listed: a stop may have come first
                        break;
                }
                return;
        }

        void remove(Node &node) {
                List         &l = theList();
                Mutex::Locker locker(l.lock, WaitForever);
                for (Node **at = &l.head; *at != nullptr; at = &(*at)->next) {
                        if (*at != &node) continue;
                        *at = node.next;
                        break;
                }
                node.next = nullptr;
                node.live = false;
                return;
        }

        size_t list(Span<Info> out) {
                List         &l = theList();
                Mutex::Locker locker(l.lock, WaitForever);
                size_t        n = 0;
                auto          put = [&](TaskHandle_t task) {
                        if (task == nullptr) return;
                        if (n < out.size()) describe(task, out[n]);
                        ++n;
                };
                for (Node *node = l.head; node != nullptr; node = node->next)
                        if (node->live) put(static_cast<TaskHandle_t>(node->task));
                if (!l.namedFound) {
                        // xTaskGetHandle() walks every list in a critical section, but no stacks.
                        for (size_t i = 0; i < SystemNameCount; ++i) l.named[i] = xTaskGetHandle(SystemNames[i]);
                        l.namedFound = true;
                }
                for (BaseType_t core = 0; core < portNUM_PROCESSORS; ++core) put(xTaskGetIdleTaskHandleForCore(core));
                put(xTimerGetTimerDaemonTaskHandle());
                for (TaskHandle_t task : l.named) put(task);
                return n;
        }

        uint64_t cpuUs(void *task) {
#if configGENERATE_RUN_TIME_STATS
                TaskStatus_t s;
                vTaskGetInfo(static_cast<TaskHandle_t>(task), &s, pdFALSE, eInvalid);
                return uint64_t(s.ulRunTimeCounter) + runningUs(static_cast<TaskHandle_t>(task));
#else
                (void)task;
                return 0;
#endif
        }

} // namespace TaskList

BMDS_NAMESPACE_END
