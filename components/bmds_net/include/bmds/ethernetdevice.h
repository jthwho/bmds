/**
 * @file      ethernetdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Ethernet driver (devplan/network.md §4): the ESP32-P4's EMAC and the
 * board's PHY as a numbered device, `ethN`.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/ethernet.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/spinlock.h>
#include <bmds/task.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A time on a 1588 clock: seconds and nanoseconds.
 *
 * @par Thread Safety
 * A value type.
 */
struct PtpTime {
                uint64_t seconds = 0;     ///< 48 bits on the wire.
                uint32_t nanoseconds = 0; ///< Below 1e9.

                /** @brief Returns the time as nanoseconds (to year 2262). */
                constexpr int64_t toNs() const {
                        return static_cast<int64_t>(seconds) * 1'000'000'000 + static_cast<int64_t>(nanoseconds);
                }

                /** @brief Returns @p ns (at least 0) as a time. */
                static constexpr PtpTime fromNs(int64_t ns) {
                        if (ns < 0) ns = 0;
                        return {static_cast<uint64_t>(ns / 1'000'000'000), static_cast<uint32_t>(ns % 1'000'000'000)};
                }
};

/**
 * @brief An Ethernet interface: the EMAC's DMA rings, its 1588 clock, and
 *        the PHY (devplan/network.md §4).
 *
 * Received frames go to one receiver (the IP stack, later the real-time
 * classifier) without a copy: the receiver gets the DMA buffer itself and
 * gives it back with `release()`, at once or later and from any task.
 * Sending copies the frame into a transmit buffer.
 *
 * The device's task (`eth0`, priority `CONFIG_BMDS_NET_PRIORITY`) takes the
 * frames the DMA has written, reclaims sent ones, and checks the link once a
 * second: the board's PHY has no interrupt line (docs/HARDWARE.md), the one
 * timed wake-up in the driver.
 *
 * @par Thread Safety
 * Thread-safe in tasks. Not ISR-safe.
 */
class EthernetDevice {
        public:
                /** @brief Devices the system can have. */
                static constexpr size_t MaxDevices = 1;

                /** @brief Receive descriptors in the DMA's ring. */
                static constexpr size_t RxDescriptors = 16;

                /** @brief Receive buffers: the ring's, and spares for frames receivers hold. */
                static constexpr size_t RxBuffers = 24;

                /** @brief Transmit descriptors (each with its buffer). */
                static constexpr size_t TxDescriptors = 16;

                /** @brief A buffer: a whole frame, in whole cache lines (128 bytes). */
                static constexpr size_t BufferBytes = 1536;

                /** @brief Transmit flags. */
                enum TxFlag : uint32_t {
                        TxStamp = 1u << 0, ///< Record the departure time (`waitTxStamp()`).
                };

                /** @brief The link as the PHY reports it. */
                struct Link {
                                bool     up = false;
                                bool     fullDuplex = false;
                                uint16_t speedMbps = 0; ///< 10 or 100; 0 while down.
                };

                /**
                 * @brief What the MAC was built with: its DMA feature register
                 *        (devplan/network.md §2).
                 */
                struct Features {
                                uint32_t raw = 0;                ///< The register as read.
                                uint32_t version = 0;            ///< The MAC's version register.
                                uint8_t  rxChannels = 1;         ///< Receive DMA channels (1 + additional).
                                uint8_t  txChannels = 1;         ///< Transmit DMA channels (1 + additional).
                                bool     avb = false;            ///< The AV feature (802.1Qav queues).
                                bool     timestampV2 = false;    ///< IEEE 1588-2008 advanced timestamps.
                                bool     txChecksum = false;     ///< Checksum insertion on transmit.
                                bool     rxChecksum = false;     ///< Checksum checking on receive (type 2).
                                bool     hashFilter = false;     ///< The multicast hash filter.
                                bool     extraAddresses = false; ///< Address filters beyond the first.
                                bool     l3l4Filter = false;     ///< The layer 3/4 filter.
                                bool     flexiblePps = false;    ///< Flexible PPS outputs.
                                bool     mmc = false;            ///< The RMON counters.
                };

