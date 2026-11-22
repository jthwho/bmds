/**
 * @file      uartconsole.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A UART port as a terminal (devplan/io.md §11.1): the UART's LL layer and its own interrupt, as
 * the USB serial console has (usbserialconsole.cpp), not ESP-IDF's driver.
 */

#include <cstdio>
#include <cstring>
#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_clk_tree.h>
#include <esp_intr_alloc.h>
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/gdma.h>
#include <esp_private/periph_ctrl.h>
#include <esp_private/periph_ctrl.h>
#include <hal/dma_types.h>
#include <hal/uart_ll.h>
#include <hal/uhci_ll.h>
#include <soc/soc.h>
#include <hal/uart_periph.h>
#include <bmds/deadline.h>
#include <bmds/flashcache.h>
#include <bmds/interrupt.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/thread.h>

#include "uartconsole.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The receive interrupt at this much in the FIFO: 96 bytes of room, 960 us at 1 Mbaud (the
        // interrupt waits up to 600 us at times: devplan/io.md §14.3).
        constexpr uint32_t RxFullBytes = 32;
        constexpr uint16_t RxIdleSymbols = 10; // or after this long without a byte
        constexpr uint32_t TxEmptyBytes = 16;  // the transmit interrupt below this much
        constexpr uint32_t RxInterrupts = UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_RXFIFO_OVF |
                                          UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR;
        // Received by UHCI: the UART tells only of errors.
        constexpr uint32_t DmaRxInterrupts = UART_INTR_RXFIFO_OVF | UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR;

        // Above this baud, input comes by UHCI's DMA (one port at a time: the chip has one).
        constexpr uint32_t         FifoMaxBaud = 1000000;
        constexpr uint32_t         DmaPieces = 32;    // the ring's descriptors
        constexpr uint32_t         DmaPiece = 2048;   // bytes each: 64 KB, 131 ms at 5 Mbaud
        constexpr uint32_t         PsramLine = 128;   // the L2 cache's line: the ring's sync is whole lines
        constexpr uint32_t         InternalLine = 64; // the L1's, over the descriptors
        constexpr soc_module_clk_t Source = soc_module_clk_t(UART_SCLK_DEFAULT);

        uart_dev_t *hwOf(unsigned port) {
                return UART_LL_GET_HW(port);
        }

} // namespace

// What the interrupt touches while the flash is busy (devplan/io.md §14.3): in internal RAM, as
// is its argument. The input of a pause, moved into the ring when the cache is back.
struct UartStash {
                UartConsole *console = nullptr;
                uart_dev_t  *hw = nullptr;
                uint8_t     *buf = nullptr;
                uint32_t     size = 0;
                uint32_t     head = 0; // counts, `% size`: one interrupt writes and reads them
                uint32_t     tail = 0;
                uint32_t     lost = 0;   // input the stash couldn't take, or the FIFO dropped
                uint32_t     errors = 0; // parity and framing errors seen meanwhile
                bool         deferred = false;
                // UHCI's ring, when the port receives by DMA (a running port's, else null).
                dma_descriptor_t *desc = nullptr; // their uncached alias: the DMA writes them back
                dma_descriptor_t *descBus = nullptr;
                uint8_t          *dmaBuf = nullptr;
                void             *dmaChan = nullptr; // gdma_channel_handle_t
                uint32_t          dmaNext = 0;       // the next descriptor the DMA gives back
                bool              dmaStopped = false;
                bool              uhci = false; // this port has UHCI

                static bool onDmaDone(gdma_channel_handle_t, gdma_event_data_t *, void *arg);
                static bool onDmaError(gdma_channel_handle_t, gdma_event_data_t *, void *arg);
                static void dmaFromIsr(UartStash &s);
};

namespace {

        constexpr unsigned MaxPorts = SOC_UART_NUM;
        // A pause's input kept: 50 ms at the port's baud, a sector erase's typical pause (45 ms on
        // this flash, 400 at most: the cache stays off through one; ESP-IDF's yield comes only
        // between sectors). By DMA, the ring keeps it.
        constexpr uint32_t StashMs = 50;
        constexpr uint32_t MinStash = 256;
        constexpr uint32_t MaxStash = 8192;

