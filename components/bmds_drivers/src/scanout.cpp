/**
 * @file      scanout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A surface on the DSI output (devplan/display.md §5c). The link and DPI
 * set-up follows the register sequence of IDF's DSI bus and DPI panel
 * (esp_lcd/dsi), on the HAL; the DMA streams the surface's shown buffer
 * from PSRAM as a circular list of blocks, re-pointed at a swap.
 */

#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_clk_tree.h>
#include <esp_cpu.h>
#include <esp_intr_alloc.h>
#include <esp_ldo_regulator.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <esp_private/dw_gdma.h>
#include <esp_private/cache_utils.h>
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/periph_ctrl.h>
#include <freertos/FreeRTOS.h>
#include <hal/axi_icm_ll.h>
#include <hal/cache_ll.h>
#include <hal/dw_gdma_ll.h>
#include <hal/mipi_dsi_hal.h>
#include <hal/mipi_dsi_ll.h>
#include <hal/mipi_dsi_periph.h>
#include <soc/reg_base.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/scanout.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t     TimeoutClockMhz = 10; // IDF's: the host's timeout clock
        constexpr uint32_t     EscapeClockMhz = 18;  // TX escape clock, 2-20 MHz
        constexpr unsigned int CheckMs = 100;        // the supervisor's look at the stream
        // No block sent for this long while running: a stuck stream raises no
        // further interrupt to act on.
        constexpr int64_t StuckUs = 50'000;
        constexpr int64_t ResyncGapUs = 50'000; // at most one resync for underruns this often
        // A surface switched to (show()) that stalls the stream this often in its first second is
        // given up for the one before.
        constexpr uint32_t FallbackStalls = 3;
        constexpr int64_t  FallbackProbationUs = 1'000'000;
        // The 2D-DMA's write regulator (devplan/display.md §5c, measured): 6 since
        // 2026-10-05, a full-screen fill 16 ms (5 gave 8 ms, and underran the scan
        // when a shell started meanwhile).
        constexpr uint32_t Dma2dWriteLevel = 6;

        Error espError(esp_err_t err) {
                return err == ESP_OK ? Error(Error::Ok) : Error::fromEspErr(err);
        }

        // An idle DW-GDMA channel held for good, so IDF's DW-GDMA group (and
        // its bus clock) outlives the scanout's own channel. IDF's
        // channel_destroy() releases the group, turning the clock off with the
        // last channel, before it frees the channel's shared interrupt: an
        // interrupt on that line in between reads the DMA's status register
        // with its clock off, a load access fault (seen on a mode change). It
        // has no interrupt and never runs; it takes one of the 4 channels.
        constinit dw_gdma_channel_handle_t dmaAnchor = nullptr;

        Error holdDmaGroup() {
                if (dmaAnchor != nullptr) return Error::Ok;
                dw_gdma_channel_alloc_config_t c = {};
                c.src.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS;
                c.src.role = DW_GDMA_ROLE_MEM;
                c.src.num_outstanding_requests = 1;
                c.dst.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS;
                c.dst.role = DW_GDMA_ROLE_MEM;
                c.dst.num_outstanding_requests = 1;
                c.flow_controller = DW_GDMA_FLOW_CTRL_SELF;
                c.chan_priority = 0;
                const esp_err_t err = dw_gdma_new_channel(&c, &dmaAnchor);
                return err == ESP_OK ? Error(Error::Ok) : Error::fromEspErr(err);
        }

} // namespace

// The DMA's circular list of descriptors, in blocks of `ChunkItems` (internal DMA RAM, 64 bytes
// each): 720 lines' 46 KB needn't be one free block. Written through their uncached addresses,
// linked through their cached ones (the DMA's view), as IDF's own list is.
struct Scanout::ItemList {
                static constexpr uint32_t ChunkItems = 64;
                static constexpr uint32_t MaxChunks = (1080 + ChunkItems - 1) / ChunkItems; // 1080 lines at most

                dw_gdma_link_list_item_t *chunk[MaxChunks] = {}; // cached addresses
                uint32_t                  base[MaxChunks] = {};  // the same, as the DMA reports them
                uint32_t                  chunks = 0;
                uint32_t                  count = 0;

                BMDS_ISR_ATTR dw_gdma_link_list_item_t *item(uint32_t k) const {
                        const auto c = reinterpret_cast<uint32_t>(chunk[k / ChunkItems]);
                        return reinterpret_cast<dw_gdma_link_list_item_t *>(CACHE_LL_L2MEM_NON_CACHE_ADDR(c)) +
                               k % ChunkItems;
                }

                // The descriptor at @p address (the DMA's, top bits masked), or `count`.
                BMDS_ISR_ATTR uint32_t indexOf(uint32_t address) const {
                        for (uint32_t c = 0; c < chunks; ++c) {
                                const uint32_t n = c + 1 < chunks ? ChunkItems : count - c * ChunkItems;
                                if (address >= base[c] && address < base[c] + n * sizeof(dw_gdma_link_list_item_t))
                                        return c * ChunkItems + (address - base[c]) / sizeof(dw_gdma_link_list_item_t);
                        }
                        return count;
                }

