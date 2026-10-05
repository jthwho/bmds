/**
 * @file      colorexpander.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * RGB565 to RGB888 on the 2D-DMA's transmit color converter, memory to memory
 * (devplan/display.md §5). The setup follows IDF's 2D-DMA test of the same
 * conversion (esp_hw_support/test_apps/dma2d): linear (not 2D) descriptors.
 */

#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_private/dma2d.h>
#include <hal/axi_icm_ll.h>
#include <hal/dma2d_types.h>
#include <soc/dma2d_channel.h>
#include <bmds/colorexpander.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t DescriptorAlign = 64;             // the cache line; the 2D-DMA wants 8
        constexpr size_t MaxBytes = (size_t(1) << 28) - 1; // a linear descriptor's 28-bit length

        // A linear descriptor: @p length bytes to send (0 to receive into
        // @p size bytes; the DMA fills the length in).
        void describe(dma2d_descriptor_t *d, const void *buffer, size_t length, size_t size, bool eof) {
                std::memset(d, 0, sizeof(*d));
                d->hb_length = length & 0x3fff;
                d->ha_length = length >> 14;
                d->vb_size = size & 0x3fff;
                d->va_size = size >> 14;
                d->pbyte = DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL;
                d->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
                d->dma2d_en = 0;
                d->suc_eof = eof ? 1 : 0;
                d->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
                d->buffer = const_cast<void *>(buffer);
                d->next = nullptr;
                return;
        }

} // namespace

struct ColorExpander::Hardware {
                dma2d_pool_handle_t  pool = nullptr;
                dma2d_trans_t       *job = nullptr; // the 2D-DMA's placeholder
                dma2d_trans_config_t config = {};
                dma2d_descriptor_t  *tx = nullptr;
                dma2d_descriptor_t  *rx = nullptr;
                size_t               descriptorBytes = 0;
};

struct ColorExpanderIsr {
                static bool picked(uint32_t count, const dma2d_trans_channel_info_t *chans, void *arg) {
                        auto                    *self = static_cast<ColorExpander *>(arg);
                        ColorExpander::Hardware &hw = *self->_hw;
                        dma2d_channel_handle_t   tx = nullptr, rx = nullptr;
                        for (uint32_t i = 0; i < count; ++i)
                                (chans[i].dir == DMA2D_CHANNEL_DIRECTION_TX ? tx : rx) = chans[i].chan;
                        (void)esp_cache_msync(hw.tx, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        (void)esp_cache_msync(hw.rx, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        dma2d_trigger_t trigger = {};
                        trigger.periph = DMA2D_TRIG_PERIPH_M2M;
                        trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX;
                        (void)dma2d_connect(tx, &trigger);
                        trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
                        (void)dma2d_connect(rx, &trigger);
                        dma2d_transfer_ability_t ability = {};
                        ability.data_burst_length = DMA2D_DATA_BURST_LENGTH_128;
                        ability.desc_burst_en = true;
                        ability.mb_size = DMA2D_MACRO_BLOCK_SIZE_NONE;
                        (void)dma2d_set_transfer_ability(tx, &ability);
                        (void)dma2d_set_transfer_ability(rx, &ability);
                        dma2d_csc_config_t csc = {};
                        csc.tx_csc_option = DMA2D_CSC_TX_RGB565_TO_RGB888;
                        csc.pre_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
                        (void)dma2d_configure_color_space_conversion(tx, &csc);
                        dma2d_rx_event_callbacks_t callbacks = {};
                        callbacks.on_recv_eof = done;
                        (void)dma2d_register_rx_event_callbacks(rx, &callbacks, self);
                        (void)dma2d_set_desc_addr(tx, reinterpret_cast<intptr_t>(hw.tx));
                        (void)dma2d_set_desc_addr(rx, reinterpret_cast<intptr_t>(hw.rx));
                        (void)dma2d_start(tx);
                        (void)dma2d_start(rx);
                        return false;
                }

                static bool done(dma2d_channel_handle_t, dma2d_event_data_t *, void *arg) {
                        static_cast<ColorExpander *>(arg)->_done.releaseFromIsr();
                        return false;
                }
};

Error ColorExpander::start() {
        if (_hw != nullptr) return Error::InvalidState;
        void *memory = Memory::alloc(MemSpace::Internal, sizeof(Hardware));
        if (memory == nullptr) return Error::NoMem;
        _hw = new (memory) Hardware();
        Hardware &hw = *_hw;
        hw.descriptorBytes =
                (sizeof(dma2d_descriptor_align8_t) + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign;
        hw.tx = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.descriptorBytes, DescriptorAlign));
        hw.rx = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.descriptorBytes, DescriptorAlign));
        hw.job = static_cast<dma2d_trans_t *>(Memory::alloc(MemSpace::Internal, dma2d_get_trans_elm_size()));
        if (hw.tx == nullptr || hw.rx == nullptr || hw.job == nullptr) {
                stop();
                return Error::NoMem;
        }
        dma2d_pool_config_t pool = {};
        pool.pool_id = 0;
        const esp_err_t err = dma2d_acquire_pool(&pool, &hw.pool);
        if (err != ESP_OK) {
                stop();
                return Error::fromEspErr(err);
        }
        // The 2D-DMA ahead of the caches on the interconnect, like the scanout's DMA.
        axi_icm_ll_set_dma2d_qos_arbiter_prio(15, 15);
        hw.config.tx_channel_num = 1;
        hw.config.rx_channel_num = 1;
        // A transmit channel with the converter, and a receive channel of the same number.
        hw.config.channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING | DMA2D_CHANNEL_FUNCTION_FLAG_TX_CSC;
        hw.config.on_job_picked = ColorExpanderIsr::picked;
        hw.config.user_config = this;
        return Error::Ok;
}

void ColorExpander::stop() {
        if (_hw == nullptr) return;
        Hardware &hw = *_hw;
        if (hw.pool != nullptr) (void)dma2d_release_pool(hw.pool);
        Memory::free(hw.tx);
        Memory::free(hw.rx);
        Memory::free(hw.job);
        hw.~Hardware();
        Memory::free(_hw);
        _hw = nullptr;
        return;
}

Error ColorExpander::expand(const uint8_t *src, uint8_t *dst, size_t pixels, unsigned int timeoutMs) {
        if (_hw == nullptr) return Error::InvalidState;
        if (pixels * 3 > MaxBytes) return Error::Invalid;
        Hardware &hw = *_hw;
        describe(hw.tx, src, pixels * 2, pixels * 2, true);
        describe(hw.rx, dst, 0, pixels * 3, false);
        const esp_err_t err = dma2d_enqueue(hw.pool, &hw.config, hw.job);
        if (err != ESP_OK) return Error::fromEspErr(err);
        return _done.acquire(timeoutMs);
}

BMDS_NAMESPACE_END
