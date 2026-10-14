/**
 * @file      system.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <bmds/board.h>
#include <bmds/system.h>
#include <bmds/thread.h>
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

const char *System::serial() {
        // Read at the first call; the same bytes each time. Whether it was read is published
        // last, so a reader that sees it sees the rest.
        static char text[MaxSerialBytes + 1];
        static bool read;
        if (!__atomic_load_n(&read, __ATOMIC_ACQUIRE)) {
                char         out[MaxSerialBytes + 1] = {};
                const size_t n = board().serial(out, sizeof(out)); // the board's own reader

                out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';
                std::memcpy(text, out, sizeof(text));
                __atomic_store_n(&read, true, __ATOMIC_RELEASE);
        }
        return text;
}

uint64_t System::uptimeUs() {
        return static_cast<uint64_t>(esp_timer_get_time());
}

void System::restart() {
        esp_restart();
}

System::Firmware System::running() {
        const esp_partition_t *p = esp_ota_get_running_partition();
        return p != nullptr && p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ? Firmware::Recovery : Firmware::Main;
}

Error System::setNextFirmware(Firmware firmware) {
        struct Job {
                        const esp_partition_t *partition;
                        esp_err_t              result;
        } job = {esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                          firmware == Firmware::Recovery ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                                                         : ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                          nullptr),
                 ESP_FAIL};
        if (job.partition == nullptr) return Error::NotExist;
        // It reads the image with the caches off and writes otadata: not on a PSRAM stack.
        const Error err = Thread::runOnInternalStack(
                "otadata", 6144,
                [](void *arg) {
                        Job *j = static_cast<Job *>(arg);
                        j->result = esp_ota_set_boot_partition(j->partition);
                        return;
                },
                &job);
        if (err.isError()) return err;
        if (job.result == ESP_OK) return Error::Ok;
        return job.result == ESP_ERR_OTA_VALIDATE_FAILED ? Error::CorruptData : Error::IoError;
}

BMDS_NAMESPACE_END
