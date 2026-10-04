/**
 * @file      usbserialconsole.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <esp_intr_alloc.h>
#include <esp_private/periph_ctrl.h>
#include <hal/usb_serial_jtag_ll.h>
#include <soc/interrupts.h>
#include <soc/soc_caps.h>
#include <bmds/board.h>
#include <bmds/deadline.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>
#include <bmds/usbserialconsole.h>

BMDS_DEBUG(UsbSerialConsole)

BMDS_NAMESPACE_BEGIN

namespace {

        // Both FIFOs hold one full-speed bulk packet.
        constexpr size_t PacketBytes = 64;

        constexpr uint8_t CtrlC = 0x03;

        static_assert((CONFIG_BMDS_USJ_TX_BYTES & (CONFIG_BMDS_USJ_TX_BYTES - 1)) == 0, "TX size: power of two");
        static_assert((CONFIG_BMDS_USJ_RX_BYTES & (CONFIG_BMDS_USJ_RX_BYTES - 1)) == 0, "RX size: power of two");
        static_assert(CONFIG_BMDS_USJ_RX_BYTES >= PacketBytes, "RX ring must hold a packet");

        constinit StaticObject<UsbSerialConsole> console;

        void logSink(const char *text, size_t lengthBytes) {
                (void)console->write(StringView(text, lengthBytes), UsbSerialConsole::LogTimeoutMs);
                return;
        }

} // namespace

UsbSerialConsole::UsbSerialConsole(WritableByteSpan txStorage, WritableByteSpan rxStorage)
    : _tx(txStorage), _rx(rxStorage), _txSpace(1, 0), _rxReady(1, 0) {}

Error UsbSerialConsole::init() {
        if (!board().usbSerialJtag) return Error::NotSupported;
        // The rings are bulk data, so PSRAM. The ISR isn't in IRAM (it is held
        // off while the cache is disabled), so it may touch PSRAM.
        auto *tx = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, CONFIG_BMDS_USJ_TX_BYTES));
        auto *rx = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, CONFIG_BMDS_USJ_RX_BYTES));
        if (tx == nullptr || rx == nullptr) {
                Memory::free(tx);
                Memory::free(rx);
                return Error::NoMem;
        }
        UsbSerialConsole &c = console.create(WritableByteSpan(tx, CONFIG_BMDS_USJ_TX_BYTES),
                                             WritableByteSpan(rx, CONFIG_BMDS_USJ_RX_BYTES));

        // The ROM and ESP-IDF console normally have the peripheral running
        // already; this is what IDF's own driver does in case they don't.
#if !SOC_RCC_IS_INDEPENDENT
        PERIPH_RCC_ATOMIC() {
                usb_serial_jtag_ll_enable_bus_clock(true);
        }
#else
        usb_serial_jtag_ll_enable_bus_clock(true);
#endif
        usb_serial_jtag_ll_phy_set_defaults();

        // If IDF's boot output is still sitting in the TX FIFO, nobody is
        // reading (the host never opened the port, or it re-enumerated after
        // reset): start stalled rather than paying a write timeout during boot.
        c._stalled.store(!usb_serial_jtag_ll_txfifo_writable(), std::memory_order_relaxed);

        // Don't clear pending status here: a set SERIAL_IN_EMPTY is what makes
        // the first TX enable fire, and a set SERIAL_OUT_RECV_PKT means input
        // is already waiting in the FIFO. TX is enabled only while there is
        // output to send.
        c.enableInterrupt(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        const esp_err_t err = esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, 0, &UsbSerialConsole::isr, &c, nullptr);
        if (err != ESP_OK) {
                bmdsErr("UsbSerialConsole(%p): interrupt allocation failed: %s", &c, Error::fromEspErr(err).desc());
                return Error::fromEspErr(err);
        }
        Log::setSink(logSink);
        bmdsDebug("UsbSerialConsole(%p): running, TX %u RX %u bytes", &c,
                  static_cast<unsigned>(CONFIG_BMDS_USJ_TX_BYTES), static_cast<unsigned>(CONFIG_BMDS_USJ_RX_BYTES));
        return Error();
}

UsbSerialConsole &UsbSerialConsole::instance() {
        return *console;
}

void UsbSerialConsole::isr(void *arg) {
        static_cast<UsbSerialConsole *>(arg)->handleInterrupt();
        return;
}

void UsbSerialConsole::handleInterrupt() {
        const uint32_t status = usb_serial_jtag_ll_get_intsts_mask();

        if (status & USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY) {
                // The host has collected the last packet; the TX FIFO is free.
                usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
                // If the FIFO isn't writable, IDF's polled console code (a stray
                // printf) got in first; another interrupt follows when it drains.
                if (usb_serial_jtag_ll_txfifo_writable()) {
                        size_t sent = 0;
                        while (sent < PacketBytes) {
                                const ByteSpan chunk = _tx.readable();
                                if (chunk.isEmpty()) break;
                                const size_t want =
                                        chunk.size() < PacketBytes - sent ? chunk.size() : PacketBytes - sent;
                                const size_t n = static_cast<size_t>(
                                        usb_serial_jtag_ll_write_txfifo(chunk.data(), static_cast<uint32_t>(want)));
                                _tx.consume(n);
                                sent += n;
                                if (n < want) break;
                        }
                        if (sent > 0) {
                                usb_serial_jtag_ll_txfifo_flush();
                                _stalled.store(false, std::memory_order_relaxed);
                        } else {
                                // Nothing left to send. Flush once more: after a full
                                // 64-byte packet the host keeps waiting for the rest of
                                // the transfer, and this zero-length packet ends it.
                                // When the host collects it, IN_EMPTY is raised again,
                                // so the next enable fires straight away.
                                usb_serial_jtag_ll_txfifo_flush();
                                _intLock.lockFromIsr();
                                usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
                                // A writer may have queued output after the ring was
                                // found empty and before the disable.
                                if (!_tx.isEmpty())
                                        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
                                _intLock.unlockFromIsr();
                        }
                        _txSpace.releaseFromIsr();
                }
        }

        if (status & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT) {
                // Always drain the FIFO at once. Leaving a packet there to hold the
                // host off does not work: the hardware loses it when a TX packet is
                // sent meanwhile (seen on the ESP32-P4 rev v1.3). Input the ring
                // can't take is dropped and counted.
                usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
                uint8_t buf[PacketBytes];
                size_t  n = static_cast<size_t>(usb_serial_jtag_ll_read_rxfifo(buf, PacketBytes));
                if (signalsEnabled()) {
                        // Ctrl-C interrupts the foreground job now, instead of
                        // waiting in the ring for a reader that may never come.
                        // Like a tty, it also flushes the input before it: the
                        // rest of this packet up to it is dropped here, and the
                        // reader discards what the ring already holds.
                        size_t kept = 0;
                        bool   interrupted = false;
                        for (size_t i = 0; i < n; ++i) {
                                if (buf[i] == CtrlC) {
                                        interrupted = true;
                                        kept = 0;
                                } else {
                                        buf[kept++] = buf[i];
                                }
                        }
                        n = kept;
                        if (interrupted) {
                                _rxFlushTo.store(_rx.writePosition(), std::memory_order_relaxed);
                                _rxFlush.store(true, std::memory_order_release);
                                interrupt().raiseFromIsr();
                        }
                }
                const size_t pushed = _rx.push(ByteSpan(buf, n));
                if (pushed < n) _rxDropped.fetch_add(static_cast<uint32_t>(n - pushed), std::memory_order_relaxed);
                _rxReady.releaseFromIsr();
        }
        return;
}

void UsbSerialConsole::enableInterrupt(uint32_t mask) {
        SpinLock::Locker lock(_intLock);
        usb_serial_jtag_ll_ena_intr_mask(mask);
        return;
}

Result<size_t> UsbSerialConsole::tryRead(WritableByteSpan buf) {
        if (_rxFlush.exchange(false, std::memory_order_acquire))
                _rx.discardTo(_rxFlushTo.load(std::memory_order_relaxed));
        const size_t n = _rx.pop(buf);
        if (n == 0) return makeError<size_t>(Error::WouldBlock);
        return makeResult(n);
}

Result<size_t> UsbSerialConsole::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        Deadline deadline(timeoutMs);
        for (;;) {
                // An interrupt comes before input typed after it.
                if (interrupt().isRaised()) return makeError<size_t>(Error::Interrupted);
                Result<size_t> r = tryRead(buf);
                if (r.isOk()) return r;
                Error err = interrupt().wait(_rxReady, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

size_t UsbSerialConsole::pushTranslated(ByteSpan data, size_t &done, bool &crSent) {
        // Copies data[done...] into the TX ring, sending '\n' as "\r\n".
        // `crSent` carries a half-sent pair across calls when the ring fills
        // between the two bytes.
        size_t pushed = 0;
        while (done < data.size()) {
                WritableByteSpan dst = _tx.writable();
                if (dst.isEmpty()) break;
                size_t n = 0;
                while (n < dst.size() && done < data.size()) {
                        const uint8_t b = data[done];
                        if (b == '\n' && !crSent) {
                                dst[n++] = '\r';
                                crSent = true;
                                continue;
                        }
                        dst[n++] = b;
                        crSent = false;
                        ++done;
                }
                _tx.produce(n);
                pushed += n;
        }
        return pushed;
}

Result<size_t> UsbSerialConsole::write(ByteSpan data, unsigned int timeoutMs) {
        if (data.isEmpty()) return makeResult(size_t(0));
        Deadline deadline(timeoutMs);
        if (_txMutex.lock(deadline).isError()) return Result<size_t>(0, Error::Timeout);

        // While stalled, take a write whole or not at all, so the backlog the
        // host eventually sees holds complete lines.
        if (_stalled.load(std::memory_order_relaxed)) {
                size_t need = data.size();
                for (uint8_t b : data) need += (b == '\n');
                if (_tx.freeSpace() < need) {
                        _txMutex.unlock();
                        return Result<size_t>(0, Error::Timeout);
                }
        }

        size_t done = 0;
        bool   crSent = false;
        Error  err;
        for (;;) {
                (void)pushTranslated(data, done, crSent);
                enableInterrupt(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
                if (done == data.size()) break;
                // Ring full. With nobody reading, give up at once instead of
                // making every writer wait out its timeout.
                if (_stalled.load(std::memory_order_relaxed) || waitForSpace(deadline).isError()) {
                        _stalled.store(true, std::memory_order_relaxed);
                        err = Error::Timeout;
                        break;
                }
        }
        _txMutex.unlock();
        return Result<size_t>(done, err);
}

Error UsbSerialConsole::waitForSpace(const Deadline &deadline) {
        // Output that makes no progress for StallMs means nobody is reading,
        // however long the caller was willing to wait.
        Deadline stall(StallMs);
        return _txSpace.acquire(deadline.remainingUs() < stall.remainingUs() ? deadline : stall);
}

Error UsbSerialConsole::flush(unsigned int timeoutMs) {
        Deadline deadline(timeoutMs);
        if (_txMutex.lock(deadline).isError()) return Error::Timeout;
        Error err;
        while (!_tx.isEmpty()) {
                if (_stalled.load(std::memory_order_relaxed)) {
                        err = Error::Timeout;
                        break;
                }
                enableInterrupt(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
                if (waitForSpace(deadline).isError()) {
                        err = Error::Timeout;
                        break;
                }
        }
        _txMutex.unlock();
        return err;
}

BMDS_NAMESPACE_END