                Error allocate(uint32_t items) {
                        if (items == 0 || items > MaxChunks * ChunkItems) return Error::Invalid;
                        count = items;
                        chunks = (items + ChunkItems - 1) / ChunkItems;
                        for (uint32_t c = 0; c < chunks; ++c) {
                                const uint32_t n = c + 1 < chunks ? ChunkItems : items - c * ChunkItems;
                                const size_t   bytes = n * sizeof(dw_gdma_link_list_item_t);
                                chunk[c] = static_cast<dw_gdma_link_list_item_t *>(Memory::allocAligned(
                                        MemSpace::InternalDma, bytes, DW_GDMA_LL_LINK_LIST_ALIGNMENT));
                                if (chunk[c] == nullptr) {
                                        release();
                                        return Error::NoMem;
                                }
                                std::memset(chunk[c], 0, bytes);
                                // Written back and dropped from the cache: written uncached from here on.
                                (void)esp_cache_msync(chunk[c], bytes,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                                                              ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                                base[c] = reinterpret_cast<uint32_t>(chunk[c]) & 0x0fffffffu;
                        }
                        for (uint32_t k = 0; k < items; ++k) {
                                const uint32_t next = (k + 1) % items;
                                dw_gdma_ll_lli_set_next_item_addr(
                                        item(k),
                                        reinterpret_cast<uint32_t>(chunk[next / ChunkItems] + next % ChunkItems));
                                dw_gdma_ll_lli_set_link_list_master_port(item(k), DW_GDMA_LL_MASTER_PORT_MEMORY);
                        }
                        return Error::Ok;
                }

                void release() {
                        for (auto &c : chunk) {
                                Memory::free(c);
                                c = nullptr;
                        }
                        chunks = 0;
                        count = 0;
                        return;
                }
};

struct Scanout::Hardware {
                mipi_dsi_hal_context_t   hal = {};
                esp_ldo_channel_handle_t ldo = nullptr;
                dw_gdma_channel_handle_t dma = nullptr;
                ItemList                 list; // the descriptors
                intr_handle_t            bridgeIntr = nullptr;
                bool                     busClock = false;
                bool                     dpiClock = false;
                uint32_t                 pixelHz = 0; // the pixel clock as set
                uint32_t                 laneMbps = 0;
                uint8_t                  channel = 0; // the DMA channel's number
                // A descriptor's control word's upper half, valid, with and without its interrupt:
                // the same for every descriptor, so making one valid again is a single store into
                // uncached memory, not three read-modify-writes of its bit fields.
                uint32_t ctrlInterrupt = 0;
                uint32_t ctrlQuiet = 0;
};

// The interrupts: a block sent, the DMA at an invalid descriptor, the DSI
// bridge running dry.
struct ScanoutIsr {
                BMDS_ISR_ATTR static dw_gdma_link_list_item_t *item(const Scanout &s, uint32_t k) {
                        return s._hw->list.item(k);
                }

                // Where descriptor @p k reads in a buffer: its block, or in line mode the row its line
                // falls on.
                BMDS_ISR_ATTR static uint32_t offset(const Scanout &s, uint32_t k) {
                        return s._lineMode ? uint32_t(k * s._rows / s._items * s._stride) : uint32_t(k * s._blockBytes);
                }

                // Whether descriptor @p k interrupts: every one in block mode; in line mode one in
                // `_irqEvery`, and the frame's last two (a swap's two steps, a frame's start).
                BMDS_ISR_ATTR static bool interrupts(const Scanout &s, uint32_t k) {
                        return !s._lineMode || (k + 1) % s._irqEvery == 0 || k + 2 >= s._items;
                }

                BMDS_ISR_ATTR static void validate(const Scanout &s, uint32_t k) {
                        item(s, k)->ctrl_hi.val = interrupts(s, k) ? s._hw->ctrlInterrupt : s._hw->ctrlQuiet;
                        return;
                }

                // Descriptors [from, to) read @p buffer. In line mode the row steps on without a
                // division a descriptor (row = k × rows / items, carried as a remainder).
                BMDS_ISR_ATTR static void pointAt(Scanout &s, const uint8_t *buffer, uint32_t from, uint32_t to) {
                        if (from >= to) return;
                        if (!s._lineMode) {
                                for (uint32_t k = from; k < to; ++k)
                                        dw_gdma_ll_lli_set_src_addr(
                                                item(s, k), reinterpret_cast<uint32_t>(buffer + k * s._blockBytes));
                                return;
                        }
                        uint32_t       row = from * s._rows / s._items;
                        uint32_t       rest = from * s._rows % s._items;
                        const uint8_t *at = buffer + row * s._stride;
                        for (uint32_t k = from; k < to; ++k) {
                                dw_gdma_ll_lli_set_src_addr(item(s, k), reinterpret_cast<uint32_t>(at));
                                rest += s._rows;
                                while (rest >= s._items) {
                                        rest -= s._items;
                                        at += s._stride;
                                }
                        }
                        return;
                }

