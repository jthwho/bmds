/**
 * @file      imagewindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs' instances sharing their image's pages through the MMU (devplan/programs.md §14.2),
 * in ranges of the PSRAM window (devplan/psramwindow.md §3a). The caches are tagged by address,
 * not by page (§2): a shared page is written back through the address it was written at before
 * it is mapped; a private page's lines under its heap address are dropped and it is written
 * through the range; a range is invalidated after mapping (no stale lines of an earlier
 * instance) and before unmapping (a dirty line written back to an unmapped address goes nowhere).
 */

#include <algorithm>
#include <cstring>
#include <esp_cache.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/programimage.h>
#include <bmds/psramwindow.h>

#include "imagewindow.h"

BMDS_NAMESPACE_BEGIN

namespace ImageWindow {

        namespace {

                constexpr uint32_t Page = ProgramImage::SharePageBytes;
                static_assert(Page == PsramWindow::PageBytes, "an image's page is the MMU's");

                struct State {
                                Mutex    lock;      // the counts
                                uint32_t limit = 0; // the window's pages programs may hold at once
                                Usage    usage;
                };

                constinit State *state = nullptr; // set once by init()

                void freePrivate(uint8_t *block, uint8_t **privatePages) {
                        Memory::free(block);
                        Memory::free(privatePages);
                        return;
                }

        } // namespace

        Error init(size_t bytes) {
                if (state != nullptr || bytes == 0) return Error::Ok;
                if (!PsramWindow::isReady()) {
                        bmdsWarn("ImageWindow: no PSRAM window: programs as plain copies");
                        return Error::NotSupported;
                }
                void *memory = Memory::alloc(MemSpace::Internal, sizeof(State));
                if (memory == nullptr) return Error::NoMem;
                State         *s = ::new (memory) State();
                const uint32_t pages = uint32_t(bytes / Page);
                s->limit = pages < PsramWindow::pages() ? pages : PsramWindow::pages();
                s->usage.pages = s->limit;
                state = s;
                bmdsInfo("ImageWindow: up to %u KB of the PSRAM window for programs' pages",
                         unsigned(s->limit * (Page / 1024)));
                return Error::Ok;
        }

        bool isEnabled() {
                return state != nullptr;
        }

        bool contains(const void *p, size_t bytes) {
                return state != nullptr && PsramWindow::contains(p, bytes);
        }

        Error map(ImageCache &cache, ImageCache::Entry &entry, Mapping &out) {
                out = Mapping();
                State *s = state;
                if (s == nullptr || !entry.hasPages()) return Error::NotSupported;
                const ProgramImage::Info &info = entry.info();
                const uint32_t            pages = ProgramImage::pageCount(info.imageSize);
                {
                        Mutex::Locker locker(s->lock, WaitForever);
                        if (s->usage.used + pages > s->limit) return Error::NoMem;
                        s->usage.used += pages; // held while mapping
                }
                uint8_t *const base = PsramWindow::takeRange(pages, false);
                auto           giveBack = [&](uint8_t *block, uint8_t **privatePages) {
                        freePrivate(block, privatePages);
                        if (base != nullptr) PsramWindow::giveRange(base, pages);
                        Mutex::Locker locker(s->lock, WaitForever);
                        s->usage.used -= pages;
                };
                if (base == nullptr) {
                        giveBack(nullptr, nullptr);
                        return Error::NoMem;
                }

                // The private pages: one 64 KB-aligned block for all of them, from the heap (any
                // physical pages will do; a window block of its own would be mapped twice). A block
                // a page left the heap a gap just under 64 KB before each, which no other page
                // could use: 11 instances of a 16-private-page program cut the largest free block
                // from 23.6 to 1.1 MB, with 12.5 MB free.
                const uint32_t shared = ProgramImage::sharedPageCount(info);
                auto         **privatePages =
                        static_cast<uint8_t **>(Memory::alloc(MemSpace::Psram, pages * sizeof(uint8_t *)));
                Memory::Options heapPages;
                heapPages.alignBytes = Page;
                heapPages.heapOnly = true;
                auto *block = pages > shared ? static_cast<uint8_t *>(Memory::alloc(
                                                       MemSpace::Psram, size_t(pages - shared) * Page, heapPages))
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
                if (block != nullptr) PsramWindow::invalidate(block, size_t(pages - shared) * Page);
                for (uint32_t p = 0; p < pages; ++p) {
                        const uint8_t *from =
                                privatePages[p] != nullptr ? privatePages[p] : entry.bytes() + size_t(p) * Page;
                        PsramWindow::mapPage(base + size_t(p) * Page, from);
                }
                PsramWindow::invalidate(base, size_t(pages) * Page);
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
                s->usage.instances += 1;
                s->usage.shared += shared;
                s->usage.mapped += 1;
                return Error::Ok;
        }

        void unmap(Mapping &m) {
                State *s = state;
                if (!m.isMapped() || s == nullptr) return;
                PsramWindow::invalidate(m.base, size_t(m.pages) * Page); // the instance's dirty lines die with it
                PsramWindow::clear(m.base, m.pages);
                PsramWindow::giveRange(m.base, m.pages);
                freePrivate(m.privateBlock, m.privatePages);
                m.cache->release(m.entry);
                {
                        Mutex::Locker locker(s->lock, WaitForever);
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
