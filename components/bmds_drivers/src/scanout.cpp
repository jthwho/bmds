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
#include <esp_clk_tree.h>
#include <esp_cpu.h>
#include <esp_intr_alloc.h>
#include <esp_ldo_regulator.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <esp_private/dw_gdma.h>
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/periph_ctrl.h>
#include <freertos/FreeRTOS.h>
#include <hal/axi_icm_ll.h>
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

struct Scanout::Hardware {
                mipi_dsi_hal_context_t     hal = {};
                esp_ldo_channel_handle_t   ldo = nullptr;
                dw_gdma_channel_handle_t   dma = nullptr;
                dw_gdma_link_list_handle_t list = nullptr;
                dw_gdma_link_list_item_t  *items[Scanout::MaxBlocks] = {}; // the descriptors (uncached addresses)
                intr_handle_t              bridgeIntr = nullptr;
                bool                       busClock = false;
                bool                       dpiClock = false;
                uint32_t                   pixelHz = 0; // the pixel clock as set
                uint32_t                   laneMbps = 0;
                uint8_t                    channel = 0;    // the DMA channel's number
                uint32_t                   itemBase = 0;   // the first descriptor's address, as the DMA sees it
                uint32_t                   itemStride = 0; // bytes from one descriptor to the next
};

// The interrupts: a block sent, the DMA at an invalid descriptor, the DSI
// bridge running dry.
struct ScanoutIsr {
                // Descriptors [from, to) read @p buffer's blocks.
                BMDS_ISR_ATTR static void pointAt(Scanout &s, const uint8_t *buffer, uint32_t from, uint32_t to) {
                        for (uint32_t k = from; k < to; ++k)
                                dw_gdma_ll_lli_set_src_addr(s._hw->items[k],
                                                            reinterpret_cast<uint32_t>(buffer + k * s._blockBytes));
                        return;
                }

