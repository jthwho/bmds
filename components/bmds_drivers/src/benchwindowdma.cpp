/**
 * @file      benchwindowdma.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The DMA engines through PSRAM window addresses (devplan/psramwindow.md step 4): each one copies
 * from a window buffer into a heap buffer and back, and the result is checked word by word.
 * The DW-GDMA (the display's scan), the 2D-DMA (BlitChain: the window manager's and surfaces'
 * copies), the PPA (scaling and expansion) and the JPEG codec (decoding into, encoding from).
 * The AXI GDMA and the SD card were checked before (memory.window.speed, programs.md §14.2).
 */

#include <cstdio>
#include <cstring>
#include <esp_cache.h>
#include <esp_private/dw_gdma.h>
#include <bmds/bench.h>
#include <bmds/blitchain.h>
#include <bmds/jpegcodec.h>
#include <bmds/jpegheader.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/ppa.h>
#include <bmds/psramwindow.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   Page = PsramWindow::PageBytes;
        constexpr uint16_t Side = 256;              // a picture's width and height
        constexpr size_t   Bytes = 4 * Page;        // 256 KB: room for 256 × 256 × 3 and its padding
        constexpr size_t   Line = Memory::DmaAlign; // the L2 cache's line

        // A buffer at window addresses: heap pages mapped into a range of the window.
        struct WindowBuffer {
                        uint8_t *heap = nullptr;
                        uint8_t *at = nullptr;
                        WindowBuffer() {
                                Memory::Options o;
                                o.alignBytes = Page;
                                o.heapOnly = true;
                                heap = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Bytes + Line, o));
                                at = heap != nullptr ? PsramWindow::takeRange(Bytes / Page, false) : nullptr;
                                if (at == nullptr) return;
                                PsramWindow::invalidate(heap, Bytes);
                                PsramWindow::mapHeap(at, heap, Bytes / Page);
                                PsramWindow::invalidate(at, Bytes);
                        }
                        ~WindowBuffer() {
                                if (at != nullptr) {
                                        PsramWindow::invalidate(at, Bytes);
                                        PsramWindow::clear(at, Bytes / Page);
                                        PsramWindow::giveRange(at, Bytes / Page);
                                }
                                Memory::free(heap);
                        }
        };

        // A plain DMA buffer on the heap.
        struct HeapBuffer {
                        uint8_t *at = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, Bytes, Line));
                        ~HeapBuffer() { Memory::free(at); }
        };

        uint32_t patternWord(size_t index, uint32_t seed) {
                return uint32_t(index) * 2654435761u ^ seed;
        }

        // A pattern written by the CPU and pushed to PSRAM; the other buffer cleared the same way.
        void prepare(uint8_t *from, uint8_t *to, uint32_t seed) {
                auto *w = reinterpret_cast<uint32_t *>(from);
                for (size_t i = 0; i < Bytes / 4; ++i) w[i] = patternWord(i, seed);
                std::memset(to, 0, Bytes);
                (void)esp_cache_msync(from, Bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                (void)esp_cache_msync(to, Bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                return;
        }

        // The first @p bytes of @p to, read from PSRAM, against the pattern.
        size_t wrongWords(uint8_t *to, size_t bytes, uint32_t seed) {
                PsramWindow::invalidate(to, Bytes);
                const auto *w = reinterpret_cast<const uint32_t *>(to);
                size_t      wrong = 0;
                for (size_t i = 0; i < bytes / 4; ++i) wrong += w[i] != patternWord(i, seed) ? 1 : 0;
                return wrong;
        }

        // Each direction of one engine: window to heap, heap to window.
        template <typename Copy> Error bothWays(BenchContext &ctx, const char *engine, size_t checked, Copy &&copy) {
                WindowBuffer window;
                HeapBuffer   heap;
                if (window.at == nullptr || heap.at == nullptr) return Error::NoMem;
                for (int way = 0; way < 2; ++way) {
                        uint8_t *from = way == 0 ? window.at : heap.at;
                        uint8_t *to = way == 0 ? heap.at : window.at;
                        prepare(from, to, 0xa5000000u + unsigned(way));
                        const Error  err = copy(from, to);
                        const size_t wrong = err.isOk() ? wrongWords(to, checked, 0xa5000000u + unsigned(way)) : 0;
                        ctx.note("%s, %s: %s, %u of %u words wrong", engine,
                                 way == 0 ? "window to heap" : "heap to window", err.desc(), unsigned(wrong),
                                 unsigned(checked / 4));
                        if (err.isOk() && wrong != 0) return Error::CorruptData;
                }
                return Error::Ok;
        }

        // ---- The DW-GDMA: one block, memory to memory, as the scan reads surface 0

        // In IRAM, its argument in internal RAM: the driver refuses anything else.
        BMDS_ISR_ATTR bool dwDone(dw_gdma_channel_handle_t, const dw_gdma_trans_done_event_data_t *, void *arg) {
                static_cast<Semaphore *>(arg)->releaseFromIsr();
                return false;
        }

        // As IDF's own test sets one up (esp_hw_support's test_dw_gdma.c), its bursts 64-bit wide.
        Error dwCopy(uint8_t *from, uint8_t *to) {
                dw_gdma_channel_alloc_config_t c = {};
                c.src.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS;
                c.src.role = DW_GDMA_ROLE_MEM;
                c.src.num_outstanding_requests = 1;
                c.dst = c.src;
                c.flow_controller = DW_GDMA_FLOW_CTRL_SELF;
                c.chan_priority = 1;
                dw_gdma_channel_handle_t chan = nullptr;
                if (esp_err_t e = dw_gdma_new_channel(&c, &chan); e != ESP_OK) return Error::fromEspErr(e);
                void *mem = Memory::alloc(MemSpace::Internal, sizeof(Semaphore));
                if (mem == nullptr) {
                        (void)dw_gdma_del_channel(chan);
                        return Error::NoMem;
                }
                Semaphore                &done = *::new (mem) Semaphore(1, 0);
                dw_gdma_event_callbacks_t cbs = {};
                cbs.on_full_trans_done = dwDone;
                esp_err_t                       e = dw_gdma_channel_register_event_callbacks(chan, &cbs, &done);
                dw_gdma_block_transfer_config_t block = {};
                block.src.addr = reinterpret_cast<uint32_t>(from);
                block.src.burst_mode = DW_GDMA_BURST_MODE_INCREMENT;
                block.src.width = DW_GDMA_TRANS_WIDTH_64;
                block.src.burst_items = DW_GDMA_BURST_ITEMS_4;
                block.dst = block.src;
                block.dst.addr = reinterpret_cast<uint32_t>(to);
                block.size = Bytes / 8;
                if (e == ESP_OK) e = dw_gdma_channel_config_transfer(chan, &block);
                if (e == ESP_OK) e = dw_gdma_channel_enable_ctrl(chan, true);
                // A copy that never ends is left disabled, not aborted (an abort waits for it).
                const Error err = e == ESP_OK ? done.acquire(1000) : Error::fromEspErr(e);
                (void)dw_gdma_channel_enable_ctrl(chan, false);
                (void)dw_gdma_del_channel(chan);
                done.~Semaphore();
                Memory::free(mem);
                return err;
        }

        // The DW-GDMA heap to heap first: the setup checked before the window is.
        Error dwControl(BenchContext &ctx) {
                HeapBuffer a;
                HeapBuffer b;
                if (a.at == nullptr || b.at == nullptr) return Error::NoMem;
                prepare(a.at, b.at, 0x11);
                const Error  err = dwCopy(a.at, b.at);
                const size_t wrong = err.isOk() ? wrongWords(b.at, Bytes, 0x11) : 0;
                ctx.note("DW-GDMA, heap to heap: %s, %u words wrong", err.desc(), unsigned(wrong));
                return err.isOk() && wrong != 0 ? Error(Error::CorruptData) : err;
        }

        // ---- The 2D-DMA through BlitChain: a 256 × 256 copy of 2-byte pixels

        Error blitCopy(uint8_t *from, uint8_t *to) {
                BlitChain chain;
                Error     err = chain.start(BlitChain::Conversion::None, 1);
                if (err.isError()) return err;
                const BlitChain::Picture src{from, Side, Side, 2};
                const BlitChain::Picture dst{to, Side, Side, 2};
                err = chain.add(src, 0, 0, dst, 0, 0, Side, Side);
                if (err.isOk()) err = chain.run(1000);
                return err;
        }

        // ---- The PPA: 256 × 256 RGB565 scaled at 1×

        Error ppaCopy(uint8_t *from, uint8_t *to) {
                Ppa   ppa;
                Error err = ppa.start();
                if (err.isError()) return err;
                const Ppa::Picture src{from, Side, Side, Ppa::Format::Rgb565};
                const Ppa::Picture dst{to, Side, Side, Ppa::Format::Rgb565};
                err = ppa.scale(src, Rect{0, 0, Side, Side}, dst, 0, 0, 16, 16, 1000);
                if (err.isOk()) err = ppa.wait(1000);
                return err;
        }

        // ---- The JPEG codec: a picture encoded from the window and from the heap gives the same
        // bytes; decoded into each, the same pixels.

        struct Collected {
                        uint8_t *bytes = nullptr;
                        size_t   size = 0;
                        size_t   room = 0;
        };

        Error collect(void *arg, ByteSpan data) {
                auto *c = static_cast<Collected *>(arg);
                if (c->size + data.size() > c->room) return Error::Full;
                std::memcpy(c->bytes + c->size, data.data(), data.size());
                c->size += data.size();
                return Error::Ok;
        }

        Error runJpeg(BenchContext &ctx) {
                auto [codec, cerr] = JpegCodec::codec(0);
                if (cerr.isError()) {
                        ctx.note("JPEG: no codec (%s)", cerr.desc());
                        return Error::Ok;
                }
                WindowBuffer window;
                HeapBuffer   heap;
                uint8_t     *encoded[2] = {static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Bytes)),
                                           static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Bytes))};
                Error err = window.at == nullptr || heap.at == nullptr || encoded[0] == nullptr || encoded[1] == nullptr
                                    ? Error(Error::NoMem)
                                    : Error(Error::Ok);
                Collected out[2];
                // A smooth picture (a JPEG of noise would only measure the noise).
                for (int i = 0; i < 2 && err.isOk(); ++i) {
                        uint8_t *p = i == 0 ? window.at : heap.at;
                        for (uint32_t y = 0; y < Side; ++y)
                                for (uint32_t x = 0; x < Side; ++x) {
                                        uint8_t *px = p + (size_t(y) * Side + x) * 3;
                                        px[0] = uint8_t(x);
                                        px[1] = uint8_t(y);
                                        px[2] = uint8_t(x + y);
                                }
                        (void)esp_cache_msync(p, Bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                        ImageBuffer in;
                        in.pixels = p;
                        in.width = Side;
                        in.height = Side;
                        in.stride = size_t(Side) * 3;
                        in.format = PixelFormat::Bgr24;
                        in.capacity = Bytes;
                        out[i] = Collected{encoded[i], 0, Bytes};
                        err = codec->encode(in, JpegCodec::Encode{}, collect, &out[i], 1000);
                }
                if (err.isOk())
                        ctx.note("JPEG encode: %u bytes from the window, %u from the heap, %s", unsigned(out[0].size),
                                 unsigned(out[1].size),
                                 out[0].size == out[1].size && std::memcmp(encoded[0], encoded[1], out[0].size) == 0
                                         ? "the same"
                                         : "DIFFERENT");
                // Decoded into each (zero-copy: the codec's DMA writes the buffer itself).
                if (err.isOk()) {
                        auto [h, herr] = JpegHeader::parse(ByteSpan(encoded[1], out[1].size));
                        err = herr;
                        const JpegCodec::Layout l = JpegCodec::layoutFor(h, PixelFormat::Bgr24);
                        bool                    zeroCopy = true;
                        for (int i = 0; i < 2 && err.isOk(); ++i) {
                                ImageBuffer o;
                                o.pixels = i == 0 ? window.at : heap.at;
                                o.width = Side;
                                o.height = Side;
                                o.stride = l.stride;
                                o.format = PixelFormat::Bgr24;
                                o.capacity = Bytes;
                                zeroCopy = zeroCopy && JpegCodec::isZeroCopy(h, o);
                                std::memset(o.pixels, 0, Bytes);
                                (void)esp_cache_msync(o.pixels, Bytes,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                                err = codec->decode(ByteSpan(encoded[1], out[1].size), o, JpegCodec::Decode{}, 1000);
                        }
                        if (err.isOk()) {
                                PsramWindow::invalidate(window.at, Bytes);
                                PsramWindow::invalidate(heap.at, Bytes);
                                const size_t rows = size_t(Side) * l.stride;
                                ctx.note("JPEG decode%s: into the window and the heap, %s",
                                         zeroCopy ? " (zero-copy)" : " (BOUNCED)",
                                         std::memcmp(window.at, heap.at, rows) == 0 ? "the same pixels"
                                                                                    : "DIFFERENT pixels");
                        }
                }
                Memory::free(encoded[0]);
                Memory::free(encoded[1]);
                return err;
        }

        Error runDma(BenchContext &ctx) {
                if (!PsramWindow::isReady()) return Error::NotSupported;
                Error err = dwControl(ctx);
                if (err.isOk()) err = bothWays(ctx, "DW-GDMA", Bytes, dwCopy);
                if (err.isOk()) err = bothWays(ctx, "2D-DMA", size_t(Side) * Side * 2, blitCopy);
                if (err.isOk()) err = bothWays(ctx, "PPA", size_t(Side) * Side * 2, ppaCopy);
                if (err.isOk()) err = runJpeg(ctx);
                return err;
        }

} // namespace

BMDS_BENCH(memoryWindowDma, "memory.window.dma", runDma, BenchQuick,
           "The DMA engines through PSRAM window addresses, both ways, checked word by word: the DW-GDMA (the scan's), "
           "the 2D-DMA, the PPA and the JPEG codec")

BMDS_NAMESPACE_END
