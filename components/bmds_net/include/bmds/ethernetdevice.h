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
#include <bmds/netclassifier.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/spinlock.h>
#include <bmds/task.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

class NetFlow;

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
 * @brief Who a stamped frame's departure time goes to (`EthernetDevice::transmitStamped()`).
 *
 * @par Thread Safety
 * `stamped()` is called from whichever task reclaims the frame (a sender's or the device's), not
 * with the driver's locks held; it must not block.
 */
class TxStampSink {
        public:
                /** @brief Frame @p ticket, sent with @p cookie, left at @p time. @nonblocking */
                virtual void stamped(uint32_t cookie, uint32_t ticket, const PtpTime &time) = 0;

        protected:
                ~TxStampSink() = default;
};

/**
 * @brief An Ethernet interface: the EMAC's DMA rings, its 1588 clock, and
 *        the PHY (devplan/network.md §4).
 *
 * Received frames go to one receiver (the IP stack, later the real-time
 * classifier) without a copy: the receiver gets the DMA buffer itself and
 * gives it back with `release()`, at once or later and from any task.
 * Sending copies the frame into a transmit buffer, or has the DMA read it
 * where it is (`transmitInPlace()`: internal RAM only, devplan/network.md §29).
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

                /**
                 * @brief Receive buffers: the ring's, and spares for frames receivers hold (bulk's,
                 *        and the real-time reserve: devplan/network.md §30).
                 */
                static constexpr size_t RxBuffers = 32;

                /** @brief Transmit descriptors (each with a buffer for copies). */
                static constexpr size_t TxDescriptors = 16;

                /** @brief A buffer: a whole frame, in whole cache lines (128 bytes). */
                static constexpr size_t BufferBytes = 1536;

                /** @brief Transmit flags. */
                enum TxFlag : uint32_t {
                        TxStamp = 1u << 0,    ///< Record the departure time (`waitTxStamp()`).
                        TxRealTime = 1u << 1, ///< Not bulk: never waits behind the bulk limit.
                        TxPriority = 1u << 2, ///< Not bulk, not a flow's: within `priorityTxLimit()`.
                };

                /** @brief The bulk transmit limit while real-time flows of 1 ms packets or longer run. */
                static constexpr size_t RealTimeBulkTxLimit = 2;

                /** @brief The descriptors `TxPriority` frames may hold while real-time flows run. */
                static constexpr size_t RealTimePriorityTxLimit = 2;

                /** @brief The receive buffers kept for real-time receivers while flows run. */
                static constexpr size_t RealTimeRxReserve = 8;

                /** @brief Real-time flows (`NetFlow`) a device takes at once. */
                static constexpr size_t MaxFlows = 8;

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
                                uint32_t txQueuedMax = 0; ///< The most descriptors queued in the ring at once.
                                uint64_t rxChecksummed = 0;    ///< Frames whose checksums the MAC checked.
                                uint64_t txInPlace = 0;        ///< Frames the DMA read in place (`transmitInPlace()`).
                                uint64_t txRealTime = 0;       ///< Frames sent with `TxRealTime`.
                                uint32_t txRealTimeWaited = 0; ///< Of those, ones that found the ring full.
                                uint32_t rxBulkDropped = 0;    ///< Frames dropped as bulk held its share of buffers.
                                uint32_t rxRestarts = 0;       ///< Times the receive path wedged and the MAC was reset.
                                uint64_t rxRealTime = 0;       ///< Frames given to real-time flows.
                                uint32_t rxRealTimeDropped = 0; ///< Of those for flows, ones past a flow's depth.
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
                                bool checksummed = false; ///< The MAC found its IPv4 and TCP/UDP/ICMP checksums good.
                                PtpTime stamp;            ///< The hardware arrival time, if `stamped`.
                };

                /**
                 * @brief Takes a received frame. Runs on the device's task: it must not block, and
                 *        must call `release()` for the frame once (now or later). It may only read
                 *        the frame there; a receiver that writes into it releases it from another
                 *        task (the buffer's cache lines are cleaned there).
                 */
                using Receiver = void (*)(void *arg, EthernetDevice &device, const RxFrame &frame);

                /**
                 * @brief Told that the DMA has finished with a frame sent in place
                 *        (`transmitInPlace()`): its buffers are the caller's again. Runs in a sender
                 *        or on the device's task, without the driver's locks; must not block or
                 *        send.
                 */
                using TxDone = void (*)(void *arg, void *token);

                /** @brief The most pieces a frame sent in place may have. */
                static constexpr size_t MaxInPlacePieces = 4;

                /** @brief Told of a link change (`link()`), on the device's task; must not block. */
                using LinkHandler = void (*)(void *arg, EthernetDevice &device);

                /** @brief Address filters the MAC has beyond the station's: the most groups filtered exactly. */
                static constexpr size_t MaxFilters = 15;

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

                /** @brief Returns the station address. @nonblocking */
                MacAddress address() const;

                /** @brief Returns the address the chip came with (its eFuse). */
                MacAddress factoryAddress() const { return _factoryAddress; }

                /**
                 * @brief Sets the station address: frames to it are received, and it is what senders
                 *        should put as their source.
                 * @return `Ok`, or `Invalid` for a group or zero address.
                 */
                Error setAddress(const MacAddress &address);

                /**
                 * @brief Sets the multicast groups to receive: the MAC's address filters
                 *        (`multicastFilters()`) when they are enough, else every multicast frame
                 *        (the chip has no hash filter; the stack drops what nobody joined).
                 *        Broadcast is always received.
                 * @return `Ok`, or `Invalid` for an address that isn't a group's.
                 */
                Error setMulticast(Span<const MacAddress> groups);

                /** @brief Returns how many address filters the MAC has for groups (probed at start). */
                size_t multicastFilters() const { return _multicastFilters; }

                /** @brief Returns true while every multicast frame is received (more groups than filters). */
                bool passesAllMulticast() const { return _allMulticast; }

                /** @brief Returns the link. @nonblocking */
                Link link() const;

                /** @brief Returns what the MAC was built with. */
                const Features &features() const { return _features; }

                /** @brief Returns the counters. @nonblocking */
                Counters counters() const;

                /**
                 * @brief Sets the receiver of every frame (nullptr: frames are dropped and counted).
                 *        A call to the old receiver that has begun is waited for, so its argument may
                 *        go away once this returns.
                 * @blocking Briefly (receivers don't block).
                 */
                void setReceiver(Receiver receiver, void *arg);

                /** @brief Returns the receiver and its argument (to pass frames on to it). */
                Receiver receiver(void *&arg) const;

                /** @brief Sets who is told of link changes (nullptr: nobody). */
                void setLinkHandler(LinkHandler handler, void *arg);

                /**
                 * @brief Gives a received frame's buffer back. Any task; from any but the device's,
                 *        it cleans the buffer's cache lines (a receiver may have written it).
                 * @nonblocking
                 */
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
                 * @brief As `transmit()`, the frame gathered from @p pieces, in order (an IP stack's
                 *        chained buffers).
                 * @blocking
                 */
                Result<uint32_t> transmit(Span<const Span<const uint8_t>> pieces, uint32_t flags,
                                          unsigned int timeoutMs);

                /**
                 * @brief As `transmit()`, but the DMA reads the pieces where they are: they must stay
                 *        unchanged until the driver passes @p token to the `setTxDone()` function.
                 *        A frame that can't be sent so is copied, and done at once: a piece outside
                 *        internal DMA memory (the EMAC underflows reading PSRAM: its transmit FIFO
                 *        is 256 bytes), more than `MaxInPlacePieces`, `TxStamp`.
                 * @return As `transmit()`. On an error the frame isn't sent and @p token isn't passed.
                 * @blocking
                 */
                Result<uint32_t> transmitInPlace(Span<const Span<const uint8_t>> pieces, uint32_t flags, void *token,
                                                 unsigned int timeoutMs);

                /**
                 * @brief As `transmit()` with `TxStamp`: the departure time goes to @p sink with
                 *        @p cookie, after the frame is reclaimed. @p sink stays valid until
                 *        `forgetStampSink()`.
                 * @blocking
                 */
                Result<uint32_t> transmitStamped(Span<const Span<const uint8_t>> pieces, uint32_t flags,
                                                 TxStampSink &sink, uint32_t cookie, unsigned int timeoutMs);

                /**
                 * @brief Stamps of frames queued for @p sink go nowhere from now on; waits for one
                 *        being handed over. Not with the transmit path's locks held. @blocking Briefly.
                 */
                void forgetStampSink(TxStampSink &sink);

                /** @brief Sets who is told when frames sent in place are done (before sending any). */
                void setTxDone(TxDone done, void *arg);

                /**
                 * @brief Sets how many transmit descriptors bulk frames (those without
                 *        `TxRealTime`) may hold at once, 1 to `TxDescriptors` (the default): fewer
                 *        bounds how long a real-time frame waits behind them, at bulk's
                 *        throughput's cost. Frames already queued stay. Any task. @nonblocking
                 */
                void setBulkTxLimit(size_t descriptors);

                /** @brief Returns the bulk transmit limit. */
                size_t bulkTxLimit() const { return _bulkTxLimit.load(std::memory_order_relaxed); }

                /**
                 * @brief Returns how many descriptors `TxPriority` frames may hold at once: all of
                 *        them without real-time flows, `RealTimePriorityTxLimit` with, so they
                 *        can't take the flows'. @nonblocking
                 */
                size_t priorityTxLimit() const { return _priorityTxLimit.load(std::memory_order_relaxed); }

                /**
                 * @brief Keeps @p buffers receive buffers (up to `RxBuffers - RxDescriptors`) from
                 *        bulk frames, for real-time receivers: bulk frames past their share are
                 *        dropped at once (`rxBulkDropped`), so the ring always refills. 0 (the
                 *        default) gives bulk every spare. Any task. @nonblocking
                 */
                void setRxReserve(size_t buffers);

                /** @brief Returns the receive reserve. */
                size_t rxReserve() const { return _rxReserve.load(std::memory_order_relaxed); }

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
                friend class NetFlow;
                struct Isr;

                struct TxSlot {
                                uint32_t ticket = 0;
                                uint16_t length = 0; // the frame's, in its last descriptor's slot (0 in the others)
                                void    *token = nullptr; // a frame sent in place, in its last descriptor's slot
                                int8_t   buffer = -1;     // a copied frame's buffer, in its last descriptor's slot
                                bool         bulk = false;    // counted in _txBulkQueued
                                bool         priority = false; // counted in _txPriorityQueued
                                TxStampSink *sink = nullptr;   // whose stamp it is, in its last descriptor's slot
                                uint32_t     cookie = 0;
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
                void        startPtp(PtpTime time);
                void        applyFilters(); // _filterLock held
                bool        checkWedged(bool tookFrames);
                void        recoverRx();
                void        readFeatures();
                void        probeFilters();
                void        receive();
                void        refill();
                void        reclaim();
                // What a reclaim found, passed on once the transmit lock is let go.
                static constexpr size_t MaxReclaimStamps = 4; // sinks' stamps a reclaim hands over
                struct FlowStamp {
                                TxStampSink *sink;
                                uint32_t     cookie;
                                uint32_t     ticket;
                                PtpTime      time;
                };
                struct Reclaimed {
                                void     *tokens[TxDescriptors] = {};
                                size_t    count = 0;
                                bool      stamped = false;
                                FlowStamp stamps[MaxReclaimStamps] = {};
                                size_t    stampCount = 0; // each counted in _flowCalls until handed over
                };

                Result<uint32_t> send(Span<const Span<const uint8_t>> pieces, uint32_t flags, void *token,
                                      unsigned int timeoutMs, TxStampSink *sink = nullptr, uint32_t cookie = 0);
                // The flows (NetFlow's): registered with their rules; the limits follow them.
                Error addFlow(NetFlow &flow, const NetClassifier::Rule &rule, const MacAddress &group);
                void  removeFlow(NetFlow &flow);
                void  setFlowRule(NetFlow &flow, const NetClassifier::Rule &rule); // narrowed (connect)
                void  applyFlowLimits();                                           // _flowSetLock held
                bool  classify(const RxFrame &frame);
                void  mergeFilters(); // _filterLock held
                // Takes the transmit lock and waits (letting it go) for a copy buffer, or for
                // @p descriptors; returns with it held, or with an error without it.
                enum class TxClass : uint8_t {
                        Bulk,
                        Priority,
                        RealTime
                };
                Error       waitForTx(const Deadline &deadline, TxClass cls, size_t descriptors, bool buffer,
                                      Reclaimed &reclaimed);
                void        reclaimLocked(Reclaimed &reclaimed); // _txLock held
                void        finish(Reclaimed &reclaimed);        // _txLock not held
                void        checkLink();
                void        applyLink(const Link &link);
                void        startTraffic();
                void        stopTraffic();

                uint8_t    _number;
                char       _name[8] = {};
                MacAddress _address;
                MacAddress _factoryAddress;
                Features   _features;
                size_t     _multicastFilters = 0;
                bool       _allMulticast = false;
                Mutex      _filterLock; // the address registers

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
                std::atomic<size_t> _rxReserve{0};
                SpinLock            _rxFreeLock;

                // Transmit: the next descriptor to fill and the next to reclaim.
                SpinLock            _txLock;            // the ring: held for a few µs, never across a wait or a copy
                Semaphore     _txSpace;           // released when room frees for a waiting sender
                uint32_t            _txWaiters = 0;     // senders waiting for room, under _txLock
                size_t        _txHead = 0;
                size_t        _txTail = 0;
                size_t        _txQueued = 0;
                size_t              _txBulkQueued = 0;           // bulk frames' descriptors in the ring
                std::atomic<size_t> _bulkTxLimit{TxDescriptors}; // set under _txLock
                size_t              _txPriorityQueued = 0;       // TxPriority frames' descriptors
                std::atomic<size_t> _priorityTxLimit{TxDescriptors}; // set under _txLock
                uint32_t      _txTicket = 0;
                TxSlot        _txSlots[TxDescriptors];
                uint8_t             _txBufFree[TxDescriptors] = {}; // copy buffers free (a stack), under _txLock
                size_t              _txBufFreeCount = 0;
                TxDone              _txDone = nullptr; // with its argument: set before frames are sent in place
                void               *_txDoneArg = nullptr;
                WaitListOf<4> _txStampWaiters;

                Receiver          _receiver = nullptr; // with its argument, under _stateLock
                void             *_receiverArg = nullptr;
                std::atomic<bool> _inReceiver{false};
                LinkHandler       _linkHandler = nullptr;
                void             *_linkHandlerArg = nullptr;

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
                uint64_t              _rxProgressUs = 0; // last time the receive FIFO wasn't stuck full
                uint32_t              _ptpAddend = 0;    // the 1588 clock's addend, restored after a reset
                MacAddress            _filterGroups[MaxFilters]; // under _filterLock: what the filters hold
                size_t                _filterGroupCount = 0;
                MacAddress            _stackGroups[MaxFilters]; // under _filterLock: setMulticast()'s
                size_t                _stackGroupCount = 0;
                bool                  _stackAllMulticast = false; // more than the filters
                MacAddress            _flowGroups[MaxFlows];      // under _filterLock: the flows'
                size_t                _flowGroupCount = 0;

                // The flows: their rules in the same order (classified under _flowLock, a spinlock);
                // add and remove under _flowSetLock too.
                Mutex                 _flowSetLock;
                SpinLock              _flowLock;
                NetFlow              *_flows[MaxFlows] = {};
                NetClassifier::Rule   _rules[MaxFlows];
                std::atomic<size_t>   _flowCount{0};
                std::atomic<uint32_t> _flowCalls{0};     // deliveries and stamps in progress (outside the locks)
                std::atomic<uint32_t> _rxFlowBuffers{0}; // pool slots flows hold (bits): not bulk's
                SpinLock              _intrLock; // the DMA's interrupt enables (ISR and task)
};

BMDS_NAMESPACE_END
