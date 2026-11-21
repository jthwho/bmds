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
#include <esp_clk_tree.h>
#include <esp_intr_alloc.h>
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/uart_share_hw_ctrl.h>
#include <hal/uart_ll.h>
#include <soc/uart_periph.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/thread.h>

#include "uartconsole.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t RxFullBytes = 64;   // the receive interrupt at this much in the FIFO
        constexpr uint16_t RxIdleSymbols = 10; // or after this long without a byte
        constexpr uint32_t TxEmptyBytes = 16;  // the transmit interrupt below this much
        constexpr uint32_t RxInterrupts = UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_RXFIFO_OVF |
                                          UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR;
        constexpr soc_module_clk_t Source = soc_module_clk_t(UART_SCLK_DEFAULT);

        uart_dev_t *hwOf(unsigned port) {
                return UART_LL_GET_HW(port);
        }

} // namespace

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
        if (esp_clk_tree_enable_src(Source, true) != ESP_OK) return Error::Invalid;
        // ESP-IDF's own bring-up order (uart.c: uart_module_enable(), uart_param_config()).
        HP_UART_BUS_CLK_ATOMIC() {
                uart_ll_enable_bus_clock(uart_port_t(_port), true);
                uart_ll_reset_register(uart_port_t(_port));
        }
        HP_UART_SRC_CLK_ATOMIC() {
                uart_ll_sclk_enable(hw);
        }
        uart_ll_set_mode(hw, UART_MODE_UART);
        uart_ll_set_tx_idle_num(hw, 0);
        bool rateOk = false;
        HP_UART_SRC_CLK_ATOMIC() {
                uart_ll_set_sclk(hw, Source);
                rateOk = uart_ll_set_baudrate(hw, line.baud, sourceHz);
        }
        if (!rateOk) {
                HP_UART_SRC_CLK_ATOMIC() {
                        uart_ll_sclk_disable(hw);
                }
                HP_UART_BUS_CLK_ATOMIC() {
                        uart_ll_enable_bus_clock(uart_port_t(_port), false);
                }
                (void)esp_clk_tree_enable_src(Source, false);
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
        intr_handle_t   handle = nullptr;
        const esp_err_t err = esp_intr_alloc(uart_periph_signal[_port].irq, 0, &UartConsole::isr, this, &handle);
        if (err != ESP_OK) {
                stop();
                return Error::NoMem;
        }
        _intr = handle;
        _running.store(true, std::memory_order_release);
        {
                SpinLock::Locker lock(_intLock);
                uart_ll_ena_intr_mask(hw, RxInterrupts);
        }
        if (!_tx.isEmpty()) enableTxInterrupt();
        return Error::Ok;
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
        const bool was = _running.exchange(false, std::memory_order_acq_rel);
        if (was) {
                HP_UART_SRC_CLK_ATOMIC() {
                        uart_ll_sclk_disable(hw);
                }
                HP_UART_BUS_CLK_ATOMIC() {
                        uart_ll_enable_bus_clock(uart_port_t(_port), false);
                }
                (void)esp_clk_tree_enable_src(Source, false);
        }
        // A reader or a writer waiting: told the port went.
        _rxReady.release();
        _txSpace.release();
        return;
}

void UartConsole::isr(void *arg) {
        static_cast<UartConsole *>(arg)->handleInterrupt();
        return;
}

void UartConsole::handleInterrupt() {
        uart_dev_t    *hw = hwOf(_port);
        const uint32_t status = uart_ll_get_intsts_mask(hw);
        if (status & (UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR)) _lineErrors.fetch_add(1, std::memory_order_relaxed);
        if (status & RxInterrupts) {
                // The FIFO drained at once, in pieces; input the ring can't take is dropped and
                // counted. Ctrl-C, Ctrl-\ and Ctrl-Z signal when the mode says so (as the USB
                // console's: the input before them flushed).
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
                        if (signalsEnabled()) {
                                size_t kept = 0;
                                for (size_t i = 0; i < n; ++i) {
                                        if (const int sig = signalFor(buf[i]); sig != 0) {
                                                sent |= 1u << (sig - 1);
                                                kept = 0;
                                        } else {
                                                buf[kept++] = buf[i];
                                        }
                                }
                                if (sent != 0) {
                                        _rxFlushTo.store(_rx.writePosition(), std::memory_order_relaxed);
                                        _rxFlush.store(true, std::memory_order_release);
                                }
                                n = kept;
                        }
                        const size_t pushed = _rx.push(ByteSpan(buf, n));
                        if (pushed < n) _rxDropped.fetch_add(uint32_t(n - pushed), std::memory_order_relaxed);
                }
                if (status & UART_INTR_RXFIFO_OVF) {
                        // The FIFO overflowed before the interrupt ran: bytes the hardware dropped.
                        _rxDropped.fetch_add(1, std::memory_order_relaxed);
                        uart_ll_rxfifo_rst(hw);
                }
                uart_ll_clr_intsts_mask(hw, status & RxInterrupts);
                if ((sent & (1u << (SignalInterrupt - 1))) != 0) sendSignalFromIsr(SignalInterrupt);
                if ((sent & (1u << (SignalQuit - 1))) != 0) sendSignalFromIsr(SignalQuit);
                if ((sent & (1u << (SignalSuspend - 1))) != 0) sendSignalFromIsr(SignalSuspend);
                _rxReady.releaseFromIsr();
                inputArrivedFromIsr();
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