        // The ports' stashes, kept (the flash's end reads them); a running port's has a buffer.
        DRAM_ATTR UartStash stashes[MaxPorts] = {};
        bool                hooked = false;
        bool                uhciTaken = false; // the ports start and stop under the I/O lock

        // Work the cache's absence held off: marked, then the flash checked again (had it ended
        // since, its hook already past, the interrupt is raised here). Any interrupt with the cache
        // handles the mark.
        BMDS_ISR_ATTR void deferFromIsr(UartStash &s) {
                __atomic_store_n(&s.deferred, true, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&FlashCache::flashOps, __ATOMIC_SEQ_CST) == 0)
                        uart_ll_ena_intr_mask(s.hw, UART_INTR_TXFIFO_EMPTY);
                return;
        }

        // The end of a flash operation: a port that deferred work has its interrupt raised again (the
        // transmit-empty one: it comes at once), to move its stash and wake its readers.
        BMDS_ISR_ATTR void afterFlash(void *) {
                for (UartStash &s : stashes)
                        if (__atomic_load_n(&s.deferred, __ATOMIC_SEQ_CST))
                                uart_ll_ena_intr_mask(s.hw, UART_INTR_TXFIFO_EMPTY);
                return;
        }

} // namespace

// A descriptor given back (full, or closed by an idle line), or the ring ran into one not yet
// read: the ring drained when the cache may be touched, else at the flash's end.
BMDS_ISR_ATTR bool UartStash::onDmaDone(gdma_channel_handle_t, gdma_event_data_t *, void *arg) {
        dmaFromIsr(*static_cast<UartStash *>(arg));
        return false;
}

BMDS_ISR_ATTR bool UartStash::onDmaError(gdma_channel_handle_t, gdma_event_data_t *, void *arg) {
        UartStash &s = *static_cast<UartStash *>(arg);
        __atomic_store_n(&s.dmaStopped, true, __ATOMIC_SEQ_CST);
        dmaFromIsr(s);
        return false;
}

BMDS_ISR_ATTR void UartStash::dmaFromIsr(UartStash &s) {
        if (FlashCache::enterFromIsr()) {
                s.console->handleDma();
                FlashCache::leaveFromIsr();
                return;
        }
        deferFromIsr(s);
        return;
}

UartConsole::UartConsole(unsigned port, WritableByteSpan txStorage, WritableByteSpan rxStorage)
    : Console(ColorDepth::None), _port(port), _tx(txStorage), _rx(rxStorage) {
        std::snprintf(_name, sizeof(_name), "ttyS%u", port);
        // A serial line carries a device's bytes as they are; a program sets the modes it wants.
        setMode(TermMode::binary());
}

