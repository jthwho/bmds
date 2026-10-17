/**
 * @file      blitchain.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Chained 2D-DMA rectangle copies: 2D descriptors (a block at an offset in a
 * picture, as IDF's 2D-DMA window test), several to a job, the transmit
 * channel's converter for RGB565 sources; fills as one line sent per row.
 */

#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_private/dma2d.h>
#include <hal/dma2d_types.h>
#include <soc/dma2d_channel.h>
#include <bmds/blitchain.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/system.h>

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
                size_t               txBytes = 0; // each chain's bytes, rounded to the cache line
                size_t               rxBytes = 0;
};

struct BlitChainIsr {
                static bool picked(uint32_t count, const dma2d_trans_channel_info_t *chans, void *arg) {
                        auto                  *self = static_cast<BlitChain *>(arg);
                        BlitChain::Hardware   &hw = *self->_hw;
                        self->_picked.store(true, std::memory_order_release);
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
                        callbacks.on_desc_empty = overrun;
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

                // The data outran the receive descriptors: a job whose two sides don't match, which may
                // never reach its end of frame. Kept for recover()'s message.
                static bool overrun(dma2d_channel_handle_t, dma2d_event_data_t *, void *arg) {
                        static_cast<BlitChain *>(arg)->_overrun.store(true, std::memory_order_relaxed);
                        return false;
                }
};

Error BlitChain::start(Conversion conversion, size_t maxPieces, size_t maxRows) {
        if (_hw != nullptr) return Error::InvalidState;
        if (maxPieces == 0) return Error::Invalid;
        // A completion left from a job of an earlier start, cleared.
        while (_done.tryAcquire().isOk()) {}
        _busy = false;
        _stuck = 0;
        _jobs = 0;
        _txCount = 0;
        _rxCount = 0;
        _max = maxPieces;
        _maxTx = maxPieces + maxRows;
        _conversion = conversion;
        void *memory = Memory::alloc(MemSpace::Internal, sizeof(Hardware));
        if (memory == nullptr) return Error::NoMem;
        _hw = new (memory) Hardware();
        Hardware &hw = *_hw;
        hw.txBytes = (_maxTx * DescriptorBytes + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign;
        hw.rxBytes = (maxPieces * DescriptorBytes + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign;
        hw.tx = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.txBytes, DescriptorAlign));
        hw.rx = static_cast<dma2d_descriptor_t *>(
                Memory::allocAligned(MemSpace::InternalDma, hw.rxBytes, DescriptorAlign));
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
        // A job under way finishes, or is ended, before its memory goes. Up to a second: past
        // StuckMs, so wait() ends a stuck job, and a job queued behind another chain's stuck one has
        // its turn once that one is ended in turn.
        for (int i = 0; _busy && i < 10; ++i) (void)wait(StopTimeoutMs);
        if (_busy) {
                // Still in the 2D-DMA's queue: when it is given channels its callback reads _hw and the
                // descriptors, so everything stays allocated (a leak, said here) rather than freed under it.
                bmdsWarn("2D-DMA: stopped with a job still queued after 1 s: its memory is left allocated");
                return;
        }
        Hardware &hw = *_hw;
        if (hw.pool != nullptr) (void)dma2d_release_pool(hw.pool);
        Memory::free(hw.tx);
        Memory::free(hw.rx);
        Memory::free(hw.job);
        hw.~Hardware();
        Memory::free(_hw);
        _hw = nullptr;
        _txCount = 0;
        _rxCount = 0;
        return;
}

Error BlitChain::add(const Picture &from, uint16_t fromX, uint16_t fromY, const Picture &to, uint16_t toX, uint16_t toY,
                     uint16_t width, uint16_t height) {
        if (_hw == nullptr) return Error::InvalidState;
        if (_rxCount == _max || _txCount == _maxTx) return Error::Full;
        if (width == 0 || height == 0 || !fits(from, fromX, fromY, width, height) || !fits(to, toX, toY, width, height))
                return Error::Invalid;
        const bool widen = _conversion == Conversion::Rgb565ToRgb888;
        if (widen ? (from.bytesPerPixel != 2 || to.bytesPerPixel != 3) : from.bytesPerPixel != to.bytesPerPixel)
                return Error::Invalid;
        // The descriptors of a job under way stay until it's done.
        if (_busy && wait(StopTimeoutMs).isError()) return Error::Timeout;
        Hardware &hw = *_hw;
        // Linked to the next slot; the last ones' links are cut in `run()`.
        describe(at(hw.tx, _txCount), from, fromX, fromY, width, height, false, at(hw.tx, _txCount + 1));
        describe(at(hw.rx, _rxCount), to, toX, toY, width, height, false, at(hw.rx, _rxCount + 1));
        ++_txCount;
        ++_rxCount;
        return Error::Ok;
}

Error BlitChain::addRepeated(const Picture &from, uint16_t fromX, uint16_t fromY, uint16_t repeat, uint16_t first,
                             const Picture &to, uint16_t toX, uint16_t toY, uint16_t width, uint16_t height) {
        if (repeat <= 1 && first == 0) return add(from, fromX, fromY, to, toX, toY, width, height);
        if (_hw == nullptr) return Error::InvalidState;
        if (repeat == 0 || first >= repeat || width == 0 || height == 0) return Error::Invalid;
        if (_rxCount == _max || _txCount + height > _maxTx) return Error::Full;
        const uint32_t rows = (uint32_t(first) + height - 1) / repeat + 1; // of the source
        if (rows > MaxField || !fits(from, fromX, fromY, width, uint16_t(rows)) || !fits(to, toX, toY, width, height))
                return Error::Invalid;
        const bool widen = _conversion == Conversion::Rgb565ToRgb888;
        if (widen ? (from.bytesPerPixel != 2 || to.bytesPerPixel != 3) : from.bytesPerPixel != to.bytesPerPixel)
                return Error::Invalid;
        if (_busy && wait(StopTimeoutMs).isError()) return Error::Timeout;
        Hardware &hw = *_hw;
        for (uint32_t i = 0; i < height; ++i, ++_txCount)
                describe(at(hw.tx, _txCount), from, fromX, uint16_t(fromY + (first + i) / repeat), width, 1, false,
                         at(hw.tx, _txCount + 1));
        describe(at(hw.rx, _rxCount), to, toX, toY, width, height, false, at(hw.rx, _rxCount + 1));
        ++_rxCount;
        return Error::Ok;
}

Error BlitChain::addFill(const uint8_t *line, const Picture &to, uint16_t toX, uint16_t toY, uint16_t width,
                         uint16_t height) {
        return addFill(line, 1, to, toX, toY, width, height);
}

Error BlitChain::addFill(const uint8_t *line, uint16_t lineRows, const Picture &to, uint16_t toX, uint16_t toY,
                         uint16_t width, uint16_t height) {
        if (_hw == nullptr) return Error::InvalidState;
        if (lineRows == 0) return Error::Invalid;
        const size_t descriptors = (size_t(height) + lineRows - 1) / lineRows;
        if (_rxCount == _max || _txCount + descriptors > _maxTx) return Error::Full;
        const Picture rows{line, width, lineRows, to.bytesPerPixel};
        if (_conversion != Conversion::None || width == 0 || height == 0 || line == nullptr ||
            !fits(to, toX, toY, width, height))
                return Error::Invalid;
        if (_busy && wait(StopTimeoutMs).isError()) return Error::Timeout;
        Hardware &hw = *_hw;
        for (uint32_t r = 0; r < height; r += lineRows, ++_txCount) {
                const auto n = static_cast<uint16_t>(height - r < lineRows ? height - r : lineRows);
                describe(at(hw.tx, _txCount), rows, 0, 0, width, n, false, at(hw.tx, _txCount + 1));
        }
        describe(at(hw.rx, _rxCount), to, toX, toY, width, height, false, at(hw.rx, _rxCount + 1));
        ++_rxCount;
        return Error::Ok;
}

Error BlitChain::run(unsigned int timeoutMs) {
        const Error err = submit();
        return err.isError() ? err : wait(timeoutMs);
}

Error BlitChain::submit() {
        if (_hw == nullptr) return Error::InvalidState;
        if (_rxCount == 0) return Error::Ok;
        Hardware           &hw = *_hw;
        dma2d_descriptor_t *lastTx = at(hw.tx, _txCount - 1);
        lastTx->suc_eof = 1;
        lastTx->next = nullptr;
        at(hw.rx, _rxCount - 1)->next = nullptr;
        (void)esp_cache_msync(hw.tx,
                              (_txCount * DescriptorBytes + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        (void)esp_cache_msync(hw.rx,
                              (_rxCount * DescriptorBytes + DescriptorAlign - 1) / DescriptorAlign * DescriptorAlign,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        _jobPieces = static_cast<uint16_t>(_rxCount);
        _jobRows = static_cast<uint16_t>(_txCount);
        _txCount = 0;
        _rxCount = 0;
        _picked.store(false, std::memory_order_relaxed);
        _overrun.store(false, std::memory_order_relaxed);
        _submittedUs = System::uptimeUs();
        const esp_err_t err = dma2d_enqueue(hw.pool, &hw.config, hw.job);
        if (err != ESP_OK) return Error::fromEspErr(err);
        _busy = true;
        ++_jobs;
        return Error::Ok;
}

// Waits for the job under way, if any; one stuck past StuckMs is ended (recover()).
Error BlitChain::wait(unsigned int timeoutMs) {
        if (!_busy) return Error::Ok;
        const Error err = _done.acquire(timeoutMs);
        if (err.isOk()) {
                _busy = false;
                return err;
        }
        if (System::uptimeUs() - _submittedUs >= uint64_t(StuckMs) * 1000u) return recover();
        return err; // only the caller's time ran out: the job may yet finish
}

// The job under way has had far longer than any copy takes. If it has its channels, they are
// stopped and given back (dma2d_force_end()); then the chain is free for the next job. A job never
// given channels (another job holding them all) can't be taken back out of the 2D-DMA's queue: it
// stays under way, and the log says whose fault it isn't.
Error BlitChain::recover() {
        if (_done.tryAcquire().isOk()) { // finished just now after all
                _busy = false;
                return Error::Ok;
        }
        const bool picked = _picked.load(std::memory_order_acquire);
        const bool overrun = _overrun.load(std::memory_order_relaxed);
        const auto ageMs = static_cast<unsigned>((System::uptimeUs() - _submittedUs) / 1000u);
        ++_stuck;
        const bool tell = _stuck <= 3 || (_stuck & (_stuck - 1)) == 0; // the first few, then sparser
        if (!picked) {
                if (tell)
                        bmdsWarn("2D-DMA: a job of %u pieces (%u rows) waited %u ms for channels: others hold them "
                                 "(%u times)",
                                 unsigned(_jobPieces), unsigned(_jobRows), ageMs, unsigned(_stuck));
                return Error::Timeout;
        }
        // In flight: stopped, its channels freed. If its end of frame came between the check above
        // and now, the force finds it not in flight; the completion is collected below either way.
        // (A narrow window remains in which a job finishing just then and another picking up its
        // channels would be the one stopped; that job's own wait recovers it in turn.)
        bool            yield = false;
        const esp_err_t err = dma2d_force_end(_hw->job, &yield);
        while (_done.tryAcquire().isOk()) {}
        _busy = false;
        if (tell) {
                const dma2d_descriptor_t *first = _hw->rx;
                bmdsWarn("2D-DMA: a job of %u pieces (%u rows) didn't finish in %u ms%s; ended (%s); first piece "
                         "%ux%u at %u,%u of %ux%u (%u times)",
                         unsigned(_jobPieces), unsigned(_jobRows), ageMs,
                         overrun ? ", its data past its receive descriptors" : "",
                         err == ESP_OK ? "stopped" : esp_err_to_name(err), unsigned(first->hb_length),
                         unsigned(first->vb_size), unsigned(first->x), unsigned(first->y), unsigned(first->ha_length),
                         unsigned(first->va_size), unsigned(_stuck));
        }
        return Error::Timeout;
}

BMDS_NAMESPACE_END
