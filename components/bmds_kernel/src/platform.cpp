/**
 * @file      platform.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <sys/time.h>
#include <esp_attr.h>
#include <esp_rtc_time.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/detail/platform.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The clock record, in RTC memory that a reset leaves alone (but
        // power-up fills with garbage): a magic number and a check word tell.
        struct RtcClockRecord {
                        uint32_t magic;
                        uint32_t check;
                        int64_t  wallMinusPersistentNs;
                        int32_t  frequencyPpb;
                        uint32_t reserved;
        };

        constexpr uint32_t ClockRecordMagic = 0x424d4343; // "BMCC"

        RTC_NOINIT_ATTR RtcClockRecord rtcClockRecord;

        uint32_t checkWord(const RtcClockRecord &r) {
                // FNV-1a over the payload.
                const auto *bytes = reinterpret_cast<const uint8_t *>(&r.wallMinusPersistentNs);
                uint32_t    h = 2166136261u;
                for (size_t i = 0; i < sizeof(r.wallMinusPersistentNs) + sizeof(r.frequencyPpb); ++i) {
                        h ^= bytes[i];
                        h *= 16777619u;
                }
                return h;
        }

} // namespace

int64_t Detail::monotonicUs() {
        return esp_timer_get_time();
}

void Detail::suspendScheduler() {
        vTaskSuspendAll();
        return;
}

void Detail::resumeScheduler() {
        (void)xTaskResumeAll();
        return;
}

namespace {

        // Index 0 is IDF's pthread's, 1 the loader's (a program's Running).
        constexpr BaseType_t InterruptTlsIndex = 2;
        static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > InterruptTlsIndex,
                      "Interrupt::current() needs thread-local pointer 2");

} // namespace

void *Detail::taskInterrupt() {
        return pvTaskGetThreadLocalStoragePointer(nullptr, InterruptTlsIndex);
}

void Detail::setTaskInterrupt(void *interrupt) {
        vTaskSetThreadLocalStoragePointer(nullptr, InterruptTlsIndex, interrupt);
        return;
}

int64_t Detail::persistentTimeUs() {
        return static_cast<int64_t>(esp_rtc_get_time_us());
}

bool Detail::loadClockRecord(ClockRecord &record) {
        const RtcClockRecord r = rtcClockRecord;
        if (r.magic != ClockRecordMagic || r.check != checkWord(r)) return false;
        record.wallMinusPersistentNs = r.wallMinusPersistentNs;
        record.frequencyPpb = r.frequencyPpb;
        return true;
}

void Detail::saveClockRecord(const ClockRecord &record) {
        RtcClockRecord r = {};
        r.magic = ClockRecordMagic;
        r.wallMinusPersistentNs = record.wallMinusPersistentNs;
        r.frequencyPpb = record.frequencyPpb;
        r.check = checkWord(r);
        rtcClockRecord = r;
        return;
}

void Detail::setLibcTime(int64_t wallNs) {
        const timeval tv = {.tv_sec = static_cast<time_t>(wallNs / 1'000'000'000),
                            .tv_usec = static_cast<suseconds_t>(wallNs % 1'000'000'000 / 1000)};
        (void)settimeofday(&tv, nullptr);
        return;
}

BMDS_NAMESPACE_END