Error UartConsole::start(const IoUartSetup &line) {
        if (_running.load(std::memory_order_acquire)) stop();
        uart_dev_t *hw = hwOf(_port);
        uint32_t    sourceHz = 0;
        if (esp_clk_tree_src_get_freq_hz(Source, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &sourceHz) != ESP_OK)
                return Error::Invalid;
        if (esp_clk_tree_acquire_src(Source, nullptr) != ESP_OK) return Error::Invalid;
        // ESP-IDF's own bring-up order (uart.c: uart_module_enable(), uart_param_config()).
        PERIPH_RCC_ATOMIC() {
                uart_ll_enable_bus_clock(uart_port_t(_port), true);
                uart_ll_reset_register(uart_port_t(_port));
        }
        PERIPH_RCC_ATOMIC() {
                uart_ll_sclk_enable(hw);
        }
        uart_ll_set_mode(hw, UART_MODE_UART);
        uart_ll_set_tx_idle_num(hw, 0);
        bool rateOk = false;
        PERIPH_RCC_ATOMIC() {
                uart_ll_set_sclk(hw, Source);
                rateOk = uart_ll_set_baudrate(hw, line.baud, sourceHz);
        }
        if (!rateOk) {
                PERIPH_RCC_ATOMIC() {
                        uart_ll_sclk_disable(hw);
                }
                PERIPH_RCC_ATOMIC() {
                        uart_ll_enable_bus_clock(uart_port_t(_port), false);
                }
                (void)esp_clk_tree_release_src(Source);
                return Error::Invalid;
        }
        uart_ll_set_data_bit_num(hw, uart_word_length_t(line.dataBits - 5));
        uart_ll_set_parity(hw, line.parity == 'e'   ? UART_PARITY_EVEN
                               : line.parity == 'o' ? UART_PARITY_ODD
                                                    : UART_PARITY_DISABLE);
        uart_ll_set_stop_bits(hw, line.stopBits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1);
        uart_ll_set_hw_flow_ctrl(hw, line.rtsCts ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
                                 SOC_UART_FIFO_LEN - 16);
        uart_ll_set_loop_back(hw, line.loopback);
        uart_ll_set_rxfifo_full_thr(hw, RxFullBytes);
        uart_ll_set_rx_tout(hw, RxIdleSymbols);
        uart_ll_set_txfifo_empty_thr(hw, TxEmptyBytes);
        uart_ll_update(hw);
        uart_ll_rxfifo_rst(hw);
        uart_ll_txfifo_rst(hw);
        uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
        uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
        // Whatever was waiting from before: gone with the port.
        (void)_rx.discardTo(_rx.writePosition());
        // The stash: 50 ms of input at this baud (a byte is ten symbols at most), in internal RAM.
        const uint32_t fifoBaud = line.baud < FifoMaxBaud ? line.baud : FifoMaxBaud;
        uint32_t       stashBytes = fifoBaud / 10 * StashMs / 1000;
        stashBytes = stashBytes < MinStash ? MinStash : stashBytes > MaxStash ? MaxStash : stashBytes;
        void *mem = Memory::alloc(MemSpace::Internal, stashBytes);
        if (mem == nullptr) {
                stop();
                return Error::NoMem;
        }
        _stash = &stashes[_port];
        *_stash = UartStash();
        _stash->console = this;
        _stash->hw = hw;
        _stash->buf = static_cast<uint8_t *>(mem);
        _stash->size = stashBytes;
        if (!hooked) {
                hooked = true;
                if (FlashCache::addAfterHook(afterFlash, nullptr).isError())
                        bmdsWarn("io: no flash hook: a serial port's input after a flash write waits for more");
        }
        // Fast input by DMA: the interrupt can't always drain the FIFO in time above FifoMaxBaud.
        if (line.baud > FifoMaxBaud) {
                if (uhciTaken) {
                        if (!line.rtsCts)
                                bmdsWarn("io: ttyS%u: input above %u baud without rts/cts may drop bytes (UHCI is "
                                         "another port's)",
                                         _port, unsigned(FifoMaxBaud));
                } else if (const Error dma = startDma(); dma.isError()) {
                        stop();
                        return dma;
                }
        }
        // Cache-safe: it runs while the flash is busy, then only into the stash (isr()).
        intr_handle_t   handle = nullptr;
        const esp_err_t err =
                esp_intr_alloc(uart_periph_signal[_port].irq, ESP_INTR_FLAG_IRAM, &UartConsole::isr, _stash, &handle);
        if (err != ESP_OK) {
                stop();
                return Error::NoMem;
        }
        _intr = handle;
        _running.store(true, std::memory_order_release);
        {
                SpinLock::Locker lock(_intLock);
                uart_ll_ena_intr_mask(hw, _stash->desc != nullptr ? DmaRxInterrupts : RxInterrupts);
        }
        if (!_tx.isEmpty()) enableTxInterrupt();
        return Error::Ok;
}

