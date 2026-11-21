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

/**
 * @brief UART @p port as a `Console`: its own interrupt over the UART's LL layer (as the USB
 *        serial console's), a receive ring the interrupt fills and a transmit ring it drains.
 *
 * Made once per UART when its port first starts; `start()` and `stop()` as `io/uartN` changes.
 * It begins binary (no signals, no `\n` to CR LF): a program sets the modes it wants.
 *
 * @par Thread Safety
 * As `Console`: writers serialized, one reader; `start()` and `stop()` from the port's owner.
 */
class UartConsole final : public Console {
        public:
                /** @brief Receive ring, bytes (PSRAM). */
                static constexpr size_t RxBytes = 2048;

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

        protected:
                Result<size_t> readInput(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> tryReadInput(WritableByteSpan buf) override;
                bool           hasInput() const override { return !_rx.isEmpty(); }

        private:
                static void isr(void *arg);
                void        handleInterrupt();
                void        enableTxInterrupt();

                unsigned              _port;
                char                  _name[8] = {};
                ByteRing              _tx;
                ByteRing              _rx;
                Mutex                 _txMutex;
                Semaphore             _txSpace{1, 0};
                Semaphore             _rxReady{1, 0};
                SpinLock              _intLock;
                void                 *_intr = nullptr; // intr_handle_t
                std::atomic<bool>     _running{false};
                std::atomic<uint32_t> _rxDropped{0};
                std::atomic<uint32_t> _lineErrors{0};
                std::atomic<uint32_t> _rxFlushTo{0};
                std::atomic<bool>     _rxFlush{false};
};

BMDS_NAMESPACE_END
