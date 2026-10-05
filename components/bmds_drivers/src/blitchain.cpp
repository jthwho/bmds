/**
 * @file      blitchain.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Chained 2D-DMA rectangle copies: RectCopier's 2D descriptors, several to a
 * job, the transmit channel's converter for RGB565 sources.
 */

#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_private/dma2d.h>
#include <hal/axi_icm_ll.h>
#include <hal/dma2d_types.h>
#include <soc/dma2d_channel.h>
#include <bmds/blitchain.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t       DescriptorAlign = 64; // the cache line; the 2D-DMA wants 8
        constexpr size_t       DescriptorBytes = sizeof(dma2d_descriptor_align8_t);
        constexpr unsigned int StopTimeoutMs = 100;
        constexpr uint32_t     MaxField = 0x3fff; // a descriptor's 14-bit sizes and offsets

        uint32_t pbyte(uint8_t bytesPerPixel) {
                switch (bytesPerPixel) {
                        case 1: return DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL;
                        case 2: return DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL;
                        case 3: return DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
                        default: return DMA2D_DESCRIPTOR_PBYTE_4B0_PER_PIXEL;
                }
        }

        void describe(dma2d_descriptor_t *d, const BlitChain::Picture &p, uint16_t x, uint16_t y, uint16_t w,
                      uint16_t h, bool eof, dma2d_descriptor_t *next) {
                std::memset(d, 0, sizeof(*d));
                d->ha_length = p.width;
                d->va_size = p.height;
                d->hb_length = w;
                d->vb_size = h;
                d->x = x;
                d->y = y;
                d->pbyte = pbyte(p.bytesPerPixel);
                d->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
                d->dma2d_en = 1;
                d->suc_eof = eof ? 1 : 0;
                d->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
                d->buffer = const_cast<uint8_t *>(p.data);
                d->next = next;
                return;
        }

        bool fits(const BlitChain::Picture &p, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
                return p.data != nullptr && p.width <= MaxField && p.height <= MaxField && uint32_t(x) + w <= p.width &&
                       uint32_t(y) + h <= p.height && p.bytesPerPixel >= 1 && p.bytesPerPixel <= 4;
        }

        dma2d_descriptor_t *at(dma2d_descriptor_t *base, size_t i) {
                return reinterpret_cast<dma2d_descriptor_t *>(reinterpret_cast<uint8_t *>(base) + i * DescriptorBytes);
        }

} // namespace

struct BlitChain::Hardware {
                dma2d_pool_handle_t  pool = nullptr;
                dma2d_trans_t       *job = nullptr; // the 2D-DMA's placeholder
                dma2d_trans_config_t config = {};
                dma2d_descriptor_t  *tx = nullptr; // _max descriptors each
                dma2d_descriptor_t  *rx = nullptr;
                size_t               descriptorBytes = 0; // of each chain, rounded to the cache line
};

struct BlitChainIsr {
                static bool picked(uint32_t count, const dma2d_trans_channel_info_t *chans, void *arg) {
                        auto                  *self = static_cast<BlitChain *>(arg);
                        BlitChain::Hardware   &hw = *self->_hw;
                        dma2d_channel_handle_t tx = nullptr, rx = nullptr;
                        for (uint32_t i = 0; i < count; ++i)
                                (chans[i].dir == DMA2D_CHANNEL_DIRECTION_TX ? tx : rx) = chans[i].chan;
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
                        if (self->_conversion == BlitChain::Conversion::Rgb565ToRgb888) {
                                dma2d_csc_config_t csc = {};
                                csc.tx_csc_option = DMA2D_CSC_TX_RGB565_TO_RGB888;
                                csc.pre_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
                                (void)dma2d_configure_color_space_conversion(tx, &csc);
                        }
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
                        static_cast<BlitChain *>(arg)->_done.releaseFromIsr();
                        return false;
                }
};

Error BlitChain::start(Conversion conversion, size_t maxPieces) {
        if (_hw != nullptr) return Error::InvalidState;
        if (maxPieces == 0) return Error::Invalid;
        // A completion left from a job of an earlier start, cleared.
        while (_done.tryAcquire().isOk()) {}
        _busy = false;
        _count = 0;
        _max = maxPieces;
        _conversion = conversion;
        void *memory = Memory::alloc(MemSpace::Internal, sizeof(Hardware));
        if (memory == nullptr) return Error::NoMem;
        _hw = new (memory) Hardware();
        Hardware &hw = *_hw;
        hw.descriptorBytes = (maxPieces * DescriptorBytes + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign;
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
        hw.config.channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING;
        if (conversion == Conversion::Rgb565ToRgb888) hw.config.channel_flags |= DMA2D_CHANNEL_FUNCTION_FLAG_TX_CSC;
        hw.config.on_job_picked = BlitChainIsr::picked;
        hw.config.user_config = this;
        return Error::Ok;
}

void BlitChain::stop() {
        if (_hw == nullptr) return;
        (void)settle(StopTimeoutMs); // a job under way finishes before its memory goes
        Hardware &hw = *_hw;
        if (hw.pool != nullptr) (void)dma2d_release_pool(hw.pool);
        Memory::free(hw.tx);
        Memory::free(hw.rx);
        Memory::free(hw.job);
        hw.~Hardware();
        Memory::free(_hw);
        _hw = nullptr;
        _count = 0;
        return;
}

Error BlitChain::add(const Picture &from, uint16_t fromX, uint16_t fromY, const Picture &to, uint16_t toX, uint16_t toY,
                     uint16_t width, uint16_t height) {
        if (_hw == nullptr) return Error::InvalidState;
        if (_count == _max) return Error::Full;
        if (width == 0 || height == 0 || !fits(from, fromX, fromY, width, height) || !fits(to, toX, toY, width, height))
                return Error::Invalid;
        const bool widen = _conversion == Conversion::Rgb565ToRgb888;
        if (widen ? (from.bytesPerPixel != 2 || to.bytesPerPixel != 3) : from.bytesPerPixel != to.bytesPerPixel)
                return Error::Invalid;
        // The descriptors of a job under way stay until it's done.
        if (_busy && settle(StopTimeoutMs).isError()) return Error::Timeout;
        Hardware &hw = *_hw;
        // Linked to the next slot; the last one's link is cut in `run()`.
        describe(at(hw.tx, _count), from, fromX, fromY, width, height, false, at(hw.tx, _count + 1));
        describe(at(hw.rx, _count), to, toX, toY, width, height, false, at(hw.rx, _count + 1));
        ++_count;
        return Error::Ok;
}

Error BlitChain::run(unsigned int timeoutMs) {
        if (_hw == nullptr) return Error::InvalidState;
        if (_count == 0) return Error::Ok;
        Hardware           &hw = *_hw;
        dma2d_descriptor_t *lastTx = at(hw.tx, _count - 1);
        lastTx->suc_eof = 1;
        lastTx->next = nullptr;
        at(hw.rx, _count - 1)->next = nullptr;
        (void)esp_cache_msync(hw.tx, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        (void)esp_cache_msync(hw.rx, hw.descriptorBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        _count = 0;
        const esp_err_t err = dma2d_enqueue(hw.pool, &hw.config, hw.job);
        if (err != ESP_OK) return Error::fromEspErr(err);
        _busy = true;
        return settle(timeoutMs);
}

// Waits for the job under way, if any.
Error BlitChain::settle(unsigned int timeoutMs) {
        if (!_busy) return Error::Ok;
        const Error err = _done.acquire(timeoutMs);
        if (err.isOk()) _busy = false;
        return err;
}

BMDS_NAMESPACE_END