// UHCI between the UART and a GDMA channel, its input into a ring of descriptors that never ends:
// the DMA gives each back full or closed by an idle line (devplan/io.md §14.3).
Error UartConsole::startDma() {
        UartStash   &s = *_stash;
        const size_t descBytes = (DmaPieces * sizeof(dma_descriptor_t) + InternalLine - 1) & ~(InternalLine - 1);
        auto        *descBus =
                static_cast<dma_descriptor_t *>(Memory::allocAligned(MemSpace::InternalDma, descBytes, InternalLine));
        auto *buf = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, DmaPieces * DmaPiece, PsramLine));
        if (descBus == nullptr || buf == nullptr) {
                Memory::free(descBus);
                Memory::free(buf);
                return Error::NoMem;
        }
        // No dirty line over either (one evicted would land over what the DMA wrote); the
        // descriptors then written past the cache. The ring in pieces: a sync masks interrupts.
        (void)esp_cache_msync(descBus, descBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        for (uint32_t at = 0; at < DmaPieces * DmaPiece; at += DmaPiece)
                (void)esp_cache_msync(buf + at, DmaPiece,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        auto *desc = reinterpret_cast<dma_descriptor_t *>(uintptr_t(descBus) + SOC_NON_CACHEABLE_OFFSET_SRAM);
        for (uint32_t i = 0; i < DmaPieces; ++i) {
                desc[i].dw0.size = DmaPiece;
                desc[i].dw0.length = 0;
                desc[i].dw0.err_eof = 0;
                desc[i].dw0.suc_eof = 0;
                desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
                desc[i].buffer = buf + i * DmaPiece;
                desc[i].next = &descBus[(i + 1) % DmaPieces];
        }
        s.desc = desc;
        s.descBus = descBus;
        s.dmaBuf = buf;
        s.dmaNext = 0;
        s.dmaStopped = false;

        gdma_channel_handle_t       chan = nullptr;
        gdma_channel_alloc_config_t alloc = {};
        alloc.flags.isr_cache_safe = 1; // it runs while the flash is busy, then defers (dmaFromIsr())
        if (gdma_new_ahb_channel(&alloc, nullptr, &chan) != ESP_OK) {
                stopDma();
                return Error::Busy;
        }
        s.dmaChan = chan;
        gdma_transfer_config_t transfer = {};
        transfer.max_data_burst_size = 32;
        transfer.access_ext_mem = true;
        gdma_strategy_config_t strategy = {};
        strategy.owner_check = true; // a descriptor not yet read stops the ring (onDmaError())
        gdma_rx_event_callbacks_t events = {};
        events.on_recv_done = &UartStash::onDmaDone;
        events.on_descr_err = &UartStash::onDmaError;
        if (gdma_connect(chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0)) != ESP_OK ||
            gdma_config_transfer(chan, &transfer) != ESP_OK || gdma_apply_strategy(chan, &strategy) != ESP_OK ||
            gdma_register_rx_event_callbacks(chan, &events, &s) != ESP_OK) {
                stopDma();
                return Error::Invalid;
        }
        uhciTaken = true;
        s.uhci = true;
        PERIPH_RCC_ATOMIC() {
                uhci_ll_enable_bus_clock(0, true);
                uhci_ll_reset_register(0);
        }
        uhci_dev_t *uhci = UHCI_LL_GET_HW(0);
        uhci_ll_init(uhci);
        uhci_ll_attach_uart_port(uhci, int(_port));
        uhci_seper_chr_t noSeparator = {};
        uhci_ll_set_seper_chr(uhci, &noSeparator); // ESP-IDF: else UHCI loses data
        uhci_ll_rx_enable_eof_modes(uhci, UHCI_RX_IDLE_EOF, true);
        // Idle after ten symbols' time, as the FIFO's timeout (bit times).
        uart_ll_set_rx_idle_thr(_stash->hw, RxIdleSymbols * 10u);
        (void)gdma_start(chan, intptr_t(descBus));
        return Error::Ok;
}

