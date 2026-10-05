/**
 * @file      paletteexpander.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The PPA blender with its CLUT, on the 2D-DMA (devplan/display.md §5). The
 * register sequence follows IDF's PPA blend (esp_driver_ppa/src/ppa_blend.c),
 * with the background plane in L8, a color mode IDF leaves out.
 */

#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_private/dma2d.h>
#include <esp_private/periph_ctrl.h>
#include <hal/axi_icm_ll.h>
#include <hal/dma2d_types.h>
#include <hal/ppa_ll.h>
#include <soc/dma2d_channel.h>
#include <bmds/memory.h>
#include <bmds/paletteexpander.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   DescriptorAlign = 64; // the cache line; the 2D-DMA wants 8
        constexpr uint32_t RxL8 = 4;             // blend0_rx_cm: L8 through the CLUT (ppa_ll.h, commented out)
        constexpr uint32_t RxA8 = 6;             // blend1_rx_cm: A8
        constexpr uint32_t TxRgb888 = 1;         // blend_tx_cm

        // A 2D-DMA descriptor for one block of @p w × @p h pixels of @p pbyte.
        void describe(dma2d_descriptor_t *d, const void *buffer, uint16_t w, uint16_t h, uint32_t pbyte) {
                std::memset(d, 0, sizeof(*d));
                d->vb_size = h;
                d->hb_length = w;
                d->va_size = h;
                d->ha_length = w;
                d->pbyte = pbyte;
                d->x = 0;
                d->y = 0;
                d->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
                d->dma2d_en = 1;
                d->suc_eof = 1;
                d->err_eof = 0;
                d->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
                d->buffer = const_cast<void *>(buffer);
                d->next = nullptr;
                return;
        }

} // namespace

struct PaletteExpander::Hardware {
                dma2d_pool_handle_t  pool = nullptr;
                dma2d_trans_t       *job = nullptr; // the 2D-DMA's placeholder
                dma2d_trans_config_t config = {};
                dma2d_descriptor_t  *bg = nullptr;
                dma2d_descriptor_t  *fg = nullptr;
                dma2d_descriptor_t  *rx = nullptr;
                size_t               descriptorBytes = 0;
                bool                 clock = false;
};

