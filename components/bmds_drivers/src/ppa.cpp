/**
 * @file      ppa.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The PPA's scaler (SRM) and its blender's CLUT on the 2D-DMA (devplan/games.md §10). The
 * register sequences follow IDF's (esp_driver_ppa/src/ppa_srm.c, ppa_blend.c), with the
 * blender's background in L8, a color mode IDF leaves out (as the palette expander of
 * devplan/display.md §8 did).
 */

#include <atomic>
#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_private/dma2d.h>
#include <esp_private/periph_ctrl.h>
#include <hal/dma2d_types.h>
#include <hal/ppa_ll.h>
#include <soc/dma2d_channel.h>
#include <bmds/memory.h>
#include <bmds/ppa.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t       DescriptorAlign = 64; // the cache line; the 2D-DMA wants 8
        constexpr unsigned int StopTimeoutMs = 100;
        constexpr uint32_t     MaxField = 0x3fff; // a descriptor's 14-bit sizes and offsets
        constexpr uint32_t     BlendRxL8 = 4;     // blend0_rx_cm: L8 through the CLUT (ppa_ll.h, commented out)
        constexpr uint32_t     BlendRxA8 = 6;     // blend1_rx_cm: A8
        constexpr uint32_t     BlendTxRgb888 = 1; // blend_tx_cm

        // The one PPA: its registers and CLUT are one user's from start() to stop() (a full-screen
        // presenter, a benchmark); a second start() is refused rather than sharing them.
        std::atomic<bool> claimed{false};

        uint32_t pbyte(Ppa::Format format) {
                switch (Ppa::bytesPerPixel(format)) {
                        case 1: return DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL;
                        case 2: return DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL;
                        case 3: return DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
                        default: return DMA2D_DESCRIPTOR_PBYTE_4B0_PER_PIXEL;
                }
        }

        // The SRM's color mode for @p format, or false (L8; Gray8 before chip revision 3).
        bool srmMode(Ppa::Format format, ppa_srm_color_mode_t &mode) {
                switch (format) {
                        case Ppa::Format::Rgb565: mode = PPA_SRM_COLOR_MODE_RGB565; break;
                        case Ppa::Format::Rgb888: mode = PPA_SRM_COLOR_MODE_RGB888; break;
                        case Ppa::Format::Argb8888: mode = PPA_SRM_COLOR_MODE_ARGB8888; break;
                        case Ppa::Format::Gray8: mode = PPA_SRM_COLOR_MODE_GRAY8; break;
                        default: return false;
                }
                return ppa_ll_srm_is_color_mode_supported(mode);
        }

        // A block of @p w × @p h pixels at (@p x, @p y) of @p p.
        void describe(dma2d_descriptor_t *d, const Ppa::Picture &p, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
                std::memset(d, 0, sizeof(*d));
                d->ha_length = p.width;
                d->va_size = p.height;
                d->hb_length = w;
                d->vb_size = h;
                d->x = x;
                d->y = y;
                d->pbyte = pbyte(p.format);
                d->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
                d->dma2d_en = 1;
                d->suc_eof = 1;
                d->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
                d->buffer = const_cast<uint8_t *>(p.data);
                d->next = nullptr;
                return;
        }

        bool fits(const Ppa::Picture &p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                return p.data != nullptr && p.width != 0 && p.width <= MaxField && p.height <= MaxField && w != 0 &&
                       h != 0 && x + w <= p.width && y + h <= p.height;
        }

} // namespace

struct Ppa::Hardware {
                dma2d_pool_handle_t  pool = nullptr;
                dma2d_trans_t       *job = nullptr; // the 2D-DMA's placeholder
                dma2d_trans_config_t config = {};
                dma2d_descriptor_t  *tx[2] = {};
                dma2d_descriptor_t  *rx = nullptr;
                size_t               descriptorBytes = 0;
                ppa_srm_color_mode_t srmIn = PPA_SRM_COLOR_MODE_RGB888;
                ppa_srm_color_mode_t srmOut = PPA_SRM_COLOR_MODE_RGB888;
                uint16_t             scaleX16 = 16;
                uint16_t             scaleY16 = 16;
                Job                  kind = Job::Scale;
                bool                 clock = false;
};

