/**
 * @file      counterchip.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/semaphore.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief `/dev/counter0` (devplan/io.md §15.3): the pulse counters (`io/pcntN` sets each up) as
 *        Linux's counter character device, open by one file at a time. Watches added, then
 *        enabled; `read()` gives the events they catch as Linux's `struct counter_event`.
 *
 * @par Thread Safety
 * Any task; the counters' interrupt queues the events.
 */
class CounterChip final : public Stream {
        public:
                static constexpr unsigned Counts = 4;      ///< The device's counts (`io/pcnt0` .. `io/pcnt3`).
                static constexpr size_t   MaxWatches = 16; ///< Watches enabled at once.
                static constexpr size_t   MaxEvents = 64;  ///< Events waiting to be read; more are dropped, counted.

                /** @brief The components and events it gives: Linux's numbers. */
                static constexpr uint8_t ComponentNone = 0, ComponentCount = 2, ScopeCount = 2, EventThreshold = 3;

                /** @brief A watch (Linux's `struct counter_watch`). */
                struct Watch {
                                uint8_t type = 0, scope = 0, parent = 0, id = 0, event = 0, channel = 0;
                };

                /** @brief An event as read (Linux's `struct counter_event`). */
                struct Event {
                                uint64_t timestamp = 0;
                                uint64_t value = 0;
                                uint8_t  type = 0, scope = 0, parent = 0, id = 0, event = 0, channel = 0, status = 0,
                                        reserved = 0;
                };
                static_assert(sizeof(Event) == 24, "Linux's counter_event");

                /** @brief A count and the device, as `BMDS_COUNTER_INFO` reports them. */
                struct Info {
                                uint32_t ncounts = Counts;
                                bool     configured = false;
                                bool     monitor = false;
                                bool     hasThreshold = false;
                                uint32_t function = 0; ///< Linux's `enum counter_function`.
                                int32_t  pinA = -1;
                                int32_t  pinB = -1;
                                uint32_t filterNs = 0;
                                int64_t  threshold = 0;
                                uint64_t eventsLost = 0;
                };

                /** @brief Adds `/dev/counter0` (once the counters have started). */
                static Error addFiles();

                /** @brief Adds @p w for the next `enableEvents()`: `Invalid` (a watch it can't give), `NoMem`. @blocking */
                Error addWatch(const Watch &w);

                /** @brief The watches added take effect (those before replaced), events then queued. @blocking */
                Error enableEvents();

                /** @brief No events; the watches gone. @blocking */
                Error disableEvents();

                /** @brief Count @p count's value and the monotonic time it was taken: `NotExist` (not set up). @blocking */
                Error value(unsigned count, int64_t &out, int64_t &timeNs);

                /** @brief Sets count @p count to @p v. @blocking */
                Error preset(unsigned count, int64_t v);

                /** @brief Sets count @p count's threshold to @p v (`EventThreshold` there), or none. @blocking */
                Error setThreshold(unsigned count, bool armed, int64_t v);

                /** @brief Count @p count and the device. @blocking */
                Error info(unsigned count, Info &out);

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan, unsigned int) override { return makeError<size_t>(Error::Invalid); }
                uint8_t        readiness() override;
                WaitList      *readinessWaiters() override { return &_pollers; }
                CounterChip   *counterChip() override { return this; }
                void           handleClosed(uint8_t mode) override;

                /** @brief Queues @p e if a watch on its event and count asks (the interrupt; the counts' lock held). */
                void eventFromIsr(unsigned count, uint8_t event, uint64_t timestamp);

                /** @brief Wakes a reader and pollers (the interrupt, after the lock). @isrsafe */
                void wakeFromIsr();

        private:
                CounterChip() = default;

                // Under the counts' lock (the interrupt reads the active watches and fills the queue).
                Watch    _next[MaxWatches];
                size_t   _nextCount = 0;
                Watch    _active[MaxWatches];
                size_t   _activeCount = 0;
                bool     _enabled = false;
                Event    _events[MaxEvents];
                uint32_t _head = 0; // counts, `% MaxEvents`
                uint32_t _tail = 0;

                Semaphore     _ready{1, 0};
                WaitListOf<4> _pollers;
};

BMDS_NAMESPACE_END
