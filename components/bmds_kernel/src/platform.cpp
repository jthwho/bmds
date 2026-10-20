/**
 * @file      platform.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <sys/time.h>
#include <cstring>
#include <bootloader_common.h>
#include <bootloader_random.h>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_random.h>
#include <esp_rtc_time.h>
#include <esp_private/systimer.h>
#include <esp_timer.h>
#include <hal/systimer_ll.h>
#include <soc/wdev_reg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/detail/platform.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The clock record, in RTC memory that a reset leaves alone (but power-up clears or fills
        // with garbage): a magic number and a check word tell. It lives in the bootloader's custom
        // area (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC), at the address both firmwares agree on: in
        // RTC_NOINIT memory each image's linker put it elsewhere, so recovery never found the main
        // firmware's (devplan/update.md §10).
        struct RtcClockRecord {
                        uint32_t magic;
                        uint32_t check;
                        int64_t  wallMinusPersistentNs;
                        int32_t  frequencyPpb;
                        uint32_t reserved;
        };

        constexpr uint32_t ClockRecordMagic = 0x424d4343; // "BMCC"

        static_assert(sizeof(RtcClockRecord) <= CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE,
                      "the clock record fits the bootloader's custom RTC area");

        // The area's bytes: at offset 12 of rtc_retain_mem_t, so copied, not cast (the record
        // holds a 64-bit field).
        uint8_t *rtcArea() {
                return bootloader_common_get_rtc_retain_mem()->custom;
        }

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

IRAM_ATTR int64_t Detail::monotonicUs() { // IRAM: esp_timer_get_time() is this (below), from IRAM interrupts too
        // esp_timer_get_time()'s counter, read as its HAL reads it (systimer_hal_get_counter_value),
        // and its 16 MHz ticks made µs by a shift: IDF's own conversion is the ROM's
        // systimer_ticks_to_us(), a software 64-bit division, a third of every clock read
        // (devplan/bench.md §15).
        systimer_dev_t *const dev = &SYSTIMER;
        systimer_ll_counter_snapshot(dev, SYSTIMER_COUNTER_ESPTIMER);
        while (!systimer_ll_is_counter_value_valid(dev, SYSTIMER_COUNTER_ESPTIMER)) {}
        uint32_t lo = systimer_ll_get_counter_value_low(dev, SYSTIMER_COUNTER_ESPTIMER), hi;
        for (;;) { // LO, HI, LO again: an interrupt between them can't tear the value
                hi = systimer_ll_get_counter_value_high(dev, SYSTIMER_COUNTER_ESPTIMER);
                const uint32_t again = systimer_ll_get_counter_value_low(dev, SYSTIMER_COUNTER_ESPTIMER);
                if (again == lo) break;
                lo = again;
        }
        return static_cast<int64_t>((uint64_t(hi) << 32 | lo) >> 4); // 16 ticks a µs (esp32p4/systimer.c)
}

// esp_timer_get_time(), everywhere in the firmware (-Wl,--wrap, CMakeLists.txt): IDF's reads the same
// counter but converts it in the ROM by a software division. FreeRTOS's run-time statistics read
// it at every context switch (devplan/bench.md §15).
extern "C" IRAM_ATTR int64_t __wrap_esp_timer_get_time(void) {
        return Detail::monotonicUs();
}

uint32_t Detail::cycles() {
        return static_cast<uint32_t>(esp_cpu_get_cycle_count());
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

        // Index 0 is IDF's pthread's, 1 the task's owner (its process: the loader's Running).
        constexpr BaseType_t OwnerTlsIndex = 1;
        constexpr BaseType_t InterruptTlsIndex = 2;
        static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > InterruptTlsIndex,
                      "Interrupt::current() needs thread-local pointer 2");

} // namespace

void *Detail::taskOwner() {
        return pvTaskGetThreadLocalStoragePointer(nullptr, OwnerTlsIndex);
}

void *Detail::taskOwnerOf(void *task) {
        return pvTaskGetThreadLocalStoragePointer(static_cast<TaskHandle_t>(task), OwnerTlsIndex);
}

void Detail::setTaskOwner(void *owner) {
        vTaskSetThreadLocalStoragePointer(nullptr, OwnerTlsIndex, owner);
        return;
}

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
        RtcClockRecord r;
        std::memcpy(&r, rtcArea(), sizeof(r));
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
        std::memcpy(rtcArea(), &r, sizeof(r));
        return;
}

void Detail::setLibcTime(int64_t wallNs) {
        const timeval tv = {.tv_sec = static_cast<time_t>(wallNs / 1'000'000'000),
                            .tv_usec = static_cast<suseconds_t>(wallNs % 1'000'000'000 / 1000)};
        (void)settimeofday(&tv, nullptr);
        return;
}

void Detail::fillRandom(void *data, size_t size) {
        esp_fill_random(data, size);
        return;
}

uint32_t Detail::randomWord() {
        return REG_READ(WDEV_RND_REG);
}

void Detail::fillBootEntropy(void *data, size_t size) {
        // The bootloader turned it off before starting us (bootloader_utility.c):
        // without it the generator has only its secondary source (devplan/random.md).
        bootloader_random_enable();
        esp_fill_random(data, size);
        bootloader_random_disable();
        return;
}

BMDS_NAMESPACE_END