                BMDS_ISR_ATTR static bool blockSent(dw_gdma_channel_handle_t, const dw_gdma_trans_done_event_data_t *,
                                                    void *arg) {
                        const uint32_t started = esp_cpu_get_cycle_count();
                        auto          &s = *static_cast<Scanout *>(arg);
                        // While the flash is written this runs with the cache off (an IRAM-safe
                        // interrupt): only internal RAM may be touched, so the surface's waiters
                        // are woken at the next interrupt with the cache on.
                        const bool notify = spi_flash_cache_enabled();
                        if (notify && s._notifyPending) {
                                s._notifyPending = false;
                                s._surface->notifyFromIsr();
                        }
                        Scanout::Hardware &hw = *s._hw;
                        // Where the DMA is: a late interrupt can stand for several blocks
                        // (IDF's handler reads the status once).
                        const uint32_t at = hw.list.indexOf(
                                static_cast<uint32_t>(
                                        dw_gdma_ll_channel_get_current_link_list_item_addr(&DW_GDMA, hw.channel)) &
                                0x0fffffffu);
                        const uint32_t blocks = s._items;
                        if (at >= blocks) return false;
                        const bool wrapped = s._primed && at < s._lastItem;
                        s._sent.fetch_add(s._primed ? (at + blocks - s._lastItem) % blocks : at,
                                          std::memory_order_relaxed);
                        // The descriptors finished since the last interrupt valid again, from the
                        // one before it (a finished one's write-back, clearing it, can land after
                        // its interrupt: the next interrupt mends it, a frame before it is needed);
                        // all but the one under way the first time, or after a long gap.
                        const uint32_t since = (at + blocks - s._lastItem) % blocks;
                        if (!s._primed || since + 2 >= blocks) {
                                for (uint32_t k = 0; k < blocks; ++k)
                                        if (k != at) validate(s, k);
                        } else {
                                for (uint32_t k = (s._lastItem + blocks - 1) % blocks; k != at;
                                     k = k + 1 == blocks ? 0 : k + 1)
                                        validate(s, k);
                        }
                        Surface &surface = *s._surface;
                        if (s._swapPhase == 1 && at != blocks - 1) {
                                // The new frame has begun from the new buffer: its last block too.
                                pointAt(s, surface.askedFromIsr(), blocks - 1, blocks);
                                s._swapPhase = 0;
                                surface.swappedFromIsr(notify);
                                s._notifyPending = s._notifyPending || !notify;
                                s._swaps.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (s._swapPhase == 0 && at == blocks - 1 && surface.isSwapPending()) {
                                // The frame's last block under way: the next frame from the new buffer.
                                pointAt(s, surface.askedFromIsr(), 0, blocks - 1);
                                s._swapPhase = 1;
                        }
                        if (wrapped) {
                                s._frames.fetch_add(1, std::memory_order_relaxed);
                                surface.frameFromIsr(notify);
                                s._notifyPending = s._notifyPending || !notify;
                        }
                        s._lastItem = at;
                        s._primed = true;
                        const uint32_t cycles = esp_cpu_get_cycle_count() - started;
                        if (cycles > s._isrCyclesMax) s._isrCyclesMax = cycles;
                        s._isrCycles += cycles;
                        return false;
                }

                BMDS_ISR_ATTR static bool invalidBlock(dw_gdma_channel_handle_t, const dw_gdma_break_event_data_t *,
                                                       void *arg) {
                        auto &s = *static_cast<Scanout *>(arg);
                        s._stalls.fetch_add(1, std::memory_order_relaxed);
                        s._stalled.store(true, std::memory_order_release);
                        s._wake.releaseFromIsr();
                        return false;
                }

                BMDS_ISR_ATTR static void bridge(void *arg) {
                        auto          *s = static_cast<Scanout *>(arg);
                        dsi_brg_dev_t *dev = s->_hw->hal.bridge;
                        const uint32_t status = mipi_dsi_brg_ll_get_interrupt_status(dev);
                        mipi_dsi_brg_ll_clear_interrupt_status(dev, status);
                        if (status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) {
                                // Lines were dropped: what follows lands lower on the screen
                                // until the stream restarts at a frame's top.
                                s->_underruns.fetch_add(1, std::memory_order_relaxed);
                                s->_underran.store(true, std::memory_order_release);
                                s->_wake.releaseFromIsr();
                        }
                        return;
                }
};

Error Scanout::start(const Config &config, Surface &surface) {
        if (_running) return Error::InvalidState;
        const VideoTiming &t = config.timing;
        if (config.lanes < 1 || config.lanes > MIPI_DSI_LL_MAX_DATA_LANES || t.pixelKhz < 1000) return Error::Invalid;
        _config = config;
        const Error laid = layOut(surface);
        if (laid.isError()) return laid;
        _surface = &surface;
        _sent.store(0);
        _frames.store(1); // the first begins as the DMA starts
        _swaps.store(0);
        _stalls.store(0);
        _underruns.store(0);
        _stalled.store(false);
        _underran.store(false);
        _stopping.store(false);
        _primed = false;
        _swapPhase = 0;
        _notifyPending = false;
        _isrCyclesMax = 0;
        _isrCycles = 0;
        _resyncs = 0;
        _rebuilds = 0;
        _failures = 0;
        _blocksBefore = 0;
        _fallback = nullptr;
        _fallbacks = 0;
        _request.store(nullptr);
        while (_wake.tryAcquire().isOk()) {}
        void *hw = Memory::alloc(MemSpace::Internal, sizeof(Hardware));
        if (hw == nullptr) return Error::NoMem;
        _hw = new (hw) Hardware();
        _running = true;
        surface.setScanning(true);
        Error err = startLink();
        if (err.isOk()) err = startDpi();
        if (err.isOk()) {
                Task::Config tc;
                tc.name = "scanout";
                tc.stackBytes = 3072;
                tc.priority = configMAX_PRIORITIES - 2;
                tc.core = config.core;
                err = _task.start(tc, supervisorTask, this);
        }
        if (err.isError()) stop();
        return err;
}

void Scanout::stop() {
        if (!_running) return;
        _stopping.store(true);
        _wake.release();
        (void)_task.stop();
        stopHardware();
        _hw->~Hardware();
        Memory::free(_hw);
        _hw = nullptr;
        _surface->setScanning(false); // a pending swap made now
        _surface = nullptr;
        _running = false;
        return;
}

// The descriptors' plan for @p surface: as many blocks (up to MaxBlocks) as divide the frame's
// lines when it is the mode's height; shorter, a descriptor per line, its rows repeated, each
// interrupting (setLineInterrupts() to try fewer).
Error Scanout::planFor(const Surface &surface, Plan &plan) const {
        const VideoTiming &t = _config.timing;
        if (!surface.isStarted() || surface.width() != t.hActive || surface.height() == 0 ||
            surface.height() > t.vActive)
                return Error::Invalid;
        Plan p;
        p.stride = surface.stride();
        p.rows = surface.height();
        if (p.rows == t.vActive) {
                uint32_t blocks = MaxBlocks;
                while (t.vActive % blocks != 0) --blocks;
                if (blocks < 2) return Error::Invalid;
                p.items = blocks;
                p.lineMode = false;
                p.irqEvery = 1;
                p.blockBytes = p.stride * (t.vActive / blocks);
        } else {
                p.items = t.vActive;
                p.lineMode = true;
                // Every line interrupts (about 4% of a core): with one in 45, swaps stopped (the frame
                // went on at 60 a second, the swap count didn't move: no interrupt saw the DMA on the
                // last line, a swap's start), measured 2026-10-14; why is open (devplan/games.md §14).
                p.irqEvery = _lineInterrupts != 0 ? _lineInterrupts : 1;
                p.blockBytes = p.stride;
        }
        if (p.blockBytes % 8 != 0) return Error::Invalid; // the DMA moves 64-bit words
        plan = p;
        return Error::Ok;
}

void Scanout::apply(const Plan &plan) {
        _stride = plan.stride;
        _rows = plan.rows;
        _items = plan.items;
        _irqEvery = plan.irqEvery;
        _lineMode = plan.lineMode;
        _blockBytes = plan.blockBytes;
        return;
}

Error Scanout::layOut(const Surface &surface) {
        Plan        plan;
        const Error err = planFor(surface, plan);
        if (err.isOk()) apply(plan);
        return err;
}

// The channel pointed at the list's first descriptor (IDF's dw_gdma_channel_use_link_list()).
void Scanout::useList() {
        dw_gdma_ll_channel_set_link_list_master_port(&DW_GDMA, _hw->channel, DW_GDMA_LL_MASTER_PORT_MEMORY);
        dw_gdma_ll_channel_set_link_list_head_addr(&DW_GDMA, _hw->channel,
                                                   reinterpret_cast<uint32_t>(_hw->list.chunk[0]));
        return;
}

// The DMA's circular list for the plan (layOut()), reading the surface's shown buffer; the list
// before it, if any, freed once the new one is made.
Error Scanout::buildList() {
        Hardware &hw = *_hw;
        ItemList  fresh;
        if (_prepared != nullptr) {
                // Made by switchSurface() before the link came down.
                fresh = *_prepared;
                Memory::free(_prepared);
                _prepared = nullptr;
                if (fresh.count != _items) {
                        fresh.release();
                        return Error::Invalid;
                }
        } else {
                const Error err = fresh.allocate(_items);
                if (err.isError()) return err;
        }
        hw.list.release();
        hw.list = fresh;
        dw_gdma_block_transfer_config_t block = {};
        block.src.burst_mode = DW_GDMA_BURST_MODE_INCREMENT;
        block.src.burst_items = DW_GDMA_BURST_ITEMS_512;
        block.src.burst_len = 16;
        block.src.width = DW_GDMA_TRANS_WIDTH_64;
        block.dst.addr = MIPI_DSI_BRG_MEM_BASE;
        block.dst.burst_mode = DW_GDMA_BURST_MODE_FIXED;
        block.dst.burst_items = DW_GDMA_BURST_ITEMS_256;
        block.dst.burst_len = 16;
        block.dst.width = DW_GDMA_TRANS_WIDTH_64;
        block.size = _blockBytes / 8;
        const uint8_t *shown = _surface->shown();
        for (uint32_t i = 0; i < _items; ++i) {
                block.src.addr = reinterpret_cast<uint32_t>(shown + ScanoutIsr::offset(*this, i));
                dw_gdma_lli_handle_t lli = hw.list.item(i);
                (void)dw_gdma_lli_config_transfer(lli, &block);
                dw_gdma_block_markers_t markers = {};
                markers.is_valid = true;
                markers.en_trans_done_intr = ScanoutIsr::interrupts(*this, i);
                (void)dw_gdma_lli_set_block_markers(lli, markers);
        }
        // The control word as configured, with and without the interrupt (validate()).
        dw_gdma_link_list_item_t word = *hw.list.item(0);
        dw_gdma_ll_lli_set_block_markers(&word, true, false, true); // interrupt, last, valid
        hw.ctrlInterrupt = word.ctrl_hi.val;
        dw_gdma_ll_lli_set_block_markers(&word, false, false, true);
        hw.ctrlQuiet = word.ctrl_hi.val;
        _swapPhase = 0;
        _notifyPending = false;
        return Error::Ok;
}

Error Scanout::show(Surface &surface, unsigned int timeoutMs) {
        if (!_running) return Error::InvalidState;
        if (&surface == _surface) return Error::Ok;
        const VideoTiming &t = _config.timing;
        if (!surface.isStarted() || surface.width() != t.hActive || surface.height() == 0 ||
            surface.height() > t.vActive)
                return Error::Invalid;
        while (_switched.tryAcquire().isOk()) {}
        _request.store(&surface, std::memory_order_release);
        _wake.release();
        const Error err = _switched.acquire(timeoutMs);
        return err.isOk() ? _switchResult : err;
}

// The DSI host and PHY: clocks, the PHY PLL at the lane rate, the host's
// command-mode defaults (IDF's esp_lcd_new_dsi_bus).
Error Scanout::startLink() {
        Hardware          &hw = *_hw;
        const VideoTiming &t = _config.timing;
        const uint32_t     laneMbps = (t.pixelKhz * 24 / _config.lanes + 999) / 1000; // RGB888 on the link
        if (laneMbps < MIPI_DSI_LL_MIN_PHY_MBPS || laneMbps > MIPI_DSI_LL_MAX_PHY_MBPS) return Error::Invalid;
        if (_config.phyLdo >= 0) {
                esp_ldo_channel_config_t ldo = {};
                ldo.chan_id = _config.phyLdo;
                ldo.voltage_mv = _config.phyLdoMv;
                const Error err = espError(esp_ldo_acquire_channel(&ldo, &hw.ldo));
                if (err.isError()) return err;
        }
        PERIPH_RCC_ATOMIC() {
                mipi_dsi_ll_enable_bus_clock(_bus, true);
                mipi_dsi_ll_reset_register(_bus);
        }
        hw.busClock = true;
#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) < 300
        const mipi_dsi_phy_pllref_clock_source_t pllRef = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY;
#else
        const mipi_dsi_phy_pllref_clock_source_t pllRef = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT;
#endif
        Error err = espError(esp_clk_tree_enable_src(static_cast<soc_module_clk_t>(pllRef), true));
        if (err.isOk())
                err = espError(
                        esp_clk_tree_enable_src(static_cast<soc_module_clk_t>(MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT), true));
        if (err.isError()) return err;
        PERIPH_RCC_ATOMIC() {
                mipi_dsi_ll_set_phy_config_clock_source(_bus, MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT);
                mipi_dsi_ll_enable_phy_config_clock(_bus, true);
                mipi_dsi_ll_set_phy_pllref_clock_source(_bus, pllRef);
                mipi_dsi_ll_set_phy_pll_ref_clock_div(_bus, 1);
                mipi_dsi_ll_enable_phy_pllref_clock(_bus, true);
        }
        mipi_dsi_hal_config_t hal = {};
        hal.bus_id = _bus;
        hal.lane_bit_rate_mbps = laneMbps;
        hw.laneMbps = laneMbps;
        hal.num_data_lanes = _config.lanes;
        mipi_dsi_hal_init(&hw.hal, &hal);
        uint32_t refHz = 0;
        err = espError(esp_clk_tree_src_get_freq_hz(static_cast<soc_module_clk_t>(pllRef),
                                                    ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &refHz));
        if (err.isError()) return err;
        mipi_dsi_hal_configure_phy_pll(&hw.hal, refHz, laneMbps);
        for (int i = 0; !mipi_dsi_phy_ll_is_pll_locked(hw.hal.host); ++i) {
                if (i == 100) return Error::Timeout;
                Task::sleep(1);
        }
        for (int i = 0; !mipi_dsi_phy_ll_are_lanes_stopped(hw.hal.host, _config.lanes); ++i) {
                if (i == 100) return Error::Timeout;
                Task::sleep(1);
        }
        dsi_host_dev_t *host = hw.hal.host;
        mipi_dsi_host_ll_enable_video_mode(host, false);
        mipi_dsi_host_ll_set_clock_lane_state(host, MIPI_DSI_LL_CLOCK_LANE_STATE_LP);
        mipi_dsi_phy_ll_set_switch_time(host, 50, 104, 46, 128);
        mipi_dsi_host_ll_enable_rx_crc(host, true);
        mipi_dsi_host_ll_enable_rx_ecc(host, true);
        mipi_dsi_host_ll_enable_tx_eotp(host, true, false);
        mipi_dsi_host_ll_set_timeout_clock_division(host, laneMbps / 8 / TimeoutClockMhz);
        mipi_dsi_host_ll_set_escape_clock_division(host, laneMbps / 8 / EscapeClockMhz);
        mipi_dsi_host_ll_set_timeout_count(host, 0, 0, 0, 0, 0, 0, 0);
        mipi_dsi_phy_ll_set_max_read_time(host, 6000);
        mipi_dsi_phy_ll_set_stop_wait_time(host, 0x3f);
        return Error::Ok;
}

// The DPI side: its clock, the bridge's interrupt, the DMA's circular list
// over the surface's shown buffer, the host's video timing, the bridge's
// formats and flow control, then video on (IDF's esp_lcd_new_panel_dpi and
// its init, with the frame in blocks).
Error Scanout::startDpi() {
        Hardware          &hw = *_hw;
        const VideoTiming &t = _config.timing;

        // The pixel clock: a fixed PLL divided (VideoModes::fitToClock picks a
        // mode's clock among these). The APLL, which IDF lists as a DPI source,
        // runs at the asked frequency but the bridge never takes pixels from it
        // on chip revision 1.x (devplan/display.md).
        mipi_dsi_dpi_clock_source_t dpiSource = MIPI_DSI_DPI_CLK_SRC_PLL_F240M;
        uint32_t                    divider = 0;
        if (240'000 % t.pixelKhz == 0) {
                divider = 240'000 / t.pixelKhz;
        } else if (160'000 % t.pixelKhz == 0) {
                dpiSource = MIPI_DSI_DPI_CLK_SRC_PLL_F160M;
                divider = 160'000 / t.pixelKhz;
        } else {
                return Error::NotSupported;
        }
        hw.pixelHz = t.pixelKhz * 1000;
        Error err = espError(esp_clk_tree_enable_src(static_cast<soc_module_clk_t>(dpiSource), true));
        if (err.isError()) return err;
        PERIPH_RCC_ATOMIC() {
                mipi_dsi_ll_set_dpi_clock_source(_bus, dpiSource);
                mipi_dsi_ll_set_dpi_clock_div(_bus, divider);
                mipi_dsi_ll_enable_dpi_clock(_bus, true);
        }
        hw.dpiClock = true;

        err = espError(esp_intr_alloc(soc_mipi_dsi_signals[_bus].brg_irq_id, ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM,
                                      ScanoutIsr::bridge, this, &hw.bridgeIntr));
        if (err.isError()) return err;

        err = holdDmaGroup();
        if (err.isError()) return err;
        dw_gdma_channel_alloc_config_t dma = {};
        dma.src.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_LIST;
        dma.src.role = DW_GDMA_ROLE_MEM;
        dma.src.handshake_type = DW_GDMA_HANDSHAKE_HW;
        dma.src.num_outstanding_requests = 5;
        dma.dst.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_LIST;
        dma.dst.role = DW_GDMA_ROLE_PERIPH_DSI;
        dma.dst.handshake_type = DW_GDMA_HANDSHAKE_HW;
        dma.dst.num_outstanding_requests = 2;
        dma.flow_controller = DW_GDMA_FLOW_CTRL_SELF;
        dma.chan_priority = 1;
        err = espError(dw_gdma_new_channel(&dma, &hw.dma));
        if (err.isError()) return err;
        int channel = 0;
        (void)dw_gdma_channel_get_id(hw.dma, &channel);
        hw.channel = static_cast<uint8_t>(channel);
        err = buildList();
        if (err.isError()) return err;
        dw_gdma_event_callbacks_t callbacks = {};
        callbacks.on_block_trans_done = ScanoutIsr::blockSent;
        callbacks.on_invalid_block = ScanoutIsr::invalidBlock;
        err = espError(dw_gdma_channel_register_event_callbacks(hw.dma, &callbacks, this));
        if (err.isError()) return err;
        dsi_host_dev_t *host = hw.hal.host;
        mipi_dsi_host_ll_dpi_set_vcid(host, 0);
        mipi_dsi_host_ll_dpi_set_color_coding(host, LCD_COLOR_FMT_RGB888, 0);
        mipi_dsi_host_ll_dpi_set_timing_polarity(host, false, false, false, false, false);
        // High speed all the time: no low-power transitions in the blanking.
        mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(host, false, false);
        mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(host, false, false, false, false);
        mipi_dsi_host_ll_dpi_enable_lp_command(host, false);
        mipi_dsi_host_ll_dpi_enable_frame_ack(host, true);
        mipi_dsi_host_ll_dpi_set_video_burst_type(host, MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES);
        mipi_dsi_host_ll_dpi_set_video_packet_pixel_num(host, t.hActive);
        mipi_dsi_host_ll_dpi_set_trunks_num(host, 0);
        mipi_dsi_host_ll_dpi_set_null_packet_size(host, 0);
        // The host counts lane byte clocks, the bridge pixels: IDF's helper takes
        // the pixel clock in whole MHz, so the ratio is worked out here exactly.
        const double perPixel = double(hw.laneMbps) * 1e6 / (double(hw.pixelHz) * 8.0);
        auto         bytes = [&](uint32_t pixels) {
                return static_cast<uint32_t>(pixels * perPixel + 0.5);
        };
        mipi_dsi_host_ll_dpi_set_horizontal_timing(host, bytes(t.hSync), bytes(t.hBack), bytes(t.hActive),
                                                   bytes(t.hFront));
        mipi_dsi_host_ll_dpi_set_vertical_timing(host, t.vSync, t.vBack, t.vActive, t.vFront);

        dsi_brg_dev_t *bridge = hw.hal.bridge;
        configureBridge();

        // The DMA reads the surface ahead of everyone on the interconnect, or other
        // masters' PSRAM traffic (the caches) starves it and the bridge runs dry.
        axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, 15);
        axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, 15);
        // Drawing's 2D-DMA below it: copies between PSRAM buffers mustn't hold
        // up the scan.
        axi_icm_ll_set_dma2d_qos_arbiter_prio(4, 4);
        // ...and its writes regulated (a full-screen fill wrote PSRAM at about 470 MB/s
        // and underran the scan): one burst per 2^(Level + 1) interconnect cycles.
        axi_icm_ll_set_qos_burstiness(AXI_ICM_MASTER_DMA2D, 4, AXI_ICM_ACCESS_WRITE);
        axi_icm_ll_set_qos_peak_transaction_rate(AXI_ICM_MASTER_DMA2D, 2, Dma2dWriteLevel, AXI_ICM_ACCESS_WRITE);