                /** @brief Counters since boot. */
                struct Counters {
                                uint64_t rxFrames = 0;
                                uint64_t rxBytes = 0;
                                uint64_t txFrames = 0;
                                uint64_t txBytes = 0;
                                uint32_t rxErrors = 0;    ///< Frames with an error in their status (dropped).
                                uint32_t rxNoBuffer = 0;  ///< Times the ring ran out of buffers.
                                uint32_t rxMissed = 0;    ///< Frames the MAC dropped (its missed-frame counter).
                                uint32_t rxOverflow = 0;  ///< Frames lost to its receive FIFO overflowing.
                                uint32_t rxUnclaimed = 0; ///< Frames with no receiver to take them.
                                uint32_t txErrors = 0;    ///< Frames whose status reported an error.
                                uint32_t txFull = 0;      ///< Sends that waited for a free descriptor.
                                uint32_t interrupts = 0;
                                uint32_t linkChanges = 0;
                                uint32_t rxHeld = 0;      ///< Buffers receivers hold now.
                                uint32_t rxHeldMax = 0;   ///< The most they have held at once.
                                uint32_t txQueuedMax = 0; ///< The most frames queued in the ring at once.
                };

                /** @brief Registers as read, for diagnosis (`net regs`). */
                struct Registers {
                                uint32_t macConfig = 0;
                                uint32_t frameFilter = 0;
                                uint32_t macDebug = 0;
                                uint32_t dmaStatus = 0;
                                uint32_t dmaMode = 0;
                                uint32_t gmacCtrl = 0;     ///< HP_SYSTEM's GMAC control (clock domain resets).
                                uint32_t clkCtrl0 = 0;     ///< HP_SYS_CLKRST's EMAC clocks (peri_clk_ctrl00).
                                uint32_t clkCtrl1 = 0;     ///< (peri_clk_ctrl01)
                                uint32_t rxList = 0;       ///< The receive ring's address.
                                uint32_t rxCurrent = 0;    ///< The descriptor the DMA is at.
                                uint32_t rxNext = 0;       ///< The driver's next descriptor (index).
                                uint32_t rxEmpty = 0;      ///< Descriptors waiting for buffers.
                                uint32_t rxFree = 0;       ///< Buffers in the pool.
                                uint32_t rxOwnedByDma = 0; ///< Ring descriptors the DMA owns.
                };

                /** @brief A received frame, owned by its receiver until `release()`. */
                struct RxFrame {
                                uint8_t *data = nullptr;
                                uint16_t length = 0; ///< Without the FCS.
                                uint8_t  buffer = 0; ///< The pool slot (for `release()`).
                                bool     stamped = false;
                                PtpTime  stamp; ///< The hardware arrival time, if `stamped`.
                };

                /**
                 * @brief Takes a received frame. Runs on the device's task: it must not block, and
                 *        must call `release()` for the frame once (now or later).
                 */
                using Receiver = void (*)(void *arg, EthernetDevice &device, const RxFrame &frame);

                /**
                 * @brief Boot stage: makes and starts the board's Ethernet devices (`eth0`). A board
                 *        without Ethernet has none.
                 * @return `Ok`, or the error that stopped the device (logged; boot goes on).
                 */
                static Error init();

                /** @brief Returns device @p number, or `NotExist`. */
                static Result<EthernetDevice *> device(size_t number);

                /** @brief Returns the number of devices. */
                static size_t count();

                explicit EthernetDevice(uint8_t number);
                EthernetDevice(const EthernetDevice &) = delete;
                EthernetDevice &operator=(const EthernetDevice &) = delete;

                /** @brief Returns the device's number (`ethN`). */
                uint8_t number() const { return _number; }

                /** @brief Returns the device's name, "eth0". */
                const char *name() const { return _name; }

                /** @brief Returns the station address. */
                MacAddress address() const { return _address; }

                /** @brief Returns the link. @nonblocking */
                Link link() const;

                /** @brief Returns what the MAC was built with. */
                const Features &features() const { return _features; }

                /** @brief Returns the counters. @nonblocking */
                Counters counters() const;

                /**
                 * @brief Sets the receiver of every frame (nullptr: frames are dropped and counted).
                 */
                void setReceiver(Receiver receiver, void *arg);

                /** @brief Gives a received frame's buffer back. Any task. @nonblocking */
                void release(const RxFrame &frame);

                /**
                 * @brief Sends @p frame (destination, source, type and payload; no FCS), copying it.
                 * @param flags     `TxFlag` bits.
                 * @param timeoutMs How long to wait for a free descriptor, or `WaitForever`.
                 * @return A ticket for `waitTxStamp()`; `Invalid` for a frame too long or short,
                 *         `NotReady` while the link is down, `Timeout`.
                 * @blocking
                 */
                Result<uint32_t> transmit(Span<const uint8_t> frame, uint32_t flags, unsigned int timeoutMs);

