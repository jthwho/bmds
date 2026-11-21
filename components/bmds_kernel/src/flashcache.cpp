/**
 * @file      flashcache.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IDF's start and end of a flash operation, wrapped (CMakeLists.txt): the
 * flash marked busy, and the IRAM-safe interrupts inside waited for, before
 * the cache is suspended; at the end, the hooks interrupts deferred work for
 * (bmds/flashcache.h).
 */

#include <bmds/flashcache.h>
#include <esp_attr.h>

BMDS_NAMESPACE_BEGIN

namespace FlashCache {

        // Internal RAM, read and written with the cache suspended.
        DRAM_ATTR uint32_t users = 0;
        DRAM_ATTR uint32_t flashOps = 0;

        namespace {
                DRAM_ATTR AfterHook afterHooks[MaxAfterHooks] = {};
                DRAM_ATTR void     *afterArgs[MaxAfterHooks] = {};
                DRAM_ATTR uint32_t  afterCount = 0;
                DRAM_ATTR uint32_t  hooksRunning = 0;
        } // namespace

        Error addAfterHook(AfterHook hook, void *arg) {
                // Set at start, from one task; read by the flash's end with the count last.
                if (afterCount == MaxAfterHooks) return Error::Full;
                afterHooks[afterCount] = hook;
                afterArgs[afterCount] = arg;
                __atomic_store_n(&afterCount, afterCount + 1, __ATOMIC_RELEASE);
                return Error::Ok;
        }

        void waitHooks() {
                while (__atomic_load_n(&hooksRunning, __ATOMIC_SEQ_CST) != 0) {}
                return;
        }

} // namespace FlashCache

BMDS_NAMESPACE_END

extern "C" {

void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);

// Busy first, then the interrupts inside (a body's worth: microseconds) left, then IDF stalls the other
// core and suspends the cache. An interrupt entering from here on finds the flash busy.
void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void) {
        __atomic_fetch_add(&bmds::FlashCache::flashOps, 1u, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&bmds::FlashCache::users, __ATOMIC_SEQ_CST) != 0) {}
        __real_spi_flash_disable_interrupts_caches_and_other_cpu();
        return;
}

// The cache resumed first, then the flash no longer busy.
void IRAM_ATTR __wrap_spi_flash_enable_interrupts_caches_and_other_cpu(void) {
        __real_spi_flash_enable_interrupts_caches_and_other_cpu();
        __atomic_fetch_sub(&bmds::FlashCache::flashOps, 1u, __ATOMIC_SEQ_CST);
        // What interrupts deferred while the flash was busy, raised again.
        __atomic_fetch_add(&bmds::FlashCache::hooksRunning, 1u, __ATOMIC_SEQ_CST);
        const uint32_t n = __atomic_load_n(&bmds::FlashCache::afterCount, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; i < n; ++i) bmds::FlashCache::afterHooks[i](bmds::FlashCache::afterArgs[i]);
        __atomic_fetch_sub(&bmds::FlashCache::hooksRunning, 1u, __ATOMIC_SEQ_CST);
        return;
}

} // extern "C"
