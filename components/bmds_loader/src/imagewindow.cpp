/**
 * @file      imagewindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs' instances sharing their image's pages through the MMU (devplan/programs.md §14.2).
 * The caches are tagged by address, not by page (§2): a shared page is written back through
 * the address it was written at before it is mapped; a private page's lines under its heap
 * address are dropped and it is written through the range; a range is invalidated after mapping
 * (no stale lines of an earlier instance) and before unmapping (a dirty line written back to an
 * unmapped address goes nowhere).
 */

#include <algorithm>
#include <cstring>
#include <esp_cache.h>
#include <esp_private/esp_mmu_map_private.h>
#include <freertos/FreeRTOS.h>
#include <hal/mmu_ll.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/programimage.h>

#include "imagewindow.h"

BMDS_NAMESPACE_BEGIN

namespace ImageWindow {

        namespace {

                constexpr uint32_t Page = ProgramImage::SharePageBytes;
                constexpr uint32_t MaxPages = 512; // 32 MB: the PSRAM window's free half

                struct State {
                                Mutex    lock; // the bitmap and the counts
                                uint8_t *start = nullptr;
                                uint32_t pages = 0;
                                uint32_t used[MaxPages / 32] = {};
                                Usage    usage;
                };

                constinit State *state = nullptr;                        // set once by init()
                portMUX_TYPE     mmuLock = portMUX_INITIALIZER_UNLOCKED; // the index/content register pair

                bool isUsed(const State &s, uint32_t p) {
                        return (s.used[p / 32] & (1u << (p % 32))) != 0;
                }

                void setUsed(State &s, uint32_t first, uint32_t count, bool on) {
                        for (uint32_t p = first; p < first + count; ++p) {
                                if (on)
                                        s.used[p / 32] |= 1u << (p % 32);
                                else
                                        s.used[p / 32] &= ~(1u << (p % 32));
                        }
                        return;
                }

                // First fit: @p count free pages in a row; their first, or -1.
                int32_t findRange(const State &s, uint32_t count) {
                        uint32_t run = 0;
                        for (uint32_t p = 0; p < s.pages; ++p) {
                                run = isUsed(s, p) ? 0 : run + 1;
                                if (run == count) return int32_t(p + 1 - count);
                        }
                        return -1;
                }

                // The physical page behind heap address @p p: read from the MMU's table, not with
                // esp_mmu_vaddr_to_paddr(), which turns the caches off for it (asserting on a
                // program's stack, which is in PSRAM).
                uint32_t physicalPage(const uint8_t *p) {
                        const uint32_t id = mmu_ll_get_entry_id(MMU_LL_PSRAM_MMU_ID, uint32_t(uintptr_t(p)));
                        portENTER_CRITICAL(&mmuLock);
                        const uint32_t entry = mmu_ll_read_entry(MMU_LL_PSRAM_MMU_ID, id);
                        portEXIT_CRITICAL(&mmuLock);
                        return entry & SOC_MMU_PSRAM_VALID_VAL_MASK;
                }

                // Points the window's page at @p vaddr to the physical page behind @p from (a heap
                // address), or makes it invalid (@p from null).
                void setEntry(uint8_t *vaddr, const uint8_t *from) {
                        const uint32_t id = mmu_ll_get_entry_id(MMU_LL_PSRAM_MMU_ID, uint32_t(uintptr_t(vaddr)));
                        const uint32_t page = from != nullptr ? physicalPage(from) : 0;
                        portENTER_CRITICAL(&mmuLock);
                        if (from != nullptr)
                                mmu_ll_write_entry(MMU_LL_PSRAM_MMU_ID, id, page, MMU_TARGET_PSRAM0);
                        else
                                mmu_ll_set_entry_invalid(MMU_LL_PSRAM_MMU_ID, id);
                        portEXIT_CRITICAL(&mmuLock);
                        return;
                }

