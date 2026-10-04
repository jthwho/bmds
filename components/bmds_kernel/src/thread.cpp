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
#include <bmds/assert.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/thread.h>

BMDS_DEBUG(Thread)

BMDS_NAMESPACE_BEGIN

namespace {

        // Everything a running thread needs, in internal RAM with its TCB.
        struct Record {
                        StaticTask_t  tcb;
                        TaskHandle_t  handle = nullptr;
                        uint8_t      *stack = nullptr;
                        Thread::Entry entry = nullptr;
                        void         *arg = nullptr;
                        Semaphore     done{1, 0};
        };

        void trampoline(void *p) {
                Record *rec = static_cast<Record *>(p);
                rec->entry(rec->arg);
                rec->done.release();
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
        auto *stack = static_cast<uint8_t *>(Memory::alloc(config.stackSpace, config.stackBytes));
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
        _impl = rec;
        // ESP-IDF stack depth is in bytes (StackType_t is uint8_t).
        rec->handle = xTaskCreateStaticPinnedToCore(trampoline, config.name, config.stackBytes / sizeof(StackType_t),
                                                    rec, config.priority, stack, &rec->tcb, tskNO_AFFINITY);
        bmdsDebug("Thread(%p '%s'): started, %u-byte stack in %s", this, config.name,
                  static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace));
        return Error::Ok;
}

void Thread::join() {
        Record *rec = static_cast<Record *>(_impl);
        if (rec == nullptr) return;
        (void)rec->done.acquire(WaitForever);
        // Usually already parked; on the other core it may take a moment.
        while (eTaskGetState(rec->handle) != eSuspended) vTaskDelay(1);
        vTaskDelete(rec->handle);
        Memory::free(rec->stack);
        rec->~Record();
        Memory::free(rec);
        _impl = nullptr;
        return;
}

BMDS_NAMESPACE_END
