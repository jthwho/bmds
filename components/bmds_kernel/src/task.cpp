/**
 * @file      task.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/ticks.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/task.h>

BMDS_DEBUG(Task)

BMDS_NAMESPACE_BEGIN

Error Task::start(const Config &config, Entry entry, void *arg) {
        if (_handle != nullptr) return Error::InvalidState;
        auto *tcb = static_cast<StaticTask_t *>(Memory::alloc(MemSpace::Internal, sizeof(StaticTask_t)));
        auto *stack = static_cast<StackType_t *>(Memory::alloc(config.stackSpace, config.stackBytes));
        if (tcb == nullptr || stack == nullptr) {
                Memory::free(tcb);
                Memory::free(stack);
                bmdsErr("Task(%p '%s'): no memory for %u-byte stack in %s", this, config.name,
                        static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace));
                return Error::NoMem;
        }
        const BaseType_t core = config.core == AnyCore ? tskNO_AFFINITY : static_cast<BaseType_t>(config.core);
        // ESP-IDF stack depth is in bytes (StackType_t is uint8_t).
        _tcb = tcb;
        _stack = stack;
        _handle = xTaskCreateStaticPinnedToCore(entry, config.name, config.stackBytes / sizeof(StackType_t), arg,
                                                config.priority, stack, tcb, core);
        bmdsDebug("Task(%p '%s'): started, %u-byte stack in %s, priority %u", this, config.name,
                  static_cast<unsigned>(config.stackBytes), memSpaceName(config.stackSpace), config.priority);
        return Error();
}

Error Task::stop() {
        if (_handle == nullptr || _handle == xTaskGetCurrentTaskHandle()) return Error::InvalidState;
        // As IDF's vTaskDeleteWithCaps(): suspended and off the CPU first, then
        // deleted (the port's delete hook may still touch the stack: FPU and
        // PIE state), and only then are its buffers freed.
        vTaskSuspend(_handle);
        while (eTaskGetState(_handle) == eRunning) taskYIELD();
        vTaskDelete(_handle);
        Memory::free(_stack);
        Memory::free(_tcb);
        _handle = nullptr;
        _stack = nullptr;
        _tcb = nullptr;
        return Error::Ok;
}

void Task::sleep(unsigned int delayMs) {
        vTaskDelay(Detail::toTicks(delayMs));
        return;
}

BMDS_NAMESPACE_END
