/**
 * @file      boot.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdint>
#include <esp_log.h>
#include <esp_log_timestamp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/assert.h>
#include <bmds/boot.h>
#include <bmds/clock.h>
#include <bmds/random.h>
#include <bmds/error.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/notifications.h>
#include <bmds/stats.h>
#include <bmds/system.h>

BMDS_DEBUG(Boot)

BMDS_NAMESPACE_BEGIN

namespace {

        using Stage = Boot::Stage;

        // Released once the boot report is out. Each waiter takes it and puts
        // it straight back, so any number of tasks pass. (Not an event group:
        // those pull in the FreeRTOS timer task, about 2.8 KB of internal RAM.)
        constinit StaticObject<Semaphore> bootDone;

        // Most stages a boot can have (kernel + system); sizes the timing array.
        constexpr size_t MaxStages = 24;

        Error initMemory() {
                // Every MemSpace the board maps must have backing memory; a
                // missing PSRAM here means a configuration or hardware fault.
                Error result;
                for (size_t i = 0; i < MemSpaceCount; ++i) {
                        const MemSpace space = static_cast<MemSpace>(i);
                        const MemStats s = Memory::stats(space);
                        bmdsDebug("MemSpace %s: total %u free %u", memSpaceName(space),
                                  static_cast<unsigned>(s.totalBytes), static_cast<unsigned>(s.freeBytes));
                        if (s.totalBytes == 0) {
                                bmdsErr("Boot: MemSpace %s has no memory", memSpaceName(space));
                                result = Error::NoMem;
                        }
                }
                return result;
        }

        // The boot stages, in order. Lives in rodata.
        constexpr Stage Stages[] = {
                {"memory", initMemory},          {"random", Random::init}, // before anything reads it
                {"clock", Clock::init},          {"stats", Stats::init},   // before any component adds its counters
                {"notify", Notifications::init},                           // before a program can post one
        };
        constexpr size_t StageCount = sizeof(Stages) / sizeof(Stages[0]);

        void printBanner() {
                const SystemInfo info = System::info();
                bmdsInfo("BMDS %s (build %s, ESP-IDF %s)", info.bmdsVersion, info.buildVersion, info.idfVersion);
                bmdsInfo("Board: %s", info.boardName);
                bmdsInfo("Chip: ESP32-P4 rev v%u.%u, %u cores @ %u MHz", info.chipRevMajor, info.chipRevMinor,
                         info.cores, info.cpuMhz);
                return;
        }

        void printMemoryReport() {
                bmdsInfo("Memory:       %10s %10s %10s %10s", "total", "free", "min free", "largest");
                for (size_t i = 0; i < MemSpaceCount; ++i) {
                        const MemSpace space = static_cast<MemSpace>(i);
                        const MemStats s = Memory::stats(space);
                        bmdsInfo("  %-11s %10u %10u %10u %10u", memSpaceName(space),
                                 static_cast<unsigned>(s.totalBytes), static_cast<unsigned>(s.freeBytes),
                                 static_cast<unsigned>(s.minFreeBytes), static_cast<unsigned>(s.largestFreeBlock));
                }
                return;
        }

} // namespace

void Boot::run(Span<const Stage> systemStages) {
        const int64_t startUs = esp_timer_get_time();
        bootDone.create(1, 0);
        const Error logErr = Log::init();
        printBanner();
        if (logErr.isError()) bmdsWarn("Boot: log history unavailable: %s", logErr.desc());

        const size_t stageCount = StageCount + systemStages.size();
        BMDS_ASSERT(stageCount <= MaxStages);
        int64_t stageUs[MaxStages];
        size_t  failed = 0;
        for (size_t i = 0; i < stageCount; ++i) {
                const Stage  &stage = i < StageCount ? Stages[i] : systemStages[i - StageCount];
                const int64_t t0 = esp_timer_get_time();
                const Error   err = stage.init();
                stageUs[i] = esp_timer_get_time() - t0;
                if (err.isError()) {
                        bmdsErr("Boot: stage '%s' failed: %s", stage.name, err.desc());
                        ++failed;
                }
        }

        const int64_t endUs = esp_timer_get_time();
        for (size_t i = 0; i < stageCount; ++i) {
                const Stage &stage = i < StageCount ? Stages[i] : systemStages[i - StageCount];
                bmdsInfo("Stage %-10s %8lu us", stage.name, static_cast<unsigned long>(stageUs[i]));
        }
        bmdsInfo("BMDS init %lu us; %lu ms since reset; %u stage(s) failed",
                 static_cast<unsigned long>(endUs - startUs), static_cast<unsigned long>(esp_log_timestamp()),
                 static_cast<unsigned>(failed));
        printMemoryReport();
        bmdsInfo("Debug modules registered: %u", static_cast<unsigned>(Log::debugModules().size()));
        bmdsInfo("Main task stack high water: %u bytes free",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

        // IDF's "Returned from app_main()" would follow the shell prompt; the
        // boot report above already says boot is done.
        esp_log_level_set("main_task", ESP_LOG_WARN);
        bootDone->release();
        return;
}

Error Boot::waitUntilComplete(unsigned int timeoutMs) {
        if (!bootDone.isCreated()) return Error::InvalidState;
        const Error err = bootDone->acquire(timeoutMs);
        if (err.isOk()) bootDone->release();
        return err;
}

BMDS_NAMESPACE_END
