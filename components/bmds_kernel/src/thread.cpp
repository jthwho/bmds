/**
 * @file      thread.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <atomic>
#include <bmds/assert.h>
#include <bmds/detail/ticks.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/tasklist.h>
#include <bmds/taskowner.h>
#include <bmds/thread.h>

BMDS_DEBUG(Thread)

BMDS_NAMESPACE_BEGIN

namespace {

        // Everything a running thread needs, in internal RAM with its TCB.
        struct Record {
                        StaticTask_t      tcb;
                        TaskHandle_t      handle = nullptr;
                        uint8_t          *stack = nullptr;
                        Thread::Entry     entry = nullptr;
                        void             *arg = nullptr;
                        TaskOwner        *owner = nullptr; // its process (the starter's), or null
                        TaskOwner::Member member;
                        TaskList::Node    listed; // in the task list while it exists
                        Semaphore         done{1, 0};
                        std::atomic<bool> detached{false}; // freed by the timer task when it ends
        };

        // A detached thread that ended, freed on the timer service's task (a task can't free its
        // own stack): once it is parked, it is deleted and its memory freed.
        void reap(void *p, uint32_t) {
                Record *rec = static_cast<Record *>(p);
                while (eTaskGetState(rec->handle) != eSuspended) vTaskDelay(1); // parking, on the other core
                TaskList::remove(rec->listed);
                vTaskDelete(rec->handle);
                Memory::free(rec->stack);
                rec->~Record();
                Memory::free(rec);
                return;
        }

        void trampoline(void *p) {
                Record *rec = static_cast<Record *>(p);
                TaskOwner::setCurrent(rec->owner);
                rec->entry(rec->arg);
                if (rec->owner != nullptr) rec->owner->detach(rec->member);
                if (rec->detached.load(std::memory_order_acquire)) {
                        // Nobody will join it: the timer service frees it once it is parked below.
                        while (xTimerPendFunctionCall(reap, rec, 0, pdMS_TO_TICKS(100)) != pdPASS) {}
                } else {
                        rec->done.release();
                }
                // Park: join() deletes the task once it sees it suspended, so
                // the task is never deleted while it is still running.
                vTaskSuspend(nullptr);
        }

} // namespace

Thread::~Thread() {
        BMDS_ASSERT(_impl == nullptr);
}

Error Thread::start(const Config &config, Entry entry, void *arg) {
        if (_impl != nullptr) return Error::InvalidState;
        void *mem = Memory::alloc(MemSpace::Internal, sizeof(Record));
        // A stack stays on the heap: IDF's cache-disabling paths judge a stack by its address.
        Memory::Options stackOptions;
        stackOptions.heapOnly = true;
        auto *stack = static_cast<uint8_t *>(Memory::alloc(config.stackSpace, config.stackBytes, stackOptions));
        if (mem == nullptr || stack == nullptr) {
                Memory::free(mem);
                Memory::free(stack);
                bmdsErr("Thread(%p '%s'): no memory for %u-byte stack in %s", this, config.name,
                        static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace));
                return Error::NoMem;
        }
        Record *rec = new (mem) Record();
        rec->stack = stack;
        rec->entry = entry;
        rec->arg = arg;
        // In the starter's process, if it has one (devplan/processes.md §3.1). A static task's
        // handle is its TCB, known before it exists.
        TaskOwner *const starter = TaskOwner::current();
        rec->owner = starter != nullptr && !config.kernel ? starter->threadOwner() : nullptr;
        rec->member.task = &rec->tcb;
        if (rec->owner != nullptr) {
                const Error err = rec->owner->attach(rec->member);
                if (err.isError()) {
                        rec->~Record();
                        Memory::free(mem);
                        Memory::free(stack);
                        return err;
                }
        }
        _impl = rec;
        TaskList::add(rec->listed, &rec->tcb);
        // ESP-IDF stack depth is in bytes (StackType_t is uint8_t).
        rec->handle = xTaskCreateStaticPinnedToCore(trampoline, config.name, config.stackBytes / sizeof(StackType_t),
                                                    rec, config.priority, stack, &rec->tcb, tskNO_AFFINITY);
        TaskList::setLive(rec->listed);
        bmdsDebug("Thread(%p '%s'): started, %u-byte stack in %s", this, config.name,
                  static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace));
        return Error::Ok;
}

// The port's own (port.c), as its FPU and PIE traps pin a task: not in a header.
extern "C" void vPortTaskPinToCore(StaticTask_t *task, int coreid);

Error Thread::pinCurrent(int core) {
        if (core < 0 || core >= portNUM_PROCESSORS) return Error::Invalid;
        TaskHandle_t     self = xTaskGetCurrentTaskHandle();
        const BaseType_t pinned = xTaskGetCoreID(self);
        if (pinned != tskNO_AFFINITY) return pinned == core ? Error::Ok : Error::InvalidState;
        // Not pinned: it has never used the FPU or PIE, so none of its state is in a core's
        // registers, and nothing pins it meanwhile (only its own FPU or PIE use would).
        vPortTaskPinToCore(reinterpret_cast<StaticTask_t *>(self), core);
        // On the other core: this one's scheduler won't run it again; the other takes it at
        // its next switch.
        while (xPortGetCoreID() != core) taskYIELD();
        return Error::Ok;
}

void Thread::sleep(unsigned int delayMs) {
        vTaskDelay(Detail::toTicks(delayMs));
        return;
}

void Thread::detach() {
        Record *rec = static_cast<Record *>(_impl);
        if (rec == nullptr) return;
        BMDS_ASSERT(rec->handle == xTaskGetCurrentTaskHandle()); // only the thread itself
        rec->detached.store(true, std::memory_order_release);
        _impl = nullptr;
        return;
}

Error Thread::runOnInternalStack(const char *name, size_t stackBytes, Entry entry, void *arg) {
        Thread thread;
        Config config;
        config.name = name;
        config.stackBytes = stackBytes;
        config.stackSpace = MemSpace::Internal;
        config.priority = uxTaskPriorityGet(nullptr);
        config.kernel = true;
        const Error err = thread.start(config, entry, arg);
        if (err.isError()) return err;
        thread.join();
        return Error::Ok;
}

void Thread::join() {
        Record *rec = static_cast<Record *>(_impl);
        if (rec == nullptr) return;
        (void)rec->done.acquire(WaitForever);
        // Usually already parked; on the other core it may take a moment.
        while (eTaskGetState(rec->handle) != eSuspended) vTaskDelay(1);
        TaskList::remove(rec->listed);
        vTaskDelete(rec->handle);
        Memory::free(rec->stack);
        rec->~Record();
        Memory::free(rec);
        _impl = nullptr;
        return;
}

BMDS_NAMESPACE_END
