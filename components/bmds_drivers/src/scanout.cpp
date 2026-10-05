/**
 * @file      scanout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The line-rendered scanout (devplan/display.md §5). The link and DPI set-up
 * follows the register sequence of IDF's DSI bus and DPI panel
 * (esp_lcd/dsi), on the HAL; the DMA streams a circular list of line
 * buffers instead of one framebuffer.
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
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/periph_ctrl.h>
#include <freertos/FreeRTOS.h>
#include <hal/axi_icm_ll.h>
#include <hal/dw_gdma_ll.h>
#include <hal/mipi_dsi_hal.h>
#include <hal/mipi_dsi_ll.h>
#include <hal/mipi_dsi_periph.h>
#include <soc/reg_base.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/scanout.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t     TimeoutClockMhz = 10; // IDF's: the host's timeout clock
        constexpr uint32_t     EscapeClockMhz = 18;  // TX escape clock, 2-20 MHz
        constexpr size_t       BufferAlign = 64;     // cache line, and the DMA's 64-bit words
        constexpr unsigned int StallCheckMs = 100;
        // A resync at most this often: one may underrun as it starts, and a
        // monitor sees each as a glitch.
        constexpr int64_t ResyncGapUs = 50'000;
        // No block sent for this long while running is a failure too: a stuck
        // stream raises no further underrun or stall to act on.
        constexpr int64_t StuckUs = 50'000;

        Error espError(esp_err_t err) {
                return err == ESP_OK ? Error(Error::Ok) : Error::fromEspErr(err);
        }

        // The ring's bytes per pixel: Indexed8 and Rgb565Expanded are expanded
        // to RGB888 into it.
        size_t bytesPerPixel(Scanout::Format f) {
                return f == Scanout::Format::Rgb565 ? 2 : 3;
        }

        lcd_color_format_t colorFormat(Scanout::Format f) {
                return f == Scanout::Format::Rgb565 ? LCD_COLOR_FMT_RGB565 : LCD_COLOR_FMT_RGB888;
        }

        // The bytes per pixel the source draws.
        size_t drawnBytesPerPixel(Scanout::Format f) {
                return f == Scanout::Format::Indexed8 ? 1 : f == Scanout::Format::Rgb888 ? 3 : 2;
        }

        bool isExpanded(Scanout::Format f) {
                return f == Scanout::Format::Indexed8 || f == Scanout::Format::Rgb565Expanded;
        }

        constexpr unsigned int ExpandTimeoutMs = 20;

        // Free-running: the DMA's list goes round the ring's buffers several
        // times, so a block interrupt that re-validates a descriptor can be
        // that many blocks late (63 at 8 lines a block: 11 ms at 720p60)
        // before the DMA meets an invalid one.
        constexpr uint32_t MaxItems = 64;

} // namespace

struct Scanout::Hardware {
                mipi_dsi_hal_context_t     hal = {};
                esp_ldo_channel_handle_t   ldo = nullptr;
                dw_gdma_channel_handle_t   dma = nullptr;
                dw_gdma_link_list_handle_t list = nullptr;
                dw_gdma_link_list_item_t  *items[MaxItems] = {}; // the list's descriptors (uncached addresses)
                intr_handle_t              bridgeIntr = nullptr;
                bool                       busClock = false;
                bool                       dpiClock = false;
                bool                       apll = false; // the pixel clock from the APLL
                uint32_t                   pixelHz = 0;  // the pixel clock as set
                uint32_t                   laneMbps = 0;
                uint8_t                    channel = 0;    // the DMA channel's number
                uint32_t                   itemBase = 0;   // the first descriptor's address, as the DMA sees it
                uint32_t                   itemStride = 0; // bytes from one descriptor to the next
};

// The interrupts: a buffer sent, the DMA at a buffer not handed back, the
// DSI bridge running dry.
struct ScanoutIsr {
                BMDS_ISR_ATTR static bool blockSent(dw_gdma_channel_handle_t, const dw_gdma_trans_done_event_data_t *,
                                                    void *arg) {
                        auto *s = static_cast<Scanout *>(arg);
                        if (!s->_config.freeRunning) {
                                s->_sent.fetch_add(1, std::memory_order_relaxed);
                                s->_wake.releaseFromIsr();
                                return false;
                        }
                        // Free-running. A late interrupt can stand for several blocks (IDF's
                        // handler reads the status once), so the count comes from where the
                        // DMA is: the blocks since the last interrupt, from its descriptor.
                        Scanout::Hardware &hw = *s->_hw;
                        const uint32_t at = ((static_cast<uint32_t>(dw_gdma_ll_channel_get_current_link_list_item_addr(
                                                      &DW_GDMA, hw.channel)) &
                                              0x0fffffffu) -
                                             hw.itemBase) /
                                            hw.itemStride;
                        const uint32_t done = s->_sent.load(std::memory_order_relaxed);
                        uint32_t       now = done + 1;
                        if (s->_primed) now = done + (at + s->_items - s->_lastItem) % s->_items;
                        s->_primed = true;
                        s->_lastItem = at;
                        // Every descriptor finished since, valid again (and the one before the
                        // last: the DMA's write-back of a finished one, its valid bit cleared,
                        // can land after the interrupt under memory load).
                        for (uint32_t k = done == 0 ? 0 : done - 1; k < now; ++k)
                                dw_gdma_ll_lli_set_block_markers(hw.items[k % s->_items], true, false, true);
                        s->_sent.store(now, std::memory_order_relaxed);
                        s->_wake.releaseFromIsr();
                        return false; // releaseFromIsr() yields at the interrupt's exit
                }

                BMDS_ISR_ATTR static bool invalidBlock(dw_gdma_channel_handle_t, const dw_gdma_break_event_data_t *,
                                                       void *arg) {
                        auto          *s = static_cast<Scanout *>(arg);
                        const uint32_t n = s->_stalls.fetch_add(1, std::memory_order_relaxed);
                        s->_stallAt[n % 8] = s->_sent.load(std::memory_order_relaxed);
                        s->_resyncWanted.store(true, std::memory_order_release);
                        s->_stalled.store(true, std::memory_order_release);
                        s->_wake.releaseFromIsr();
                        return false;
                }

                BMDS_ISR_ATTR static void bridge(void *arg) {
                        auto          *s = static_cast<Scanout *>(arg);
                        dsi_brg_dev_t *dev = s->_hw->hal.bridge;
                        const uint32_t status = mipi_dsi_brg_ll_get_interrupt_status(dev);
                        mipi_dsi_brg_ll_clear_interrupt_status(dev, status);
                        if (status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN)
                                s->_underruns.fetch_add(1, std::memory_order_relaxed);
                        return;
                }
};

Error Scanout::start(const Config &config, LineSource &source) {
        if (_running) return Error::InvalidState;
        const VideoTiming &t = config.timing;
        if (config.blocks < 2 || config.blocks > 16 || config.linesPerBlock == 0 ||
            t.vActive % config.linesPerBlock != 0 || config.lanes < 1 || config.lanes > MIPI_DSI_LL_MAX_DATA_LANES ||
            t.pixelKhz < 1000)
                return Error::Invalid;
        if (config.frameSource != nullptr && (config.format != Format::Rgb565Expanded || config.framebuffer))
                return Error::Invalid;
        if (config.framebuffer &&
            (config.format != Format::Rgb888 || uint32_t(config.blocks) * config.linesPerBlock != t.vActive))
                return Error::Invalid;
        _config = config;
        _source = &source;
        _stride = t.hActive * bytesPerPixel(config.format);
        if (_stride % 8 != 0) return Error::Invalid; // the DMA moves 64-bit words
        _blockBytes = _stride * config.linesPerBlock;
        _blocksPerFrame = t.vActive / config.linesPerBlock;
        _sent.store(0);
        _primed = false;
        _refilled = 0;
        _late.store(0);
        _skipped = 0;
        _stalls.store(0);
        _underruns.store(0);
        _stalled.store(false);
        _resyncWanted.store(false);
        _resyncs = 0;
        _resyncFailures = 0;
        _rebuilds = 0;
        _blocksBefore = 0;
        _resyncUs = 0;
        _stopping.store(false);
        _renderCyclesMax = 0;
        _renderCycles = 0;
        _syncCycles = 0;
        _syncCyclesMax = 0;

        void *hw = Memory::alloc(MemSpace::Internal, sizeof(Hardware));
        _ring = static_cast<uint8_t *>(
                Memory::allocAligned(config.framebuffer ? MemSpace::PsramDma : MemSpace::InternalDma,
                                     _blockBytes * config.blocks, BufferAlign));
        if (hw == nullptr || _ring == nullptr) {
                if (hw != nullptr) Memory::free(hw);
                if (_ring != nullptr) Memory::free(_ring);
                _ring = nullptr;
                return Error::NoMem;
        }
        _hw = new (hw) Hardware();
        _expandCycles = 0;
        _expandCyclesMax = 0;
        _expandErrors = 0;
        if (isExpanded(config.format)) {
                const size_t drawn = size_t(t.hActive) * config.linesPerBlock * drawnBytesPerPixel(config.format);
                // From a framebuffer, the 2D-DMA reads it directly: no staging.
                if (config.frameSource == nullptr)
                        _staging =
                                static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, drawn, BufferAlign));
                Error err = Error::NoMem;
                if (_staging != nullptr || config.frameSource != nullptr)
                        err = config.format == Format::Indexed8 ? _expander.start() : _widener.start();
                if (err.isError()) {
                        _expander.stop();
                        _widener.stop();
                        Memory::free(_staging);
                        _staging = nullptr;
                        _hw->~Hardware();
                        Memory::free(_hw);
                        Memory::free(_ring);
                        _hw = nullptr;
                        _ring = nullptr;
                        return err;
                }
                if (config.format == Format::Indexed8 && config.palette != nullptr)
                        _expander.setPalette(config.palette, 0, PaletteExpander::PaletteSize);
                // Only the PPA or the 2D-DMA writes the ring: no stale lines of it may stay in the cache.
                (void)esp_cache_msync(_ring, _blockBytes * config.blocks, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }
        // The ring starts full: buffer i carries block i.
        for (uint32_t i = 0; i < config.blocks; ++i) renderSlot(i);
        _running = true;

        Error err = startLink();
        if (err.isOk()) err = startDpi();
        if (err.isOk()) {
                Task::Config tc;
                tc.name = "scanout";
                tc.stackBytes = 4096;
                tc.priority = configMAX_PRIORITIES - 2;
                tc.core = config.core;
                err = _task.start(tc, refillTask, this);
        }
        if (err.isError()) stop();
        return err;
}

// The DSI host and PHY: clocks, the PHY PLL at the lane rate, the host's
// command-mode defaults (IDF's esp_lcd_new_dsi_bus).
Error Scanout::startLink() {
        Hardware          &hw = *_hw;
        const VideoTiming &t = _config.timing;
#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) < 300
        const uint32_t linkBits = static_cast<uint32_t>(bytesPerPixel(_config.format) * 8); // the buffers' format
#else
        const uint32_t linkBits = 24;
#endif
        const uint32_t laneBits = _config.fullLaneRate ? 24 : linkBits;
        const uint32_t laneMbps = (t.pixelKhz * laneBits / _config.lanes + 999) / 1000;
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

// The DPI side: its clock, the bridge's interrupt, the DMA ring, the host's
// video timing, the bridge's formats and flow control, then video on (IDF's
// esp_lcd_new_panel_dpi and its init, with a ring of blocks).
Error Scanout::startDpi() {
        Hardware                &hw = *_hw;
        const VideoTiming       &t = _config.timing;
        const lcd_color_format_t in = colorFormat(_config.format);
#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) < 300
        // Before v3.0 the bridge has one format field for both sides: no
        // conversion, so the link carries the buffers' format.
        const lcd_color_format_t out = in;
#else
        const lcd_color_format_t out = LCD_COLOR_FMT_RGB888;
#endif

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
        _items = _config.freeRunning ? _config.blocks * (MaxItems / _config.blocks) : _config.blocks;
        list.num_items = _items;
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
        for (uint32_t i = 0; i < _items; ++i) {
                block.src.addr = reinterpret_cast<uint32_t>(_ring + (i % _config.blocks) * _blockBytes);
                dw_gdma_lli_handle_t lli = dw_gdma_link_list_get_item(hw.list, i);
                hw.items[i] = reinterpret_cast<dw_gdma_link_list_item_t *>(lli);
                (void)dw_gdma_lli_config_transfer(lli, &block);
                (void)dw_gdma_lli_set_block_markers(lli, markers);
        }
        hw.itemBase = reinterpret_cast<uint32_t>(hw.items[0]) & 0x0fffffffu;
        hw.itemStride = reinterpret_cast<uint32_t>(hw.items[1]) - reinterpret_cast<uint32_t>(hw.items[0]);

        dsi_host_dev_t *host = hw.hal.host;
        mipi_dsi_host_ll_dpi_set_vcid(host, 0);
        mipi_dsi_host_ll_dpi_set_color_coding(host, out, out == LCD_COLOR_FMT_RGB888 ? 0 : _config.linkPacking);
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

        // The DMA reads the ring ahead of everyone on the interconnect, or other
        // masters' PSRAM traffic (the caches) starves it and the bridge runs dry.
        axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, 15);
        axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, 15);

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
        const VideoTiming       &t = _config.timing;
        dsi_brg_dev_t           *bridge = _hw->hal.bridge;
        const lcd_color_format_t in = colorFormat(_config.format);
#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) < 300
        const lcd_color_format_t out = in;
#else
        const lcd_color_format_t out = LCD_COLOR_FMT_RGB888;
#endif
        mipi_dsi_brg_ll_set_horizontal_timing(bridge, t.hSync, t.hBack, t.hActive, t.hFront);
        mipi_dsi_brg_ll_set_vertical_timing(bridge, t.vSync, t.vBack, t.vActive, t.vFront);
        mipi_dsi_brg_ll_set_num_pixel_bits(bridge, uint32_t(t.hActive) * t.vActive * bytesPerPixel(_config.format) * 8);
        mipi_dsi_brg_ll_set_underrun_discard_count(bridge, t.hActive);
        mipi_dsi_brg_ll_set_input_color_format(bridge, in);
        mipi_dsi_brg_ll_set_output_color_format(bridge, out, out == LCD_COLOR_FMT_RGB888 ? 0 : _config.linkPacking);
        mipi_dsi_brg_ll_set_flow_controller(bridge, MIPI_DSI_LL_FLOW_CONTROLLER_DMA);
        mipi_dsi_brg_ll_set_multi_block_number(bridge, 1);
        mipi_dsi_brg_ll_set_burst_len(bridge, 256);
        mipi_dsi_brg_ll_set_empty_threshold(bridge, 1024 - 256);
        mipi_dsi_brg_ll_enable_interrupt(bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, true);
        mipi_dsi_brg_ll_enable(bridge, true);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        return;
}

void Scanout::stop() {
        if (!_running) return;
        _stopping.store(true);
        _wake.release();
        (void)_task.stop();
        stopHardware();
        _hw->~Hardware();
        Memory::free(_hw);
        Memory::free(_ring);
        _expander.stop();
        _widener.stop();
        Memory::free(_staging);
        _staging = nullptr;
        _hw = nullptr;
        _ring = nullptr;
        _running = false;
        return;
}

// The DSI link, the DPI side and the DMA off and released; the ring, the
// task and the palette stay.
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

// The last resort: the whole DSI path rebuilt from scratch, as a fresh
// start does (the monitor sees the link drop for a moment), then the
// bridge chip told to lock onto it again (`Config::onRestart`).
void Scanout::rebuild() {
        stopHardware();
        _resyncWanted.store(false, std::memory_order_relaxed);
        _stalled.store(false, std::memory_order_relaxed);
        _blocksBefore += _sent.load(std::memory_order_relaxed);
        _sent.store(0, std::memory_order_relaxed);
        _primed = false;
        _refilled = 0;
        while (_wake.tryAcquire().isOk()) {}
        if (!_config.framebuffer) {
                for (uint32_t i = 0; i < _config.blocks; ++i) renderSlot(i);
        }
        Error err = startLink();
        if (err.isOk()) err = startDpi();
        if (err.isOk() && _config.onRestart != nullptr) _config.onRestart(_config.onRestartArg);
        if (err.isError()) ++_resyncFailures;
        ++_rebuilds;
        _resyncUs = esp_timer_get_time();
        return;
}

Scanout::Stats Scanout::stats() const {
        Stats s;
        s.blocks = _blocksBefore + _sent.load(std::memory_order_relaxed);
        s.resyncs = _resyncs;
        s.resyncFailures = _resyncFailures;
        s.rebuilds = _rebuilds;
        if (_hw != nullptr) {
                s.pixelHz = _hw->pixelHz;
        }
        s.frames = _blocksPerFrame == 0 ? 0 : s.blocks / _blocksPerFrame;
        s.late = _late.load(std::memory_order_relaxed);
        s.skipped = _skipped;
        s.stalls = _stalls.load(std::memory_order_relaxed);
        s.underruns = _underruns.load(std::memory_order_relaxed);
        s.renderCyclesMax = _renderCyclesMax;
        s.renderCycles = _renderCycles;
        for (int i = 0; i < 8; ++i) s.stallAt[i] = _blocksPerFrame == 0 ? 0 : _stallAt[i] % _blocksPerFrame;
        s.syncCycles = _syncCycles;
        s.syncCyclesMax = _syncCyclesMax;
        s.expandCycles = _expandCycles;
        s.expandCyclesMax = _expandCyclesMax;
        s.expandErrors = _expandErrors;
        if (_hw != nullptr && _hw->dma != nullptr) {
                int id = 0;
                (void)dw_gdma_channel_get_id(_hw->dma, &id);
                const intptr_t item =
                        dw_gdma_ll_channel_get_current_link_list_item_addr(&DW_GDMA, static_cast<uint8_t>(id));
                const intptr_t base = reinterpret_cast<intptr_t>(dw_gdma_link_list_get_item(_hw->list, 0));
                const intptr_t next = reinterpret_cast<intptr_t>(dw_gdma_link_list_get_item(_hw->list, 1));
                s.dmaItem = next > base ? static_cast<uint32_t>((item & 0x0fffffff) - (base & 0x0fffffff)) /
                                                  static_cast<uint32_t>(next - base)
                                        : 0;
                s.dmaStatus = dw_gdma_ll_channel_get_intr_status(&DW_GDMA, static_cast<uint8_t>(id));
                s.dmaAmount = dw_gdma_ll_channel_get_trans_amount(&DW_GDMA, static_cast<uint8_t>(id));
                s.bridgeFifo = mipi_dsi_brg_ll_get_fifo_depth(_hw->hal.bridge);
        }
        return s;
}

void Scanout::refillTask(void *arg) {
        static_cast<Scanout *>(arg)->refill();
        for (;;) Task::sleep(WaitForever); // stop() deletes the task
}

// Each sent buffer gets the lines it carries next: buffer (n mod blocks),
// just sent as block n, carries block n + blocks.
BMDS_HOT_ATTR void Scanout::refill() {
        uint32_t lastSent = 0;
        int64_t  progressUs = esp_timer_get_time();
        while (!_stopping.load()) {
                (void)_wake.acquire(StallCheckMs);
                const uint32_t sent = _sent.load(std::memory_order_relaxed);
                const int64_t  now = esp_timer_get_time();
                if (sent != lastSent) {
                        lastSent = sent;
                        progressUs = now;
                } else if (now - progressUs >= StuckUs) {
                        // Stuck: no underrun or stall will come to act on.
                        rebuild();
                        lastSent = 0;
                        progressUs = esp_timer_get_time();
                        continue;
                }
                if (sent - _refilled > _config.blocks) {
                        // The DMA lapped us: those buffers went out again as they were.
                        _late.fetch_add(sent - _refilled - _config.blocks, std::memory_order_relaxed);
                        _refilled = sent - _config.blocks;
                }
                while (_config.framebuffer && _refilled != sent) {
                        // The frame stays: hand the block back. At a frame's start,
                        // the source changes what moves, written back for the DMA.
                        const uint32_t slot = _refilled % _config.blocks;
                        dw_gdma_ll_lli_set_block_markers(_hw->items[_refilled % _items], true, false, true);
                        ++_refilled;
                        if (slot == 0) {
                                const uint32_t start = esp_cpu_get_cycle_count();
                                _source->frame(_ring, _stride);
                                (void)esp_cache_msync(_ring, _blockBytes * _config.blocks,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                                const uint32_t cycles = esp_cpu_get_cycle_count() - start;
                                if (cycles > _renderCyclesMax) _renderCyclesMax = cycles;
                                _renderCycles += cycles;
                        }
                }
                while (_config.freeRunning && !_config.framebuffer) {
                        // The DMA is sending block `sending`; the buffers of the blocks after
                        // it, up to a ring ahead, can be drawn. Behind: skip to the nearest.
                        const uint32_t sending = _sent.load(std::memory_order_relaxed);
                        // Behind the interrupt as a backup: the descriptor just sent valid again.
                        if (sending > 1)
                                dw_gdma_ll_lli_set_block_markers(_hw->items[(sending - 2) % _items], true, false, true);
                        uint32_t next = _refilled + _config.blocks;
                        if (next <= sending) {
                                _skipped += sending + 1 - next;
                                _refilled = sending + 1 - _config.blocks;
                                next = sending + 1;
                        }
                        if (next > sending + _config.blocks - 1) break;
                        const uint32_t start = esp_cpu_get_cycle_count();
                        // From a framebuffer: what moves changes as a frame starts.
                        if (_config.frameSource != nullptr && next % _blocksPerFrame == 0)
                                _source->frame(_config.frameSource, size_t(_config.timing.hActive) * 2);
                        renderSlot(next);
                        const uint32_t cycles = esp_cpu_get_cycle_count() - start;
                        if (cycles > _renderCyclesMax) _renderCyclesMax = cycles;
                        _renderCycles += cycles;
                        ++_refilled;
                }
                while (!_config.freeRunning && _refilled != sent) {
                        const uint32_t start = esp_cpu_get_cycle_count();
                        renderSlot(_refilled + _config.blocks);
                        const uint32_t cycles = esp_cpu_get_cycle_count() - start;
                        if (cycles > _renderCyclesMax) _renderCyclesMax = cycles;
                        _renderCycles += cycles;
                        // Handed back to the DMA (inline: the IDF call is in flash).
                        dw_gdma_ll_lli_set_block_markers(_hw->items[_refilled % _config.blocks], true, false, true);
                        ++_refilled;
                }
                if (_resyncWanted.load(std::memory_order_acquire) && esp_timer_get_time() - _resyncUs >= ResyncGapUs) {
                        resync();
                        lastSent = 0;
                        progressUs = esp_timer_get_time();
                } else if (_stalled.exchange(false, std::memory_order_acquire)) {
                        (void)dw_gdma_channel_continue(_hw->dma);
                }
        }
        return;
}

// After an underrun the bridge sends blue for the rest of the frame and its
// count of the frame's data no longer matches the ring's lines: the picture
// stays offset, or every block stalls. Only a restart of the data path
// aligns them again: the bridge and its DPI output off (the bridge's frame
// timing restarts with them), the DMA stopped, the ring refilled from line
// 0, then everything on again. The DSI link and its video mode keep running.
void Scanout::resync() {
        Hardware       &hw = *_hw;
        dsi_brg_dev_t  *bridge = hw.hal.bridge;
        dsi_host_dev_t *host = hw.hal.host;
        const int       v = _resyncVariant;
        _resyncVariant = 1; // a manual variant is for one resync
        mipi_dsi_brg_ll_enable_dpi_output(bridge, false);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        if (v >= 2) mipi_dsi_host_ll_enable_video_mode(host, false);
        // Stop the DMA before touching its registers (a write to a live
        // channel's is a bus fault). A disable waits for the burst under way,
        // which a wedged bridge never takes: then rebuild everything.
        int id = 0;
        (void)dw_gdma_channel_get_id(hw.dma, &id);
        (void)dw_gdma_channel_enable_ctrl(hw.dma, false);
        bool stopped = false;
        for (int i = 0; i < 20 && !stopped; ++i) {
                stopped = (DW_GDMA.chen0.val & (1u << id)) == 0;
                if (!stopped) esp_rom_delay_us(100);
        }
        if (!stopped) {
                rebuild();
                return;
        }
        dw_gdma_ll_channel_clear_intr(&DW_GDMA, static_cast<uint8_t>(id), UINT32_MAX);
        Task::sleep(1); // a block interrupt already raised lands before the counters restart
        _resyncWanted.store(false, std::memory_order_relaxed);
        _stalled.store(false, std::memory_order_relaxed);
        _blocksBefore += _sent.load(std::memory_order_relaxed);
        _sent.store(0, std::memory_order_relaxed);
        _primed = false;
        _refilled = 0;
        while (_wake.tryAcquire().isOk()) {}
        for (uint32_t i = 0; i < _config.blocks; ++i)
                if (!_config.framebuffer) renderSlot(i);
        for (uint32_t i = 0; i < _items; ++i) dw_gdma_ll_lli_set_block_markers(hw.items[i], true, false, true);
        const VideoTiming &t = _config.timing;
        mipi_dsi_brg_ll_clear_interrupt_status(bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
        mipi_dsi_brg_ll_set_num_pixel_bits(bridge, uint32_t(t.hActive) * t.vActive * bytesPerPixel(_config.format) * 8);
        (void)dw_gdma_channel_use_link_list(hw.dma, hw.list);
        (void)dw_gdma_channel_enable_ctrl(hw.dma, true);
        if (v >= 2) {
                mipi_dsi_host_ll_enable_video_mode(host, true);
                mipi_dsi_host_ll_set_clock_lane_state(host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
        }
        mipi_dsi_brg_ll_enable_dpi_output(bridge, true);
        mipi_dsi_brg_ll_update_dpi_config(bridge);
        ++_resyncs;
        _resyncUs = esp_timer_get_time();
        return;
}

// Renders block @p block (a count since the start) into its buffer and
// writes it back from the cache for the DMA.
BMDS_HOT_ATTR void Scanout::renderSlot(uint32_t block) {
        uint8_t       *dst = _ring + (block % _config.blocks) * _blockBytes;
        const uint32_t first = (block % _blocksPerFrame) * _config.linesPerBlock;
        const uint16_t width = _config.timing.hActive;
        if (_config.frameSource != nullptr) {
                // The 2D-DMA widens the block's lines straight from the framebuffer.
                const uint32_t started = esp_cpu_get_cycle_count();
                if (_widener.expand(_config.frameSource + size_t(first) * width * 2, dst,
                                    size_t(width) * _config.linesPerBlock, ExpandTimeoutMs)
                            .isError())
                        ++_expandErrors;
                const uint32_t cycles = esp_cpu_get_cycle_count() - started;
                _expandCycles += cycles;
                if (cycles > _expandCyclesMax) _expandCyclesMax = cycles;
                return;
        }
        if (isExpanded(_config.format)) {
                // Drawn by the CPU, written back; the PPA (indices) or the
                // 2D-DMA (RGB565) turns them into RGB888.
                const size_t drawnStride = size_t(width) * drawnBytesPerPixel(_config.format);
                _source->render(static_cast<uint16_t>(first), _config.linesPerBlock, _staging, drawnStride);
                (void)esp_cache_msync(_staging, drawnStride * _config.linesPerBlock, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                const uint32_t started = esp_cpu_get_cycle_count();
                const Error    err =
                        _config.format == Format::Indexed8
                                   ? _expander.expand(_staging, dst, width, _config.linesPerBlock, ExpandTimeoutMs)
                                   : _widener.expand(_staging, dst, size_t(width) * _config.linesPerBlock,
                                                     ExpandTimeoutMs);
                if (err.isError()) ++_expandErrors;
                const uint32_t cycles = esp_cpu_get_cycle_count() - started;
                _expandCycles += cycles;
                if (cycles > _expandCyclesMax) _expandCyclesMax = cycles;
                return;
        }
        _source->render(static_cast<uint16_t>(first), _config.linesPerBlock, dst, _stride);
        const uint32_t synced = esp_cpu_get_cycle_count();
        (void)esp_cache_msync(dst, _blockBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        const uint32_t sync = esp_cpu_get_cycle_count() - synced;
        _syncCycles += sync;
        if (sync > _syncCyclesMax) _syncCyclesMax = sync;
        return;
}

BMDS_NAMESPACE_END
