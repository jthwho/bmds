/**
 * @file      psramwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The PSRAM window (devplan/psramwindow.md §3a): the addresses after the heap's, heap pages
 * mapped there again. The MMU's index/content register pair is written under one spinlock.
 */

#include <atomic>
#include <esp_cache.h>
#include <esp_mmu_map.h>
#include <esp_private/esp_mmu_map_private.h>
#include <freertos/FreeRTOS.h>
#include <hal/mmu_ll.h>
#include <soc/ext_mem_defs.h>
#include <bmds/log.h>
#include <bmds/mutex.h>
#include <bmds/pagewindow.h>
#include <bmds/psramwindow.h>
#include <bmds/staticobject.h>
#include "memorywindow.h"

BMDS_NAMESPACE_BEGIN

namespace PsramWindow {

        namespace {

                constexpr uintptr_t HeapBase = SOC_MMU_PSRAM_VADDR_BASE; // the heap's PSRAM, mapped in order

                constinit std::atomic<uintptr_t> start{0};
                constinit uint32_t               pageCount = 0;
                constinit uint32_t               entryFlags = 0; // valid, PSRAM, encrypted if so
                constinit bool                   heapInOrder = false;
                constinit StaticObject<Mutex>    lock;
                constinit PageWindow             ranges;
                portMUX_TYPE                     mmuLock = portMUX_INITIALIZER_UNLOCKED;

                uint32_t readEntry(uint32_t id) {
                        portENTER_CRITICAL(&mmuLock);
                        const uint32_t entry = mmu_ll_read_entry(MMU_LL_PSRAM_MMU_ID, id);
                        portEXIT_CRITICAL(&mmuLock);
                        return entry;
                }

                uint32_t entryOf(const void *p) {
                        return mmu_ll_get_entry_id(MMU_LL_PSRAM_MMU_ID, uint32_t(uintptr_t(p)));
                }

                // Written as mmu_ll_write_entry() writes them, without its eFuse reads.
                void writeEntries(uint32_t id, uint32_t firstPage, uint32_t count) {
                        portENTER_CRITICAL(&mmuLock);
                        for (uint32_t i = 0; i < count; ++i) {
                                REG_WRITE(SPI_MEM_S_MMU_ITEM_INDEX_REG, id + i);
                                REG_WRITE(SPI_MEM_S_MMU_ITEM_CONTENT_REG, (firstPage + i) | entryFlags);
                        }
                        portEXIT_CRITICAL(&mmuLock);
                        return;
                }

        } // namespace

        Error init() {
                if (start.load() != 0) return Error::Ok;
                // Every address the heap leaves (IDF then refuses "writable" or "8-bit" with
                // "executable", though on the P4 the addresses are the same for data).
                size_t          free = 0;
                const esp_err_t lerr = esp_mmu_map_get_max_consecutive_free_block_size(
                        mmu_mem_caps_t(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_32BIT), MMU_TARGET_PSRAM0, &free);
                const uint32_t  want = uint32_t((lerr == ESP_OK ? free : 0) / PageBytes);
                const uint32_t  pages = want < PageWindow::MaxPages ? want : PageWindow::MaxPages;
                const void     *at = nullptr;
                const esp_err_t err = pages == 0 ? ESP_ERR_NO_MEM
                                                 : esp_mmu_map_reserve_block_with_caps(
                                                           size_t(pages) * PageBytes,
                                                           mmu_mem_caps_t(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_32BIT),
                                                           MMU_TARGET_PSRAM0, &at);
                if (err != ESP_OK) {
                        bmdsWarn("PsramWindow: no addresses (%s): large blocks from the heap only",
                                 esp_err_to_name(err));
                        return Error::fromEspErr(err);
                }
                lock.create();
                entryFlags = SOC_MMU_PSRAM_VALID | SOC_MMU_ACCESS_PSRAM |
                             (mmu_ll_cache_encryption_enabled() ? SOC_MMU_PSRAM_SENSITIVE : 0);
                const uint32_t first = entryOf(at);
                portENTER_CRITICAL(&mmuLock);
                for (uint32_t i = 0; i < pages; ++i) mmu_ll_set_entry_invalid(MMU_LL_PSRAM_MMU_ID, first + i);
                portEXIT_CRITICAL(&mmuLock);
                // The heap's pages in order behind its addresses (as IDF maps them): then a block's
                // physical pages follow its first.
                const uint32_t heapPages = uint32_t((uintptr_t(at) - HeapBase) / PageBytes);
                const uint32_t heapFirst = entryOf(reinterpret_cast<const void *>(HeapBase));
                const uint32_t page0 = readEntry(heapFirst) & SOC_MMU_PSRAM_VALID_VAL_MASK;
                bool           inOrder = true;
                for (uint32_t i = 1; i < heapPages && inOrder; ++i)
                        inOrder = (readEntry(heapFirst + i) & SOC_MMU_PSRAM_VALID_VAL_MASK) == page0 + i;
                heapInOrder = inOrder;
                ranges.reset(pages);
                pageCount = pages;
                start.store(uintptr_t(at), std::memory_order_release);
                bmdsInfo("PsramWindow: %u KB at %p%s", unsigned(pages * (PageBytes / 1024)), at,
                         inOrder ? "" : " (the heap's pages out of order: mapped a page at a time)");
                return MemoryWindow::init();
        }

