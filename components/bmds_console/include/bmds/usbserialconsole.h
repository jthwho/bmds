/**
 * @file      usbserialconsole.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/bytering.h>
#include <bmds/config.h>
#include <bmds/console.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/spinlock.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The console on the USB-Serial/JTAG peripheral (the board's USB-C port).
 *
 * The console is a singleton, created by the `init()` boot stage, which
 * allocates its ring buffers from `MemSpace::Psram`. Nothing is allocated
 * after that. Input and output are interrupt-driven, and a reader waiting
 * for input uses no CPU.
 *
 * - **Output** goes through a ring buffer (`CONFIG_BMDS_USJ_TX_BYTES`),
 *   with `'\n'` sent as CR LF. When no host is reading (the serial port
 *   isn't open), the console marks itself *stalled* after one write times
 *   out (or at `init()`, if boot output is still unread). While stalled,
 *   a write is queued whole if it fits and otherwise dropped, without
 *   waiting, until the host reads again.
 * - **Input** goes through a ring buffer (`CONFIG_BMDS_USJ_RX_BYTES`).
 *   There is no flow control toward the host: input that arrives while
 *   the ring is full is dropped and counted (`rxDroppedBytes()`). The
 *   hardware cannot be used to hold the host off (see the ISR).
 *
 * ESP-IDF's polled console code keeps using the peripheral before `init()`
 * and after a panic.
 *
 * @par Thread Safety
 * Thread-safe for writing; one reader task.
 */
class UsbSerialConsole final : public Console {
        public:
                /** @brief Output timeout used by the log sink; a stalled console drops log text. */
                static constexpr unsigned int LogTimeoutMs = 20;

                /**
                 * @brief Boot stage: creates the console, starts its interrupt
                 *        and routes kernel log output to it.
                 * @return `Error::Ok`, `Error::NotSupported` if the board has no
                 *         USB-Serial/JTAG port, or the interrupt allocation error.
                 */
                static Error init();

                /** @brief Returns the console. `init()` must have succeeded. */
                static UsbSerialConsole &instance();

                using Console::write;

                const char    *name() const override { return "usb"; }
                uint32_t       capabilities() const override { return Ansi | Color; }
                Size           size() const override { return Size(); }
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> tryRead(WritableByteSpan buf) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                Error          flush(unsigned int timeoutMs) override;

                /** @brief Returns true while output is being discarded because no host is reading. */
                bool isStalled() const { return _stalled.load(std::memory_order_relaxed); }

                /** @brief Returns the input bytes dropped because the input ring was full. */
                uint32_t rxDroppedBytes() const { return _rxDropped.load(std::memory_order_relaxed); }

                /** @brief Called by `StaticObject`; use `init()`. */
                UsbSerialConsole(WritableByteSpan txStorage, WritableByteSpan rxStorage);

        private:
                static void isr(void *arg);
                void        handleInterrupt();
                void        enableInterrupt(uint32_t mask);
                size_t      pushTranslated(ByteSpan data, size_t &done, bool &crSent);

                ByteRing              _tx;
                ByteRing              _rx;
                Mutex                 _txMutex;
                Semaphore             _txSpace; // released by the ISR when it frees TX space
                Semaphore             _rxReady; // released by the ISR when input arrives
                SpinLock              _intLock; // guards interrupt-enable read-modify-writes
                std::atomic<bool>     _stalled{false};
                std::atomic<uint32_t> _rxDropped{0};
};

BMDS_NAMESPACE_END
