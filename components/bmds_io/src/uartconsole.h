/**
 * @file      uartconsole.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A UART port as a terminal, `/dev/ttySN` (devplan/io.md §11.1). Target only.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/bytering.h>
#include <bmds/config.h>
#include <bmds/console.h>
#include <bmds/ioport.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/spinlock.h>

BMDS_NAMESPACE_BEGIN

class Deadline;
struct UartStash;

/**
 * @brief UART @p port as a `Console`: its own interrupt over the UART's LL layer (as the USB
 *        serial console's), a receive ring the interrupt fills and a transmit ring it drains.
 *
 * The interrupt is cache-safe: while the flash is busy it keeps the FIFO's input in a stash in
 * internal RAM, moved into the ring at the flash's end. Above 1 Mbaud the input comes by UHCI's
 * DMA instead (one port at a time), into a ring of descriptors in PSRAM (devplan/io.md §14.3).
 *
 * Made once per UART when its port first starts; `start()` and `stop()` as `io/uartN` changes.
 * It begins binary (no signals, no `\n` to CR LF): a program sets the modes it wants.
 *
 * @par Thread Safety
 * As `Console`: writers serialized, one reader; `start()` and `stop()` from the port's owner.
 */
class UartConsole final : public Console {
        public:
                /**
                 * @brief Receive ring, bytes (PSRAM): input while readers can't run, as through a
                 *        flash write (85 ms measured at 1 Mbaud); 131 ms at the top baud
                 *        (devplan/io.md §14.3).
                 */
                static constexpr size_t RxBytes = 65536;

                /** @brief Transmit ring, bytes (PSRAM). */
                static constexpr size_t TxBytes = 1024;

                UartConsole(unsigned port, WritableByteSpan txStorage, WritableByteSpan rxStorage);

                /**
                 * @brief Starts the UART as @p line says (its pins routed by the caller).
                 * @return `Ok`, `Invalid` (a rate the clock can't make), or the interrupt's error.
                 */
                Error start(const IoUartSetup &line);

                /** @brief Stops it: its interrupt freed, its clocks off, waiting input dropped. */
                void stop();

                /** @brief True while it runs. */
                bool isRunning() const { return _running.load(std::memory_order_acquire); }

                /** @brief Bytes lost to a full receive ring or FIFO. */
                uint32_t rxDropped() const { return _rxDropped.load(std::memory_order_relaxed); }

                /** @brief Framing and parity errors seen. */
                uint32_t lineErrors() const { return _lineErrors.load(std::memory_order_relaxed); }

                using Console::write;

                const char    *name() const override { return _name; }
                uint32_t       capabilities() const override { return 0; }
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                Error          flush(unsigned int timeoutMs) override;
                size_t         writeRoom() override; // the transmit ring's free bytes (half while \n is CR LF)

        protected:
                Result<size_t> readInput(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> tryReadInput(WritableByteSpan buf) override;
                bool           hasInput() const override { return !_rx.isEmpty(); }

        private:
                static void isr(void *arg);
                void        handleInterrupt();
                void        enableTxInterrupt();
                void        accept(uint8_t *buf, size_t n, uint32_t &sent);
                void        drainStash(uint32_t &sent);
                void        delivered(uint32_t sent);
                Error       startDma();
                void        stopDma();
                void        handleDma();
                void        drainDma(uint32_t &sent);

                friend struct UartStash;

                unsigned              _port;
                char                  _name[8] = {};
                ByteRing              _tx;
                ByteRing              _rx;
                Mutex                 _txMutex;
                Semaphore             _txSpace{1, 0};
                Semaphore             _rxReady{1, 0};
                SpinLock              _intLock;
                void                 *_intr = nullptr;  // intr_handle_t
                UartStash            *_stash = nullptr; // internal RAM: input while the flash is busy
                std::atomic<bool>     _running{false};
                std::atomic<uint32_t> _rxDropped{0};
                std::atomic<uint32_t> _lineErrors{0};
                std::atomic<uint32_t> _rxFlushTo{0};
                std::atomic<bool>     _rxFlush{false};
};

BMDS_NAMESPACE_END