// The ring stopped and given back; UHCI free for another port.
void UartConsole::stopDma() {
        UartStash &s = *_stash;
        if (s.dmaChan != nullptr) {
                auto chan = static_cast<gdma_channel_handle_t>(s.dmaChan);
                (void)gdma_stop(chan);
                (void)gdma_disconnect(chan);
                (void)gdma_del_channel(chan);
                s.dmaChan = nullptr;
        }
        if (s.uhci) {
                PERIPH_RCC_ATOMIC() {
                        uhci_ll_enable_bus_clock(0, false);
                }
                uhciTaken = false;
                s.uhci = false;
        }
        Memory::free(s.descBus);
        Memory::free(s.dmaBuf);
        s.desc = nullptr;
        s.descBus = nullptr;
        s.dmaBuf = nullptr;
        return;
}

void UartConsole::stop() {
        uart_dev_t *hw = hwOf(_port);
        {
                SpinLock::Locker lock(_intLock);
                uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
        }
        if (_intr != nullptr) {
                (void)esp_intr_free(static_cast<intr_handle_t>(_intr));
                _intr = nullptr;
        }
        if (_stash != nullptr && (_stash->desc != nullptr || _stash->dmaChan != nullptr)) stopDma();
        if (_stash != nullptr) {
                // The interrupt gone; the flash's end told nothing is deferred, and waited for if
                // it read the mark before (the port's clock about to stop under its write).
                __atomic_store_n(&_stash->deferred, false, __ATOMIC_SEQ_CST);
                FlashCache::waitHooks();
                Memory::free(_stash->buf);
                *_stash = UartStash();
                _stash = nullptr;
        }
        const bool was = _running.exchange(false, std::memory_order_acq_rel);
        if (was) {
                PERIPH_RCC_ATOMIC() {
                        uart_ll_sclk_disable(hw);
                }
                PERIPH_RCC_ATOMIC() {
                        uart_ll_enable_bus_clock(uart_port_t(_port), false);
                }
                (void)esp_clk_tree_release_src(Source);
        }
        // A reader or a writer waiting: told the port went.
        _rxReady.release();
        _txSpace.release();
        return;
}

// In IRAM, its argument the stash (internal RAM): while the flash is busy, the cache and PSRAM
// (this console) are out of reach, so then it only moves the FIFO into the stash and leaves the
// rest for when the cache is back (the flash's end raises it again: afterFlash()).
BMDS_ISR_ATTR void UartConsole::isr(void *arg) {
        UartStash &s = *static_cast<UartStash *>(arg);
        if (FlashCache::enterFromIsr()) {
                s.console->handleInterrupt();
                FlashCache::leaveFromIsr();
                return;
        }
        uart_dev_t    *hw = s.hw;
        const uint32_t status = uart_ll_get_intsts_mask(hw);
        if (status & (UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR)) ++s.errors;
        if (status & RxInterrupts) {
                // By DMA, the FIFO is UHCI's.
                for (uint32_t left = s.desc != nullptr ? 0 : uart_ll_get_rxfifo_len(hw); left > 0;
                     left = uart_ll_get_rxfifo_len(hw)) {
                        const uint32_t room = s.size - (s.head - s.tail);
                        if (room == 0) { // full: the rest dropped, counted
                                uint8_t        drop[16];
                                const uint32_t n = left < sizeof(drop) ? left : uint32_t(sizeof(drop));
                                uart_ll_read_rxfifo(hw, drop, n);
                                s.lost += n;
                                continue;
                        }
                        // Up to the end of the stash's buffer at a time.
                        const uint32_t at = s.head % s.size;
                        uint32_t       n = s.size - at;
                        if (n > room) n = room;
                        if (n > left) n = left;
                        uart_ll_read_rxfifo(hw, s.buf + at, n);
                        s.head += n;
                }
                if (status & UART_INTR_RXFIFO_OVF) {
                        ++s.lost;
                        uart_ll_rxfifo_rst(hw);
                }
                uart_ll_clr_intsts_mask(hw, status & RxInterrupts);
        }
        // The transmit ring is in PSRAM: its interrupt waits (masked) for the cache.
        if (status & UART_INTR_TXFIFO_EMPTY) {
                uart_ll_disable_intr_mask(hw, UART_INTR_TXFIFO_EMPTY);
                uart_ll_clr_intsts_mask(hw, UART_INTR_TXFIFO_EMPTY);
        }
        deferFromIsr(s);
        return;
}