struct PpaIsr {
                static void sync(Ppa::Hardware &hw, dma2d_descriptor_t *d) {
                        (void)esp_cache_msync(d, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        return;
                }

                static void prepare(dma2d_channel_handle_t chan, const dma2d_trigger_t &trigger) {
                        (void)dma2d_connect(chan, &trigger);
                        dma2d_transfer_ability_t ability = {};
                        ability.data_burst_length = DMA2D_DATA_BURST_LENGTH_128;
                        ability.desc_burst_en = true;
                        ability.mb_size = DMA2D_MACRO_BLOCK_SIZE_NONE;
                        (void)dma2d_set_transfer_ability(chan, &ability);
                        return;
                }

                static bool picked(uint32_t count, const dma2d_trans_channel_info_t *chans, void *arg) {
                        auto                  *self = static_cast<Ppa *>(arg);
                        Ppa::Hardware         &hw = *self->_hw;
                        dma2d_channel_handle_t tx[2] = {}, rx = nullptr;
                        size_t                 txCount = 0;
                        for (uint32_t i = 0; i < count; ++i) {
                                if (chans[i].dir == DMA2D_CHANNEL_DIRECTION_TX) {
                                        if (txCount < 2) tx[txCount++] = chans[i].chan;
                                } else {
                                        rx = chans[i].chan;
                                }
                        }
                        ppa_dev_t      *dev = &PPA;
                        dma2d_trigger_t trigger = {};
                        if (hw.kind == Ppa::Job::Scale) {
                                ppa_ll_srm_reset(dev);
                                sync(hw, hw.tx[0]);
                                sync(hw, hw.rx);
                                trigger.periph = DMA2D_TRIG_PERIPH_PPA_SRM;
                                trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_SRM_TX;
                                prepare(tx[0], trigger);
                                trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_SRM_RX;
                                prepare(rx, trigger);
                                // The SRM takes macro blocks with a pixel around (its interpolation).
                                uint32_t blockH = 0, blockV = 0;
                                ppa_ll_srm_get_dma_dscr_port_mode_block_size(dev, hw.srmIn, ppa_ll_srm_get_mb_size(dev),
                                                                             &blockH, &blockV);
                                dma2d_dscr_port_mode_config_t port = {};
                                port.block_h = blockH;
                                port.block_v = blockV;
                                (void)dma2d_configure_dscr_port_mode(tx[0], &port);
                        } else {
                                ppa_ll_blend_reset(dev);
                                const bool bypass = hw.kind == Ppa::Job::ExpandBypass;
                                sync(hw, hw.tx[0]);
                                if (!bypass) sync(hw, hw.tx[1]);
                                sync(hw, hw.rx);
                                trigger.periph = DMA2D_TRIG_PERIPH_PPA_BLEND;
                                trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_BG_TX;
                                prepare(tx[0], trigger);
                                if (!bypass) {
                                        trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_FG_TX;
                                        prepare(tx[1], trigger);
                                }
                                trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_RX;
                                prepare(rx, trigger);
                        }
                        dma2d_rx_event_callbacks_t callbacks = {};
                        callbacks.on_recv_eof = done;
                        (void)dma2d_register_rx_event_callbacks(rx, &callbacks, self);
                        (void)dma2d_set_desc_addr(tx[0], reinterpret_cast<intptr_t>(hw.tx[0]));
                        if (txCount == 2) (void)dma2d_set_desc_addr(tx[1], reinterpret_cast<intptr_t>(hw.tx[1]));
                        (void)dma2d_set_desc_addr(rx, reinterpret_cast<intptr_t>(hw.rx));
                        for (size_t i = 0; i < txCount; ++i) (void)dma2d_start(tx[i]);
                        (void)dma2d_start(rx);

                        if (hw.kind == Ppa::Job::Scale) {
                                ppa_ll_srm_set_rx_color_mode(dev, hw.srmIn);
                                ppa_ll_srm_enable_rx_byte_swap(dev, false);
                                ppa_ll_srm_enable_rx_rgb_swap(dev, false);
                                ppa_ll_srm_configure_rx_alpha(dev, PPA_ALPHA_NO_CHANGE, 0);
                                ppa_ll_srm_set_tx_color_mode(dev, hw.srmOut);
                                ppa_ll_srm_set_rotation_angle(dev, PPA_SRM_ROTATION_ANGLE_0);
                                ppa_ll_srm_set_scaling_x(dev, hw.scaleX16 >> 4, hw.scaleX16 & 15);
                                ppa_ll_srm_set_scaling_y(dev, hw.scaleY16 >> 4, hw.scaleY16 & 15);
                                ppa_ll_srm_enable_mirror_x(dev, false);
                                ppa_ll_srm_enable_mirror_y(dev, false);
                                ppa_ll_srm_start(dev);
                        } else {
                                dev->blend_color_mode.blend0_rx_cm = BlendRxL8;
                                ppa_ll_blend_enable_rx_bg_byte_swap(dev, false);
                                ppa_ll_blend_enable_rx_bg_rgb_swap(dev, false);
                                ppa_ll_blend_configure_rx_bg_alpha(dev, PPA_ALPHA_FIX_VALUE, 0xff);
                                dev->blend_color_mode.blend1_rx_cm = BlendRxA8;
                                color_pixel_rgb888_data_t black = {.b = 0, .g = 0, .r = 0};
                                ppa_ll_blend_set_rx_fg_fix_rgb(dev, &black);
                                ppa_ll_blend_enable_rx_fg_byte_swap(dev, false);
                                ppa_ll_blend_enable_rx_fg_rgb_swap(dev, false);
                                ppa_ll_blend_configure_rx_fg_alpha(dev, PPA_ALPHA_FIX_VALUE, 0); // invisible
                                dev->blend_color_mode.blend_tx_cm = BlendTxRgb888;
                                // Color keying off (empty ranges), as IDF's blend does.
                                color_pixel_rgb888_data_t low = {.b = 0xff, .g = 0xff, .r = 0xff};
                                ppa_ll_blend_configure_rx_bg_ck_range(dev, &low, &black);
                                ppa_ll_blend_configure_rx_fg_ck_range(dev, &low, &black);
                                ppa_ll_blend_set_ck_default_rgb(dev, &black);
                                ppa_ll_blend_enable_ck_fg_bg_reverse(dev, false);
                                ppa_ll_blend_start(dev, hw.kind == Ppa::Job::ExpandBypass
                                                                ? PPA_LL_BLEND_TRANS_MODE_BYPASS_BLEND
                                                                : PPA_LL_BLEND_TRANS_MODE_BLEND);
                        }
                        return false;
                }

                static bool done(dma2d_channel_handle_t, dma2d_event_data_t *, void *arg) {
                        static_cast<Ppa *>(arg)->_done.releaseFromIsr();
                        return false;
                }
};

Error Ppa::start() {
        if (_hw != nullptr) return Error::InvalidState;
        if (claimed.exchange(true, std::memory_order_acq_rel)) return Error::Busy;
        // A completion left from a job of an earlier start, cleared.
        while (_done.tryAcquire().isOk()) {}
        _busy = false;
        void *memory = Memory::alloc(MemSpace::Internal, sizeof(Hardware));
        if (memory == nullptr) {
                claimed.store(false, std::memory_order_release);
                return Error::NoMem;
        }
        _hw = new (memory) Hardware();
        Hardware &hw = *_hw;
        hw.descriptorBytes =
                (sizeof(dma2d_descriptor_align8_t) + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign;
        for (auto &d : hw.tx)
                d = static_cast<dma2d_descriptor_t *>(
                        Memory::allocAligned(MemSpace::InternalDma, hw.descriptorBytes, DescriptorAlign));
        hw.rx = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.descriptorBytes, DescriptorAlign));
        hw.job = static_cast<dma2d_trans_t *>(Memory::alloc(MemSpace::Internal, dma2d_get_trans_elm_size()));
        if (hw.tx[0] == nullptr || hw.tx[1] == nullptr || hw.rx == nullptr || hw.job == nullptr) {
                stop();
                return Error::NoMem;
        }
        PERIPH_RCC_ATOMIC() {
                ppa_ll_enable_bus_clock(true);
                ppa_ll_reset_register();
        }
        hw.clock = true;
        dma2d_pool_config_t pool = {};
        pool.pool_id = 0;
        const esp_err_t err = dma2d_acquire_pool(&pool, &hw.pool);
        if (err != ESP_OK) {
                stop();
                return Error::fromEspErr(err);
        }
        ppa_dev_t *dev = &PPA;
        ppa_ll_blend_enable_clut_mem(dev);
        ppa_ll_configure_clut_access_mode(dev, false); // memory mode: entries by index
        ppa_ll_blend_reset_rx_bg_clut_mem(dev);
        for (size_t i = 0; i < PaletteSize; ++i)
                ppa_ll_blend_wr_rx_bg_clut_data_by_mem(dev, static_cast<uint32_t>(i), 0xff000000u);
        hw.config.user_config = this;
        hw.config.on_job_picked = PpaIsr::picked;
        return Error::Ok;
}