                // Drops the range's lines from every cache, a page at a time (each a critical
                // section of its own in esp_cache_msync()).
                void invalidate(uint8_t *base, uint32_t pages) {
                        for (uint32_t p = 0; p < pages; ++p)
                                (void)esp_cache_msync(base + size_t(p) * Page, Page, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        return;
                }

                void freePrivate(uint8_t *block, uint8_t **privatePages) {
                        Memory::free(block);
                        Memory::free(privatePages);
                        return;
                }

        } // namespace

        Error init(size_t bytes) {
                if (state != nullptr || bytes == 0) return Error::Ok;
                const uint32_t pages = uint32_t(bytes / Page < MaxPages ? bytes / Page : MaxPages);
                const void    *start = nullptr;
                // Executable (IDF then refuses "writable" or "8-bit", though on the P4 the
                // addresses are the same for data: programs read and write their pages there).
                const esp_err_t err = esp_mmu_map_reserve_block_with_caps(
                        size_t(pages) * Page, mmu_mem_caps_t(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_32BIT), MMU_TARGET_PSRAM0,
                        &start);
                if (err != ESP_OK) {
                        bmdsWarn("ImageWindow: no %u KB of addresses for programs' shared pages (%s): plain copies",
                                 unsigned(pages * (Page / 1024)), esp_err_to_name(err));
                        return Error::fromEspErr(err);
                }
                void *memory = Memory::alloc(MemSpace::Internal, sizeof(State));
                if (memory == nullptr) return Error::NoMem;
                State *s = ::new (memory) State();
                s->start = static_cast<uint8_t *>(const_cast<void *>(start));
                s->pages = pages;
                s->usage.pages = pages;
                for (uint32_t p = 0; p < pages; ++p) setEntry(s->start + size_t(p) * Page, nullptr);
                state = s;
                Memory::setProgramWindow(s->start, size_t(pages) * Page); // DMA reaches it (the SD card's)
                bmdsInfo("ImageWindow: %u KB at %p for programs' shared pages", unsigned(pages * (Page / 1024)),
                         s->start);
                return Error::Ok;
        }

        bool isEnabled() {
                return state != nullptr;
        }

        bool contains(const void *p, size_t bytes) {
                const State *s = state;
                if (s == nullptr) return false;
                const uintptr_t at = uintptr_t(p), start = uintptr_t(s->start);
                return at >= start && at + bytes >= at && at + bytes <= start + size_t(s->pages) * Page;
        }