// Input into the ring: Ctrl-C, Ctrl-\ and Ctrl-Z signal when the mode says so (as the USB
// console's: the input before them flushed); what the ring can't take dropped and counted.
void UartConsole::accept(uint8_t *buf, size_t n, uint32_t &sent) {
        if (signalsEnabled()) {
                size_t kept = 0;
                bool   any = false;
                for (size_t i = 0; i < n; ++i) {
                        if (const int sig = signalFor(buf[i]); sig != 0) {
                                sent |= 1u << (sig - 1);
                                kept = 0;
                                any = true;
                        } else {
                                buf[kept++] = buf[i];
                        }
                }
                if (any) {
                        _rxFlushTo.store(_rx.writePosition(), std::memory_order_relaxed);
                        _rxFlush.store(true, std::memory_order_release);
                }
                n = kept;
        }
        const size_t pushed = _rx.push(ByteSpan(buf, n));
        if (pushed < n) _rxDropped.fetch_add(uint32_t(n - pushed), std::memory_order_relaxed);
        return;
}

// Input delivered: the signals its characters raised sent, readers woken.
void UartConsole::delivered(uint32_t sent) {
        if ((sent & (1u << (SignalInterrupt - 1))) != 0) sendSignalFromIsr(SignalInterrupt);
        if ((sent & (1u << (SignalQuit - 1))) != 0) sendSignalFromIsr(SignalQuit);
        if ((sent & (1u << (SignalSuspend - 1))) != 0) sendSignalFromIsr(SignalSuspend);
        _rxReady.releaseFromIsr();
        inputArrivedFromIsr();
        return;
}

// The DMA's interrupt, the cache there: the ring drained (both interrupts may, so under the lock).
void UartConsole::handleDma() {
        uint32_t sent = 0;
        _intLock.lockFromIsr();
        drainDma(sent);
        _intLock.unlockFromIsr();
        delivered(sent);
        return;
}