void Ppa::stop() {
        if (_hw == nullptr) return;
        (void)wait(StopTimeoutMs); // a job under way finishes before its memory goes
        Hardware &hw = *_hw;
        if (hw.pool != nullptr) (void)dma2d_release_pool(hw.pool);
        if (hw.clock) {
                ppa_ll_blend_disable_clut_mem(&PPA);
                PERIPH_RCC_ATOMIC() {
                        ppa_ll_enable_bus_clock(false);
                }
        }
        for (auto d : hw.tx) Memory::free(d);
        Memory::free(hw.rx);
        Memory::free(hw.job);
        hw.~Hardware();
        Memory::free(_hw);
        _hw = nullptr;
        claimed.store(false, std::memory_order_release);
        return;
}

void Ppa::setPalette(const uint32_t *rgb, size_t first, size_t count) {
        if (_hw == nullptr) return;
        for (size_t i = 0; i < count && first + i < PaletteSize; ++i)
                ppa_ll_blend_wr_rx_bg_clut_data_by_mem(&PPA, static_cast<uint32_t>(first + i), 0xff000000u | rgb[i]);
        return;
}

Error Ppa::scale(const Picture &from, const Rect &block, const Picture &to, uint16_t toX, uint16_t toY,
                 uint16_t scaleX16, uint16_t scaleY16, unsigned int timeoutMs) {
        if (_hw == nullptr) return Error::InvalidState;
        Hardware            &hw = *_hw;
        ppa_srm_color_mode_t in = PPA_SRM_COLOR_MODE_RGB888, out = PPA_SRM_COLOR_MODE_RGB888;
        if (!srmMode(from.format, in) || !srmMode(to.format, out)) return Error::Invalid;
        if (scaleX16 == 0 || scaleX16 > MaxScale16 || scaleY16 == 0 || scaleY16 > MaxScale16) return Error::Invalid;
        if (!fits(from, block.x, block.y, block.width, block.height) ||
            !fits(to, toX, toY, scaled(block.width, scaleX16), scaled(block.height, scaleY16)))
                return Error::Invalid;
        // The last job's descriptors stay until it's done.
        if (_busy && wait(timeoutMs).isError()) return Error::Timeout;
        describe(hw.tx[0], from, block.x, block.y, block.width, block.height);
        // The receive side's block size is the engine's (any nonzero value: written back).
        describe(hw.rx, to, toX, toY, 2, 2);
        hw.srmIn = in;
        hw.srmOut = out;
        hw.scaleX16 = scaleX16;
        hw.scaleY16 = scaleY16;
        hw.kind = Job::Scale;
        hw.config.tx_channel_num = 1;
        hw.config.rx_channel_num = 1;
        hw.config.channel_flags = 0;
        const esp_err_t err = dma2d_enqueue(hw.pool, &hw.config, hw.job);
        if (err != ESP_OK) return Error::fromEspErr(err);
        _busy = true;
        return Error::Ok;
}