                /**
                 * @brief Waits for the departure time of a frame sent with `TxStamp`.
                 * @return The time; `NotExist` if the frame wasn't stamped or its stamp is no
                 *         longer kept, `Timeout`.
                 * @blocking
                 */
                Result<PtpTime> waitTxStamp(uint32_t ticket, unsigned int timeoutMs);

                /** @brief Returns the 1588 clock's time. @nonblocking */
                PtpTime ptpTime() const;

                /** @brief Sets the 1588 clock. */
                Error setPtpTime(PtpTime time);

                /**
                 * @brief Sets the 1588 clock's frequency correction, in parts per billion
                 *        (±5'000'000), through its addend.
                 */
                Error setPtpFrequency(int32_t ppb);

                /**
                 * @brief Resets the PHY through its reset line and starts it again (on the
                 *        device's task, which then sees the link go down and come back). @nonblocking
                 */
                void restartPhy();

                /** @brief Reads the registers that show the DMA's and MAC's state. */
                Registers registers();

                /** @brief Reads PHY register @p reg (MDIO). @blocking (about 30 µs) */
                Result<uint16_t> readPhy(uint8_t reg);

                /** @brief Writes PHY register @p reg (MDIO). @blocking (about 30 µs) */
                Error writePhy(uint8_t reg, uint16_t value);

        private:
                struct Isr;

                struct TxSlot {
                                uint32_t ticket = 0;
                                uint16_t length = 0;
                                bool     wantsStamp = false;
                                bool     done = false; // sent (or failed): the descriptor is back
                                bool     stamped = false;
                                PtpTime  stamp;
                };

                static void taskEntry(void *arg);
                void        run();
                Error       start();
                Error       resetPhy();
                Error       startMac();
                void        startPtp();
                void        readFeatures();
                void        receive();
                void        refill();
                void        reclaim();
                void        checkLink();
                void        applyLink(const Link &link);
                void        startTraffic();
                void        stopTraffic();

                uint8_t    _number;
                char       _name[8] = {};
                MacAddress _address;
                Features   _features;

                // The descriptors and buffers: one block in internal DMA memory.
                uint8_t *_memory = nullptr;
                void    *_rxDesc = nullptr; // eth_dma_rx_descriptor_t[RxDescriptors]
                void    *_txDesc = nullptr; // eth_dma_tx_descriptor_t[TxDescriptors]
                uint8_t *_rxPool = nullptr; // RxBuffers × BufferBytes
                uint8_t *_txPool = nullptr; // TxDescriptors × BufferBytes

                // Receive: the next descriptor the DMA fills and the next to refill; the pool's
                // free slots (a stack), and which slot each descriptor holds.
                size_t              _rxNext = 0;
                size_t              _rxRefill = 0;
                std::atomic<size_t> _rxEmpty{0}; // descriptors waiting for a buffer
                uint8_t             _rxSlot[RxDescriptors] = {};
                uint8_t             _rxFree[RxBuffers] = {};
                size_t              _rxFreeCount = 0;
                SpinLock            _rxFreeLock;

                // Transmit: the next descriptor to fill and the next to reclaim.
                Mutex         _txLock;
                Semaphore     _txSpace;
                size_t        _txHead = 0;
                size_t        _txTail = 0;
                size_t        _txQueued = 0;
                uint32_t      _txTicket = 0;
                TxSlot        _txSlots[TxDescriptors];
                WaitListOf<4> _txStampWaiters;

                Receiver _receiver = nullptr;
                void    *_receiverArg = nullptr;

                Semaphore _wake;
                Task      _task;
                void     *_intr = nullptr;

                mutable SpinLock  _stateLock; // the link and the counters
                Link              _link;
                Counters          _counters;
                Mutex             _mdioLock;
                uint32_t          _phyId = 0;
                bool              _phyStarted = false; // reset and autonegotiating (the task's first look)
                std::atomic<bool> _phyRestart{false};
                bool              _phyWasReset = false; // negotiating from its reset

                std::atomic<uint32_t> _pendingStatus{0}; // DMA status bits the ISR saw
};

BMDS_NAMESPACE_END