// The descriptors the DMA gave back, in order, into the ring and given back to it; a ring that
// stopped (it met one not yet read) started again where it stopped. The lock held.
void UartConsole::drainDma(uint32_t &sent) {
        UartStash &s = *_stash;
        for (uint32_t i = 0; i < DmaPieces; ++i) {
                dma_descriptor_t &d = s.desc[s.dmaNext];
                if (d.dw0.owner != DMA_DESCRIPTOR_BUFFER_OWNER_CPU) break;
                const uint32_t n = d.dw0.length;
                const uint8_t *src = s.dmaBuf + s.dmaNext * DmaPiece;
                if (n != 0) {
                        (void)esp_cache_msync(const_cast<uint8_t *>(src), (n + PsramLine - 1) & ~(PsramLine - 1),
                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        if (!signalsEnabled()) {
                                const size_t pushed = _rx.push(ByteSpan(src, n));
                                if (pushed < n) _rxDropped.fetch_add(uint32_t(n - pushed), std::memory_order_relaxed);
                        } else {
                                // Copied out to be filtered: the ring's lines are never written by the CPU.
                                uint8_t chunk[64];
                                for (uint32_t at = 0; at < n;) {
                                        const uint32_t m = n - at < sizeof(chunk) ? n - at : uint32_t(sizeof(chunk));
                                        std::memcpy(chunk, src + at, m);
                                        accept(chunk, m, sent);
                                        at += m;
                                }
                        }
                }
                d.dw0.length = 0;
                d.dw0.err_eof = 0;
                d.dw0.suc_eof = 0;
                d.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
                s.dmaNext = (s.dmaNext + 1) % DmaPieces;
        }
        if (__atomic_exchange_n(&s.dmaStopped, false, __ATOMIC_SEQ_CST)) {
                // Input meanwhile waited in the FIFO (or overflowed it: counted by its interrupt).
                auto chan = static_cast<gdma_channel_handle_t>(s.dmaChan);
                (void)gdma_reset(chan);
                (void)gdma_start(chan, intptr_t(&s.descBus[s.dmaNext]));
        }
        return;
}

// What a pause left in the stash, into the ring, in order (before the FIFO's newer input).
void UartConsole::drainStash(uint32_t &sent) {
        UartStash &s = *_stash;
        while (s.tail != s.head) {
                const uint32_t at = s.tail % s.size;
                uint32_t       n = s.size - at;
                if (n > s.head - s.tail) n = s.head - s.tail;
                accept(s.buf + at, n, sent);
                s.tail += n;
        }
        if (s.lost != 0) {
                _rxDropped.fetch_add(s.lost, std::memory_order_relaxed);
                s.lost = 0;
        }
        if (s.errors != 0) {
                _lineErrors.fetch_add(s.errors, std::memory_order_relaxed);
                s.errors = 0;
        }
        return;
}

void UartConsole::handleInterrupt() {
        uart_dev_t    *hw = hwOf(_port);
        const uint32_t status = uart_ll_get_intsts_mask(hw);
        if (status & (UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR)) _lineErrors.fetch_add(1, std::memory_order_relaxed);
        // A pause before: its input first, the transmit interrupt back if there is more to send.
        if (_stash->deferred || _stash->head != _stash->tail) {
                _stash->deferred = false;
                uint32_t sent = 0;
                drainStash(sent);
                if (_stash->desc != nullptr) {
                        _intLock.lockFromIsr();
                        drainDma(sent);
                        _intLock.unlockFromIsr();
                }
                delivered(sent);
                _intLock.lockFromIsr();
                if (!_tx.isEmpty()) uart_ll_ena_intr_mask(hw, UART_INTR_TXFIFO_EMPTY);
                _intLock.unlockFromIsr();
        }
        if ((status & RxInterrupts) && _stash->desc != nullptr) {
                // By DMA: only the FIFO's overflow (the ring stalled) and errors come here.
                if (status & UART_INTR_RXFIFO_OVF) {
                        _rxDropped.fetch_add(1, std::memory_order_relaxed);
                        uart_ll_rxfifo_rst(hw);
                }
                uart_ll_clr_intsts_mask(hw, status & RxInterrupts);
        } else if (status & RxInterrupts) {
                // The FIFO drained at once, in pieces (accept()).
                uint8_t  buf[64];
                uint32_t sent = 0; // bit sig - 1
                // At most a FIFO's worth a pass: bytes arriving meanwhile raise the interrupt again.
                uint32_t budget = SOC_UART_FIFO_LEN;
                for (uint32_t left = uart_ll_get_rxfifo_len(hw); left > 0 && budget > 0;
                     left = uart_ll_get_rxfifo_len(hw)) {
                        size_t n = left < sizeof(buf) ? left : sizeof(buf);
                        if (n > budget) n = budget;
                        budget -= uint32_t(n);
                        uart_ll_read_rxfifo(hw, buf, uint32_t(n));
                        accept(buf, n, sent);
                }
                if (status & UART_INTR_RXFIFO_OVF) {
                        // The FIFO overflowed before the interrupt ran: bytes the hardware dropped.
                        _rxDropped.fetch_add(1, std::memory_order_relaxed);
                        uart_ll_rxfifo_rst(hw);
                }
                uart_ll_clr_intsts_mask(hw, status & RxInterrupts);
                delivered(sent);
        }
        if (status & UART_INTR_TXFIFO_EMPTY) {
                uart_ll_clr_intsts_mask(hw, UART_INTR_TXFIFO_EMPTY);
                // uart_ll_get_txfifo_len() is the room left in the FIFO, not what it holds.
                uint32_t room = uart_ll_get_txfifo_len(hw);
                while (room > 0) {
                        const ByteSpan chunk = _tx.readable();
                        if (chunk.isEmpty()) break;
                        const uint32_t n = chunk.size() < room ? uint32_t(chunk.size()) : room;
                        uart_ll_write_txfifo(hw, chunk.data(), n);
                        _tx.consume(n);
                        room -= n;
                }
                _intLock.lockFromIsr();
                if (_tx.isEmpty()) uart_ll_disable_intr_mask(hw, UART_INTR_TXFIFO_EMPTY);
                _intLock.unlockFromIsr();
                _txSpace.releaseFromIsr();
        }
        return;
}

void UartConsole::enableTxInterrupt() {
        SpinLock::Locker lock(_intLock);
        if (_running.load(std::memory_order_relaxed)) uart_ll_ena_intr_mask(hwOf(_port), UART_INTR_TXFIFO_EMPTY);
        return;
}

Result<size_t> UartConsole::tryReadInput(WritableByteSpan buf) {
        if (_rxFlush.exchange(false, std::memory_order_acquire))
                _rx.discardTo(_rxFlushTo.load(std::memory_order_relaxed));
        const size_t n = _rx.pop(buf);
        if (n == 0)
                return makeError<size_t>(_running.load(std::memory_order_acquire) ? Error::WouldBlock : Error::NoMedia);
        return makeResult(n);
}

Result<size_t> UartConsole::readInput(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        Deadline   deadline(timeoutMs);
        Interrupt *interrupt = Interrupt::current();
        for (;;) {
                if (interrupt != nullptr && interrupt->isRaised()) return makeError<size_t>(Error::Interrupted);
                Result<size_t> r = tryReadInput(buf);
                if (r.isOk() || r.error() != Error::WouldBlock) return r; // input, or the port went
                const Error err = Interrupt::wait(interrupt, _rxReady, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

size_t UartConsole::writeRoom() {
        // Stopped, a write says so; else what fits even if every byte were \n.
        if (!_running.load(std::memory_order_acquire)) return RoomUnknown;
        const size_t free = _tx.freeSpace();
        return mode().nlToCrNl ? free / 2 : free;
}

Result<size_t> UartConsole::write(ByteSpan data, unsigned int timeoutMs) {
        if (data.isEmpty()) return makeResult(size_t(0));
        Deadline deadline(timeoutMs);
        if (_txMutex.lock(deadline).isError()) return makeError<size_t>(Error::Timeout);
        const bool translate = mode().nlToCrNl;
        size_t     done = 0;
        bool       crSent = false;
        Error      err;
        while (done < data.size()) {
                if (!_running.load(std::memory_order_acquire)) {
                        err = Error::NoMedia;
                        break;
                }
                // Into the ring, `\n` as CR LF when the mode says so.
                WritableByteSpan dst = _tx.writable();
                size_t           n = 0;
                while (n < dst.size() && done < data.size()) {
                        const uint8_t b = data[done];
                        if (translate && b == '\n' && !crSent) {
                                dst[n++] = '\r';
                                crSent = true;
                                continue;
                        }
                        dst[n++] = b;
                        crSent = false;
                        ++done;
                }
                if (n > 0) {
                        _tx.produce(n);
                        enableTxInterrupt();
                        continue;
                }
                // The ring is full: wait for the interrupt to drain some (flow control may hold it).
                if (_txSpace.acquire(deadline).isError()) {
                        err = Error::Timeout;
                        break;
                }
        }
        _txMutex.unlock();
        if (done == 0 && err.isError()) return makeError<size_t>(err);
        return makeResult(done);
}

Error UartConsole::flush(unsigned int timeoutMs) {
        Deadline deadline(timeoutMs);
        if (_txMutex.lock(deadline).isError()) return Error::Timeout;
        Error err;
        while (_running.load(std::memory_order_acquire) && (!_tx.isEmpty() || !uart_ll_is_tx_idle(hwOf(_port)))) {
                enableTxInterrupt();
                if (!_tx.isEmpty()) {
                        if (_txSpace.acquire(deadline).isError()) {
                                err = Error::Timeout;
                                break;
                        }
                } else if (deadline.isExpired()) {
                        err = Error::Timeout;
                        break;
                } else {
                        Thread::sleep(1); // the FIFO's last bytes on the wire (a slow line: a while)
                }
        }
        _txMutex.unlock();
        return err;
}

BMDS_NAMESPACE_END