Error Ppa::expand(const Picture &from, const Rect &block, const Picture &to, uint16_t toX, uint16_t toY, bool bypass,
                  unsigned int timeoutMs) {
        if (_hw == nullptr) return Error::InvalidState;
        Hardware &hw = *_hw;
        if (from.format != Format::L8 || to.format != Format::Rgb888) return Error::Invalid;
        if (!fits(from, block.x, block.y, block.width, block.height) || !fits(to, toX, toY, block.width, block.height))
                return Error::Invalid;
        if (_busy && wait(timeoutMs).isError()) return Error::Timeout;
        describe(hw.tx[0], from, block.x, block.y, block.width, block.height);
        describe(hw.tx[1], from, block.x, block.y, block.width, block.height); // the foreground: invisible
        describe(hw.rx, to, toX, toY, block.width, block.height);
        hw.kind = bypass ? Job::ExpandBypass : Job::Expand;
        hw.config.tx_channel_num = bypass ? 1 : 2;
        hw.config.rx_channel_num = 1;
        hw.config.channel_flags = 0;
        const esp_err_t err = dma2d_enqueue(hw.pool, &hw.config, hw.job);
        if (err != ESP_OK) return Error::fromEspErr(err);
        _busy = true;
        return Error::Ok;
}

Error Ppa::wait(unsigned int timeoutMs) {
        if (!_busy) return Error::Ok;
        const Error err = _done.acquire(timeoutMs);
        if (err.isOk()) _busy = false;
        return err;
}

BMDS_NAMESPACE_END