                BMDS_ISR_ATTR static bool blockSent(dw_gdma_channel_handle_t, const dw_gdma_trans_done_event_data_t *,
                                                    void *arg) {
                        const uint32_t     started = esp_cpu_get_cycle_count();
                        auto              &s = *static_cast<Scanout *>(arg);
                        Scanout::Hardware &hw = *s._hw;
                        // Where the DMA is: a late interrupt can stand for several blocks
                        // (IDF's handler reads the status once).
                        const uint32_t at = ((static_cast<uint32_t>(dw_gdma_ll_channel_get_current_link_list_item_addr(
                                                      &DW_GDMA, hw.channel)) &
                                              0x0fffffffu) -
                                             hw.itemBase) /
                                            hw.itemStride;
                        const uint32_t blocks = s._blocks;
                        if (at >= blocks) return false;
                        const bool wrapped = s._primed && at < s._lastItem;
                        s._sent.fetch_add(s._primed ? (at + blocks - s._lastItem) % blocks : at,
                                          std::memory_order_relaxed);
                        // Every descriptor but the one under way valid again (a finished
                        // one's write-back, clearing it, can land after its interrupt:
                        // the next interrupt mends it, a frame before it is needed).
                        for (uint32_t k = 0; k < blocks; ++k)
                                if (k != at) dw_gdma_ll_lli_set_block_markers(hw.items[k], true, false, true);
                        Surface &surface = *s._surface;
                        if (s._swapPhase == 1 && at != blocks - 1) {
                                // The new frame has begun from the new buffer: its last block too.
                                pointAt(s, surface.askedFromIsr(), blocks - 1, blocks);
                                s._swapPhase = 0;
                                surface.swappedFromIsr();
                                s._swaps.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (s._swapPhase == 0 && at == blocks - 1 && surface.isSwapPending()) {
                                // The frame's last block under way: the next frame from the new buffer.
                                pointAt(s, surface.askedFromIsr(), 0, blocks - 1);
                                s._swapPhase = 1;
                        }
                        if (wrapped) {
                                s._frames.fetch_add(1, std::memory_order_relaxed);
                                surface.frameFromIsr();
                        }
                        s._lastItem = at;
                        s._primed = true;
                        const uint32_t cycles = esp_cpu_get_cycle_count() - started;
                        if (cycles > s._isrCyclesMax) s._isrCyclesMax = cycles;
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
        if (!surface.isStarted() || surface.width() != t.hActive || surface.height() != t.vActive || config.lanes < 1 ||
            config.lanes > MIPI_DSI_LL_MAX_DATA_LANES || t.pixelKhz < 1000)
                return Error::Invalid;
        // The frame in as many blocks (up to MaxBlocks) as divide its lines.
        uint32_t blocks = MaxBlocks;
        while (t.vActive % blocks != 0) --blocks;
        if (blocks < 2) return Error::Invalid;
        _blockBytes = surface.stride() * (t.vActive / blocks);
        if (_blockBytes % 8 != 0) return Error::Invalid; // the DMA moves 64-bit words
        _blocks = blocks;
        _config = config;
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
        _isrCyclesMax = 0;
        _resyncs = 0;
        _rebuilds = 0;
        _failures = 0;
        _blocksBefore = 0;
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

        err = espError(esp_intr_alloc(soc_mipi_dsi_signals[_bus].brg_irq_id, ESP_INTR_FLAG_LOWMED, ScanoutIsr::bridge,
                                      this, &hw.bridgeIntr));
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
        dw_gdma_link_list_config_t list = {};
        list.num_items = _blocks;
        list.link_type = DW_GDMA_LINKED_LIST_TYPE_CIRCULAR;
        err = espError(dw_gdma_new_link_list(&list, &hw.list));
        if (err.isError()) return err;
        dw_gdma_event_callbacks_t callbacks = {};
        callbacks.on_block_trans_done = ScanoutIsr::blockSent;
        callbacks.on_invalid_block = ScanoutIsr::invalidBlock;
        err = espError(dw_gdma_channel_register_event_callbacks(hw.dma, &callbacks, this));
        if (err.isError()) return err;
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
        dw_gdma_block_markers_t markers = {};
        markers.is_valid = true;
        markers.en_trans_done_intr = true;
        const uint8_t *shown = _surface->shown();
        for (uint32_t i = 0; i < _blocks; ++i) {
                block.src.addr = reinterpret_cast<uint32_t>(shown + i * _blockBytes);
                dw_gdma_lli_handle_t lli = dw_gdma_link_list_get_item(hw.list, i);
                hw.items[i] = reinterpret_cast<dw_gdma_link_list_item_t *>(lli);
                (void)dw_gdma_lli_config_transfer(lli, &block);
                (void)dw_gdma_lli_set_block_markers(lli, markers);
        }
        _swapPhase = 0;
        hw.itemBase = reinterpret_cast<uint32_t>(hw.items[0]) & 0x0fffffffu;
        hw.itemStride = reinterpret_cast<uint32_t>(hw.items[1]) - reinterpret_cast<uint32_t>(hw.items[0]);

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

        err = espError(dw_gdma_channel_use_link_list(hw.dma, hw.list));
        if (err.isOk()) err = espError(dw_gdma_channel_enable_ctrl(hw.dma, true));
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
        if (hw.list != nullptr) (void)dw_gdma_del_link_list(hw.list);
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
                const bool underran =
                        _underran.load(std::memory_order_acquire) && esp_timer_get_time() - resyncUs >= ResyncGapUs;
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
                        rebuild();
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
                rebuild();
                return;
        }
        dw_gdma_ll_channel_clear_intr(&DW_GDMA, hw.channel, UINT32_MAX);
        Task::sleep(1); // a block interrupt already raised lands before the counters restart
        _blocksBefore += _sent.load(std::memory_order_relaxed);
        _sent.store(0, std::memory_order_relaxed);
        _primed = false;
        _swapPhase = 0;
        for (uint32_t k = 0; k < _blocks; ++k) {
                dw_gdma_ll_lli_set_src_addr(hw.items[k],
                                            reinterpret_cast<uint32_t>(_surface->shown() + k * _blockBytes));
                dw_gdma_ll_lli_set_block_markers(hw.items[k], true, false, true);
        }
        const VideoTiming &t = _config.timing;
        mipi_dsi_brg_ll_clear_interrupt_status(bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
        mipi_dsi_brg_ll_set_num_pixel_bits(bridge, uint32_t(t.hActive) * t.vActive * Surface::BytesPerPixel * 8);
        (void)dw_gdma_channel_use_link_list(hw.dma, hw.list);
        (void)dw_gdma_channel_enable_ctrl(hw.dma, true);
        mipi_dsi_brg_ll_enable_dpi_output(bridge, true);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        ++_resyncs;
        return;
}

// The last resort: the whole DSI path rebuilt from scratch, as a fresh
// start does (the monitor sees the link drop for a moment), then the
// bridge chip told to lock onto it again (`Config::onRestart`).
void Scanout::rebuild() {
        stopHardware();
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
        return;
}

Scanout::Stats Scanout::stats() const {
        Stats s;
        s.frames = _frames.load(std::memory_order_relaxed);
        s.blocks = _blocksBefore + _sent.load(std::memory_order_relaxed);
        s.blocksPerFrame = _blocks;
        s.swaps = _swaps.load(std::memory_order_relaxed);
        s.stalls = _stalls.load(std::memory_order_relaxed);
        s.underruns = _underruns.load(std::memory_order_relaxed);
        s.resyncs = _resyncs;
        s.rebuilds = _rebuilds;
        s.failures = _failures;
        s.isrCyclesMax = _isrCyclesMax;
        s.dmaItem = _lastItem;
        if (_hw != nullptr) {
                s.pixelHz = _hw->pixelHz;
                s.bridgeFifo = _hw->hal.bridge != nullptr ? mipi_dsi_brg_ll_get_fifo_depth(_hw->hal.bridge) : 0;
        }
        return s;
}

BMDS_NAMESPACE_END
