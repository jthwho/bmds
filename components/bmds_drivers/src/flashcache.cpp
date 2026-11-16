/**
 * @file      flashcache.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IDF's start and end of a flash operation, wrapped (CMakeLists.txt): the
 * flash marked busy, and the IRAM-safe interrupts inside waited for, before
 * the cache is suspended (bmds/flashcache.h).
 */

#include <bmds/flashcache.h>
#include <esp_attr.h>

BMDS_NAMESPACE_BEGIN

namespace FlashCache {

        // Internal RAM, read and written with the cache suspended.
        DRAM_ATTR uint32_t users = 0;
        DRAM_ATTR uint32_t flashOps = 0;

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
        return;
}

} // extern "C"
