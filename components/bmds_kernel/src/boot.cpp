/**
 * @file      boot.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdint>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_log_timestamp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <bmds/board.h>
#include <bmds/boot.h>
#include <bmds/error.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/version.h>

BMDS_DEBUG(Boot)

BMDS_NAMESPACE_BEGIN

namespace {

        struct Stage {
                        const char *name;
                        Error (*init)();
        };

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
                {"memory", initMemory},
        };
        constexpr size_t StageCount = sizeof(Stages) / sizeof(Stages[0]);

        void printBanner() {
                const esp_app_desc_t *app = esp_app_get_description();
                esp_chip_info_t       chip;
                esp_chip_info(&chip);
                bmdsInfo("BMDS %s (build %s, ESP-IDF %s)", VersionString, app->version, app->idf_ver);
                bmdsInfo("Board: %s", board().name);
                bmdsInfo("Chip: ESP32-P4 rev v%u.%u, %u cores @ %u MHz", static_cast<unsigned>(chip.revision / 100),
                         static_cast<unsigned>(chip.revision % 100), static_cast<unsigned>(chip.cores),
                         static_cast<unsigned>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
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

void Boot::run() {
        const int64_t startUs = esp_timer_get_time();
        printBanner();

        int64_t stageUs[StageCount];
        size_t  failed = 0;
        for (size_t i = 0; i < StageCount; ++i) {
                const int64_t t0 = esp_timer_get_time();
                const Error   err = Stages[i].init();
                stageUs[i] = esp_timer_get_time() - t0;
                if (err.isError()) {
                        bmdsErr("Boot: stage '%s' failed: %s", Stages[i].name, err.desc());
                        ++failed;
                }
        }

        const int64_t endUs = esp_timer_get_time();
        for (size_t i = 0; i < StageCount; ++i) {
                bmdsInfo("Stage %-10s %8lu us", Stages[i].name, static_cast<unsigned long>(stageUs[i]));
        }
        bmdsInfo("BMDS init %lu us; %lu ms since reset; %u stage(s) failed",
                 static_cast<unsigned long>(endUs - startUs), static_cast<unsigned long>(esp_log_timestamp()),
                 static_cast<unsigned>(failed));
        printMemoryReport();
        bmdsInfo("Debug modules registered: %u", static_cast<unsigned>(Log::debugModules().size()));
        bmdsInfo("Main task stack high water: %u bytes free",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        return;
}

BMDS_NAMESPACE_END