        Error map(ImageCache &cache, ImageCache::Entry &entry, Mapping &out) {
                out = Mapping();
                State *s = state;
                if (s == nullptr || !entry.hasPages()) return Error::NotSupported;
                const ProgramImage::Info &info = entry.info();
                const uint32_t            pages = ProgramImage::pageCount(info.imageSize);
                int32_t                   first = -1;
                {
                        Mutex::Locker locker(s->lock, WaitForever);
                        first = findRange(*s, pages);
                        if (first >= 0) setUsed(*s, uint32_t(first), pages, true);
                }
                if (first < 0) return Error::NoMem;
                uint8_t *const base = s->start + size_t(first) * Page;
                auto           giveBack = [&](uint8_t *block, uint8_t **privatePages) {
                        freePrivate(block, privatePages);
                        Mutex::Locker locker(s->lock, WaitForever);
                        setUsed(*s, uint32_t(first), pages, false);
                };

                // The private pages: one 64 KB-aligned block for all of them (any physical pages
                // will do). A block a page left the heap a gap just under 64 KB before each, which
                // no other page could use: 11 instances of a 16-private-page program cut the
                // largest free block from 23.6 to 1.1 MB, with 12.5 MB free.
                const uint32_t shared = ProgramImage::sharedPageCount(info);
                auto         **privatePages =
                        static_cast<uint8_t **>(Memory::alloc(MemSpace::Psram, pages * sizeof(uint8_t *)));
                auto *block = pages > shared ? static_cast<uint8_t *>(Memory::allocAligned(
                                                       MemSpace::Psram, size_t(pages - shared) * Page, Page))
                                             : nullptr;
                if (privatePages == nullptr || (block == nullptr && pages > shared)) {
                        giveBack(block, privatePages);
                        return Error::NoMem;
                }
                for (uint32_t p = 0, k = 0; p < pages; ++p)
                        privatePages[p] = ProgramImage::isSharedPage(info, p) ? nullptr : block + size_t(k++) * Page;
                // The block's lines under its heap addresses dropped (whoever had the memory last may
                // have left dirty ones, which would land on it later), every page mapped (the entry's
                // bytes were written back when it was read), the range's stale lines dropped; then the
                // private pages placed through the range itself, so the program finds its data in the
                // cache as written. What it may execute (below the writable data: the code and
                // constants moved to the end, programs.md §14.3) is written back: each core fetches
                // instructions past the data cache, and found libgcc's lui unpatched otherwise (a load
                // from the bare offset). The data and the bss stay in the cache: the write-back of two
                // pages took ~0.5 ms (§19).
                if (block != nullptr)
                        (void)esp_cache_msync(block, size_t(pages - shared) * Page, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                for (uint32_t p = 0; p < pages; ++p) {
                        const uint8_t *from =
                                privatePages[p] != nullptr ? privatePages[p] : entry.bytes() + size_t(p) * Page;
                        setEntry(base + size_t(p) * Page, from);
                }
                invalidate(base, pages);
                for (uint32_t p = 0; p < pages; ++p)
                        if (privatePages[p] != nullptr) privatePages[p] = base + size_t(p) * Page;
                ProgramImage::placePages(info, entry.bytes(), entry.patches(), privatePages, uint32_t(uintptr_t(base)));
                uint32_t firstPrivate = pages;
                for (uint32_t p = 0; p < pages && firstPrivate == pages; ++p)
                        if (privatePages[p] != nullptr) firstPrivate = p;
                const uint32_t codeStart = firstPrivate * Page;
                const uint32_t codeEnd = std::min(info.writableOffset, info.imageSize);
                if (codeEnd > codeStart)
                        (void)esp_cache_msync(base + codeStart, codeEnd - codeStart,
                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                cache.pin(&entry);
                out.base = base;
                out.pages = pages;
                out.shared = shared;
                out.privatePages = privatePages;
                out.privateBlock = block;
                out.entry = &entry;
                out.cache = &cache;
                Mutex::Locker locker(s->lock, WaitForever);
                s->usage.used += pages;
                s->usage.instances += 1;
                s->usage.shared += shared;
                s->usage.mapped += 1;
                return Error::Ok;
        }

        void unmap(Mapping &m) {
                State *s = state;
                if (!m.isMapped() || s == nullptr) return;
                invalidate(m.base, m.pages); // the instance's dirty lines die with it
                for (uint32_t p = 0; p < m.pages; ++p) setEntry(m.base + size_t(p) * Page, nullptr);
                freePrivate(m.privateBlock, m.privatePages);
                m.cache->release(m.entry);
                {
                        Mutex::Locker  locker(s->lock, WaitForever);
                        const uint32_t first = uint32_t((m.base - s->start) / Page);
                        setUsed(*s, first, m.pages, false);
                        s->usage.used -= m.pages;
                        s->usage.instances -= 1;
                        s->usage.shared -= m.shared;
                }
                m = Mapping();
                return;
        }

        Usage usage() {
                State *s = state;
                if (s == nullptr) return Usage();
                Mutex::Locker locker(s->lock, WaitForever);
                return s->usage;
        }

        void countFallback() {
                State *s = state;
                if (s == nullptr) return;
                Mutex::Locker locker(s->lock, WaitForever);
                ++s->usage.fallbacks;
                return;
        }

} // namespace ImageWindow

BMDS_NAMESPACE_END