struct PaletteExpanderIsr {
                // All three channels are ours: describe the planes, connect them to
                // the blender, start them, then the blender.
                static bool picked(uint32_t count, const dma2d_trans_channel_info_t *chans, void *arg) {
                        auto                      *self = static_cast<PaletteExpander *>(arg);
                        PaletteExpander::Hardware &hw = *self->_hw;
                        dma2d_channel_handle_t     bg = nullptr, fg = nullptr, rx = nullptr;
                        for (uint32_t i = 0; i < count; ++i) {
                                if (chans[i].dir == DMA2D_CHANNEL_DIRECTION_TX) {
                                        (bg == nullptr ? bg : fg) = chans[i].chan;
                                } else {
                                        rx = chans[i].chan;
                                }
                        }
                        ppa_dev_t *dev = &PPA;
                        ppa_ll_blend_reset(dev);
                        (void)esp_cache_msync(hw.bg, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        (void)esp_cache_msync(hw.fg, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        (void)esp_cache_msync(hw.rx, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        dma2d_trigger_t trigger = {};
                        trigger.periph = DMA2D_TRIG_PERIPH_PPA_BLEND;
                        trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_BG_TX;
                        (void)dma2d_connect(bg, &trigger);
                        trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_FG_TX;
                        (void)dma2d_connect(fg, &trigger);
                        trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_RX;
                        (void)dma2d_connect(rx, &trigger);
                        dma2d_transfer_ability_t ability = {};
                        ability.data_burst_length = DMA2D_DATA_BURST_LENGTH_128;
                        ability.desc_burst_en = true;
                        ability.mb_size = DMA2D_MACRO_BLOCK_SIZE_NONE;
                        (void)dma2d_set_transfer_ability(bg, &ability);
                        (void)dma2d_set_transfer_ability(fg, &ability);
                        (void)dma2d_set_transfer_ability(rx, &ability);
                        dma2d_rx_event_callbacks_t callbacks = {};
                        callbacks.on_recv_eof = done;
                        (void)dma2d_register_rx_event_callbacks(rx, &callbacks, self);
                        (void)dma2d_set_desc_addr(bg, reinterpret_cast<intptr_t>(hw.bg));
                        (void)dma2d_set_desc_addr(fg, reinterpret_cast<intptr_t>(hw.fg));
                        (void)dma2d_set_desc_addr(rx, reinterpret_cast<intptr_t>(hw.rx));
                        (void)dma2d_start(bg);
                        (void)dma2d_start(fg);
                        (void)dma2d_start(rx);

                        dev->blend_color_mode.blend0_rx_cm = RxL8;
                        ppa_ll_blend_enable_rx_bg_byte_swap(dev, false);
                        ppa_ll_blend_enable_rx_bg_rgb_swap(dev, false);
                        ppa_ll_blend_configure_rx_bg_alpha(dev, PPA_ALPHA_FIX_VALUE, 0xff);
                        dev->blend_color_mode.blend1_rx_cm = RxA8;
                        color_pixel_rgb888_data_t black = {.b = 0, .g = 0, .r = 0};
                        ppa_ll_blend_set_rx_fg_fix_rgb(dev, &black);
                        ppa_ll_blend_enable_rx_fg_byte_swap(dev, false);
                        ppa_ll_blend_enable_rx_fg_rgb_swap(dev, false);
                        ppa_ll_blend_configure_rx_fg_alpha(dev, PPA_ALPHA_FIX_VALUE, 0); // invisible
                        dev->blend_color_mode.blend_tx_cm = TxRgb888;
                        // Color keying off (empty ranges), as IDF's blend does.
                        color_pixel_rgb888_data_t low = {.b = 0xff, .g = 0xff, .r = 0xff};
                        ppa_ll_blend_configure_rx_bg_ck_range(dev, &low, &black);
                        ppa_ll_blend_configure_rx_fg_ck_range(dev, &low, &black);
                        ppa_ll_blend_set_ck_default_rgb(dev, &black);
                        ppa_ll_blend_enable_ck_fg_bg_reverse(dev, false);
                        ppa_ll_blend_start(dev, PPA_LL_BLEND_TRANS_MODE_BLEND);
                        return false;
                }

                static bool done(dma2d_channel_handle_t, dma2d_event_data_t *, void *arg) {
                        static_cast<PaletteExpander *>(arg)->_done.releaseFromIsr();
                        return false;
                }
};

Error PaletteExpander::start() {
        if (_hw != nullptr) return Error::InvalidState;
        void *memory = Memory::alloc(MemSpace::Internal, sizeof(Hardware));
        if (memory == nullptr) return Error::NoMem;
        _hw = new (memory) Hardware();
        Hardware &hw = *_hw;
        hw.descriptorBytes =
                (sizeof(dma2d_descriptor_align8_t) + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign;
        hw.bg = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.descriptorBytes, DescriptorAlign));
        hw.fg = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.descriptorBytes, DescriptorAlign));
        hw.rx = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.descriptorBytes, DescriptorAlign));
        hw.job = static_cast<dma2d_trans_t *>(Memory::alloc(MemSpace::Internal, dma2d_get_trans_elm_size()));
        if (hw.bg == nullptr || hw.fg == nullptr || hw.rx == nullptr || hw.job == nullptr) {
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
        // The 2D-DMA ahead of the caches on the interconnect, like the scanout's DMA.
        axi_icm_ll_set_dma2d_qos_arbiter_prio(15, 15);
        ppa_dev_t *dev = &PPA;
        ppa_ll_blend_enable_clut_mem(dev);
        ppa_ll_configure_clut_access_mode(dev, false); // memory mode: entries by index
        ppa_ll_blend_reset_rx_bg_clut_mem(dev);
        hw.config.tx_channel_num = 2;
        hw.config.rx_channel_num = 1;
        hw.config.channel_flags = 0;
        hw.config.on_job_picked = PaletteExpanderIsr::picked;
        hw.config.user_config = this;
        return Error::Ok;
}

void PaletteExpander::stop() {
        if (_hw == nullptr) return;
        Hardware &hw = *_hw;
        if (hw.pool != nullptr) (void)dma2d_release_pool(hw.pool);
        if (hw.clock) {
                ppa_ll_blend_disable_clut_mem(&PPA);
                PERIPH_RCC_ATOMIC() {
                        ppa_ll_enable_bus_clock(false);
                }
        }
        Memory::free(hw.bg);
        Memory::free(hw.fg);
        Memory::free(hw.rx);
        Memory::free(hw.job);
        hw.~Hardware();
        Memory::free(_hw);
        _hw = nullptr;
        return;
}

void PaletteExpander::setPalette(const uint32_t *rgb, size_t first, size_t count) {
        if (_hw == nullptr) return;
        for (size_t i = 0; i < count && first + i < PaletteSize; ++i)
                ppa_ll_blend_wr_rx_bg_clut_data_by_mem(&PPA, static_cast<uint32_t>(first + i), 0xff000000u | rgb[i]);
        return;
}

Error PaletteExpander::expand(const uint8_t *indices, uint8_t *rgb, uint16_t width, uint16_t lines,
                              unsigned int timeoutMs) {
        if (_hw == nullptr) return Error::InvalidState;
        Hardware &hw = *_hw;
        describe(hw.bg, indices, width, lines, DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL);
        describe(hw.fg, indices, width, lines, DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL);
        describe(hw.rx, rgb, width, lines, DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL);
        const esp_err_t err = dma2d_enqueue(hw.pool, &hw.config, hw.job);
        if (err != ESP_OK) return Error::fromEspErr(err);
        return _done.acquire(timeoutMs);
}

BMDS_NAMESPACE_END