        bool isReady() {
                return start.load(std::memory_order_acquire) != 0;
        }

        bool contains(const void *p, size_t bytes) {
                const uintptr_t s = start.load(std::memory_order_acquire);
                const uintptr_t at = uintptr_t(p);
                return s != 0 && at >= s && at + bytes >= at && at + bytes <= s + size_t(pageCount) * PageBytes;
        }

        uint8_t *base() {
                return reinterpret_cast<uint8_t *>(start.load(std::memory_order_acquire));
        }

        uint32_t pages() {
                return pageCount;
        }

        uint8_t *takeRange(uint32_t count, bool fromTop) {
                if (!isReady()) return nullptr;
                Mutex::Locker  locker(*lock, WaitForever);
                const uint32_t first = ranges.take(count, fromTop);
                return first == PageWindow::None ? nullptr : base() + size_t(first) * PageBytes;
        }

        bool takeRangeAt(uint8_t *at, uint32_t count) {
                if (!contains(at, 0)) return false;
                Mutex::Locker locker(*lock, WaitForever);
                return ranges.takeAt(uint32_t((at - base()) / PageBytes), count);
        }

        void giveRange(uint8_t *at, uint32_t count) {
                if (!contains(at, 0)) return;
                Mutex::Locker locker(*lock, WaitForever);
                ranges.give(uint32_t((at - base()) / PageBytes), count);
                return;
        }

        Usage usage() {
                Usage u;
                if (!isReady()) return u;
                Mutex::Locker locker(*lock, WaitForever);
                u.pages = ranges.pages();
                u.used = ranges.usedPages();
                u.longest = ranges.longestFree();
                return u;
        }

        uint32_t physicalPage(const void *p) {
                return readEntry(entryOf(p)) & SOC_MMU_PSRAM_VALID_VAL_MASK;
        }

        void mapHeap(uint8_t *at, const uint8_t *heap, uint32_t count) {
                if (heapInOrder && !contains(heap, 0)) { // a window block's runs aren't in order
                        // Sixteen entries a hold of the spinlock: about 3 µs with interrupts off.
                        const uint32_t id = entryOf(at);
                        const uint32_t page = physicalPage(heap);
                        for (uint32_t i = 0; i < count; i += 16)
                                writeEntries(id + i, page + i, count - i < 16 ? count - i : 16);
                        return;
                }
                for (uint32_t i = 0; i < count; ++i) mapPage(at + size_t(i) * PageBytes, heap + size_t(i) * PageBytes);
                return;
        }

        void mapPage(uint8_t *at, const uint8_t *heap) {
                writeEntries(entryOf(at), physicalPage(heap), 1);
                return;
        }

        void clear(uint8_t *at, uint32_t count) {
                const uint32_t id = entryOf(at);
                for (uint32_t i = 0; i < count; i += 16) {
                        portENTER_CRITICAL(&mmuLock);
                        for (uint32_t k = i; k < count && k < i + 16; ++k) {
                                REG_WRITE(SPI_MEM_S_MMU_ITEM_INDEX_REG, id + k);
                                REG_WRITE(SPI_MEM_S_MMU_ITEM_CONTENT_REG, SOC_MMU_PSRAM_INVALID);
                        }
                        portEXIT_CRITICAL(&mmuLock);
                }
                return;
        }

        void invalidate(const void *p, size_t bytes) {
                if (bytes != 0) (void)esp_cache_msync(const_cast<void *>(p), bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                return;
        }

} // namespace PsramWindow

BMDS_NAMESPACE_END
