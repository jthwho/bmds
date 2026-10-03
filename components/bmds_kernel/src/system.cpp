/**
 * @file      system.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <bmds/board.h>
#include <bmds/system.h>
#include <bmds/version.h>

BMDS_NAMESPACE_BEGIN

SystemInfo System::info() {
        const esp_app_desc_t *app = esp_app_get_description();
        esp_chip_info_t       chip;
        esp_chip_info(&chip);
        SystemInfo info;
        info.bmdsVersion = VersionString;
        info.buildVersion = app->version;
        info.idfVersion = app->idf_ver;
        info.boardName = board().name;
        info.chipRevMajor = chip.revision / 100;
        info.chipRevMinor = chip.revision % 100;
        info.cores = chip.cores;
        info.cpuMhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        return info;
}

uint64_t System::uptimeUs() {
        return static_cast<uint64_t>(esp_timer_get_time());
}

void System::restart() {
        esp_restart();
}

BMDS_NAMESPACE_END