        useList();
        err = espError(dw_gdma_channel_enable_ctrl(hw.dma, true));
        if (err.isError()) return err;
        mipi_dsi_host_ll_enable_video_mode(host, true);
        mipi_dsi_host_ll_set_clock_lane_state(host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
        mipi_dsi_brg_ll_enable_dpi_output(bridge, true);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        return Error::Ok;
}

// The DSI bridge's registers (after its reset too): the DPI timing it
// generates, its formats, the DMA flow, the underrun interrupt; then on.
void Scanout::configureBridge() {
        const VideoTiming &t = _config.timing;
        dsi_brg_dev_t     *bridge = _hw->hal.bridge;
        mipi_dsi_brg_ll_set_horizontal_timing(bridge, t.hSync, t.hBack, t.hActive, t.hFront);
        mipi_dsi_brg_ll_set_vertical_timing(bridge, t.vSync, t.vBack, t.vActive, t.vFront);
        mipi_dsi_brg_ll_set_num_pixel_bits(bridge, uint32_t(t.hActive) * t.vActive * Surface::BytesPerPixel * 8);
        mipi_dsi_brg_ll_set_underrun_discard_count(bridge, t.hActive);
        mipi_dsi_brg_ll_set_input_color_format(bridge, LCD_COLOR_FMT_RGB888);
        mipi_dsi_brg_ll_set_output_color_format(bridge, LCD_COLOR_FMT_RGB888, 0);
        mipi_dsi_brg_ll_set_flow_controller(bridge, MIPI_DSI_LL_FLOW_CONTROLLER_DMA);
        mipi_dsi_brg_ll_set_multi_block_number(bridge, 1);
        mipi_dsi_brg_ll_set_burst_len(bridge, 256);
        mipi_dsi_brg_ll_set_empty_threshold(bridge, 1024 - 256);
        mipi_dsi_brg_ll_enable_interrupt(bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, true);
        mipi_dsi_brg_ll_enable(bridge, true);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        return;
}

// The DSI link, the DPI side and the DMA off and released; the task stays.
void Scanout::stopHardware() {
        Hardware &hw = *_hw;
        if (hw.dpiClock) {
                mipi_dsi_brg_ll_enable_dpi_output(hw.hal.bridge, false);
                mipi_dsi_brg_ll_update_dpi_config(hw.hal.bridge);
                mipi_dsi_host_ll_enable_video_mode(hw.hal.host, false);
                mipi_dsi_brg_ll_enable(hw.hal.bridge, false);
                PERIPH_RCC_ATOMIC() {
                        mipi_dsi_ll_enable_dpi_clock(_bus, false);
                }
        }
        if (hw.dma != nullptr) {
                int id = 0;
                (void)dw_gdma_channel_get_id(hw.dma, &id);
                dw_gdma_ll_channel_abort(&DW_GDMA, static_cast<uint8_t>(id));
                for (int i = 0; i < 100 && (DW_GDMA.chen0.val & (1u << id)) != 0; ++i) esp_rom_delay_us(100);
                // Nothing of this channel's left to interrupt with.
                dw_gdma_ll_channel_enable_intr_propagation(&DW_GDMA, static_cast<uint8_t>(id), UINT32_MAX, false);
                dw_gdma_ll_channel_clear_intr(&DW_GDMA, static_cast<uint8_t>(id), UINT32_MAX);
                (void)dw_gdma_del_channel(hw.dma);
        }
        hw.list.release();
        if (hw.bridgeIntr != nullptr) (void)esp_intr_free(hw.bridgeIntr);
        if (hw.busClock) {
                mipi_dsi_hal_deinit(&hw.hal);
                PERIPH_RCC_ATOMIC() {
                        mipi_dsi_ll_enable_phy_pllref_clock(_bus, false);
                        mipi_dsi_ll_enable_phy_config_clock(_bus, false);
                        mipi_dsi_ll_enable_bus_clock(_bus, false);
                }
        }
        if (hw.ldo != nullptr) (void)esp_ldo_release_channel(hw.ldo);
        hw = Hardware();
        return;
}

void Scanout::supervisorTask(void *arg) {
        static_cast<Scanout *>(arg)->supervise();
        for (;;) Task::sleep(WaitForever); // stop() deletes the task
}

// Restarts the stream when the DMA meets an invalid descriptor or the bridge
// runs dry (the picture would stay offset: the bridge drops what it missed),
// or the whole path when the stream stops moving.
void Scanout::supervise() {
        uint32_t last = 0;
        int64_t  progressUs = esp_timer_get_time();
        int64_t  resyncUs = 0;
        while (!_stopping.load()) {
                (void)_wake.acquire(CheckMs);
                if (_stopping.load()) break;
                if (_request.load(std::memory_order_acquire) != nullptr) {
                        switchSurface();
                        _underran.store(false, std::memory_order_relaxed);
                        resyncUs = esp_timer_get_time();
                        last = 0;
                        progressUs = esp_timer_get_time();
                        continue;
                }
                const bool underran =
                        _underran.load(std::memory_order_acquire) && esp_timer_get_time() - resyncUs >= ResyncGapUs;
                if (_fallback != nullptr) {
                        // The new surface on probation: stalls in its first moments send the
                        // stream back to the surface before it.
                        const uint32_t stalls = _stalls.load(std::memory_order_relaxed) - _fallbackStalls;
                        if (stalls >= FallbackStalls) {
                                fallBack();
                                _stalled.store(false, std::memory_order_relaxed);
                                _underran.store(false, std::memory_order_relaxed);
                                resyncUs = esp_timer_get_time();
                                last = 0;
                                progressUs = esp_timer_get_time();
                                continue;
                        }
                        if (esp_timer_get_time() - _fallbackUs >= FallbackProbationUs) _fallback = nullptr;
                }
                if (_stalled.exchange(false, std::memory_order_acquire) || underran) {
                        resync();
                        // What the restart itself raised (the output off and on) is no news.
                        _underran.store(false, std::memory_order_relaxed);
                        resyncUs = esp_timer_get_time();
                        last = 0;
                        progressUs = esp_timer_get_time();
                        continue;
                }
                const uint32_t sent = _sent.load(std::memory_order_relaxed);
                const int64_t  now = esp_timer_get_time();
                if (sent != last) {
                        last = sent;
                        progressUs = now;
                } else if (now - progressUs >= StuckUs) {
                        (void)rebuild();
                        last = 0;
                        progressUs = esp_timer_get_time();
                }
        }
        return;
}

// The stream restarted from the frame's first block: the DMA stopped, every
// descriptor valid and on the shown buffer (a swap half made waits for the
// next frame), the bridge's frame count started again.
void Scanout::resync() {
        Hardware      &hw = *_hw;
        dsi_brg_dev_t *bridge = hw.hal.bridge;
        mipi_dsi_brg_ll_enable_dpi_output(bridge, false);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        // Stop the DMA before touching its registers (a write to a live
        // channel's is a bus fault). A disable waits for the burst under way,
        // which a wedged bridge never takes: then rebuild everything.
        (void)dw_gdma_channel_enable_ctrl(hw.dma, false);
        bool stopped = false;
        for (int i = 0; i < 20 && !stopped; ++i) {
                stopped = (DW_GDMA.chen0.val & (1u << hw.channel)) == 0;
                if (!stopped) esp_rom_delay_us(100);
        }
        if (!stopped) {
                (void)rebuild();
                return;
        }
        dw_gdma_ll_channel_clear_intr(&DW_GDMA, hw.channel, UINT32_MAX);
        Task::sleep(1); // a block interrupt already raised lands before the counters restart
        _blocksBefore += _sent.load(std::memory_order_relaxed);
        _sent.store(0, std::memory_order_relaxed);
        _primed = false;
        _swapPhase = 0;
        _notifyPending = false;
        ScanoutIsr::pointAt(*this, _surface->shown(), 0, _items);
        for (uint32_t k = 0; k < _items; ++k) ScanoutIsr::validate(*this, k);
        const VideoTiming &t = _config.timing;
        mipi_dsi_brg_ll_clear_interrupt_status(bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
        mipi_dsi_brg_ll_set_num_pixel_bits(bridge, uint32_t(t.hActive) * t.vActive * Surface::BytesPerPixel * 8);
        useList();
        (void)dw_gdma_channel_enable_ctrl(hw.dma, true);
        mipi_dsi_brg_ll_enable_dpi_output(bridge, true);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        ++_resyncs;
        return;
}

// show()'s request: the whole DSI path rebuilt for the new surface (a resync alone left the
// picture out of step with the frame once), the old surface back if that fails.
void Scanout::switchSurface() {
        Surface *next = _request.exchange(nullptr, std::memory_order_acq_rel);
        if (next == nullptr) return;
        Surface *old = _surface;
        // The new plan and surface are taken by rebuild() once the link is down: the interrupt reads
        // them, and the old list runs until then (a plan changed earlier walked it with the new
        // one's count: a store into a chunk it doesn't have).
        Plan  plan;
        Error err = planFor(*next, plan);
        if (err.isOk()) {
                // The new descriptors made before the link comes down: without memory for them,
                // nothing changes on the screen.
                void *memory = Memory::alloc(MemSpace::Internal, sizeof(ItemList));
                auto *list = memory != nullptr ? ::new (memory) ItemList() : nullptr;
                err = list != nullptr ? list->allocate(plan.items) : Error(Error::NoMem);
                if (err.isError()) {
                        if (list != nullptr) list->~ItemList();
                        Memory::free(memory);
                        _switchResult = err;
                        _switched.release();
                        return;
                }
                _prepared = list;
                _nextSurface = next;
                _nextPlan = plan;
                err = rebuild();
                if (err.isError()) {
                        if (_prepared != nullptr) { // not taken: the link failed before the list
                                _prepared->release();
                                Memory::free(_prepared);
                                _prepared = nullptr;
                        }
                        if (planFor(*old, _nextPlan).isOk()) {
                                _nextSurface = old;
                                (void)rebuild();
                        }
                }
        }
        if (err.isOk()) {
                old->setScanning(false); // its pending swap made now
                next->setScanning(true);
                // Kept until the new surface has run cleanly for a while (supervise()).
                _fallback = old;
                _fallbackStalls = _stalls.load(std::memory_order_relaxed);
                _fallbackUs = esp_timer_get_time();
        }
        _switchResult = err;
        _switched.release();
        return;
}

// A surface switched to that keeps stalling the stream: back to the one before.
void Scanout::fallBack() {
        Surface *next = _fallback;
        _fallback = nullptr;
        if (next == nullptr || next == _surface) return;
        Surface *bad = _surface;
        if (planFor(*next, _nextPlan).isError()) return;
        _nextSurface = next; // taken with its plan once the link is down
        (void)rebuild();
        bad->setScanning(false);
        next->setScanning(true);
        ++_fallbacks;
        return;
}

// The last resort: the whole DSI path rebuilt from scratch, as a fresh
// start does (the monitor sees the link drop for a moment), then the
// bridge chip told to lock onto it again (`Config::onRestart`).
Error Scanout::rebuild() {
        stopHardware();
        // No link, no interrupt: a surface switched to, and its plan, taken now.
        if (_nextSurface != nullptr) {
                _surface = _nextSurface;
                apply(_nextPlan);
                _nextSurface = nullptr;
        }
        _stalled.store(false, std::memory_order_relaxed);
        _underran.store(false, std::memory_order_relaxed);
        _blocksBefore += _sent.load(std::memory_order_relaxed);
        _sent.store(0, std::memory_order_relaxed);
        _primed = false;
        Error err = startLink();
        if (err.isOk()) err = startDpi();
        if (err.isOk() && _config.onRestart != nullptr) _config.onRestart(_config.onRestartArg);
        if (err.isError()) ++_failures;
        ++_rebuilds;
        return err;
}

Scanout::Stats Scanout::stats() const {
        Stats s;
        s.frames = _frames.load(std::memory_order_relaxed);
        s.blocks = _blocksBefore + _sent.load(std::memory_order_relaxed);
        s.blocksPerFrame = _items;
        s.swaps = _swaps.load(std::memory_order_relaxed);
        s.stalls = _stalls.load(std::memory_order_relaxed);
        s.underruns = _underruns.load(std::memory_order_relaxed);
        s.resyncs = _resyncs;
        s.rebuilds = _rebuilds;
        s.failures = _failures;
        s.isrCyclesMax = _isrCyclesMax;
        s.isrCycles = _isrCycles;
        s.dmaItem = _lastItem;
        s.fallbacks = _fallbacks;
        if (_hw != nullptr) {
                s.pixelHz = _hw->pixelHz;
                s.bridgeFifo = _hw->hal.bridge != nullptr ? mipi_dsi_brg_ll_get_fifo_depth(_hw->hal.bridge) : 0;
        }
        return s;
}

BMDS_NAMESPACE_END
