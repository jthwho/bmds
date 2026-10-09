/**
 * @file      midiqueue.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/midievent.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A ring of MIDI events (`devplan/audio.md` §21): a destination's or
 *        a reader's, filled by the router and emptied by its owner.
 *
 * Lock-free, one producer and one consumer, on storage the owner gives (a
 * power of two of events). A full ring takes nothing: the producer counts
 * the drop.
 *
 * @par Thread Safety
 * One producer (`push()`) and one consumer (`pop()`, `peek()`, `clear()`)
 * at a time, concurrently. @nonblocking throughout.
 */
class MidiQueue {
        public:
                constexpr MidiQueue() = default;
                MidiQueue(const MidiQueue &) = delete;
                MidiQueue &operator=(const MidiQueue &) = delete;

                /**
                 * @brief Uses @p storage (its size a power of two, or empty for
                 *        none), empty. Not while it is in use.
                 * @return False if the size isn't a power of two.
                 */
                bool setStorage(Span<MidiEvent> storage) {
                        if ((storage.size() & (storage.size() - 1)) != 0) return false;
                        _events = storage.data();
                        _mask = storage.isEmpty() ? 0 : storage.size() - 1;
                        _head.store(0, std::memory_order_relaxed);
                        _tail.store(0, std::memory_order_relaxed);
                        return true;
                }

                /** @brief Returns the events it holds at most (0: no storage). */
                size_t capacity() const { return _events == nullptr ? 0 : _mask + 1; }

                /** @brief Returns the events waiting. */
                size_t size() const {
                        return _head.load(std::memory_order_acquire) - _tail.load(std::memory_order_acquire);
                }

                /** @brief Returns true if nothing waits. */
                bool isEmpty() const { return size() == 0; }

                /** @brief Adds @p event (producer). @return False if full. */
                bool push(const MidiEvent &event) {
                        const size_t head = _head.load(std::memory_order_relaxed);
                        if (head - _tail.load(std::memory_order_acquire) >= capacity()) return false;
                        _events[head & _mask] = event;
                        _head.store(head + 1, std::memory_order_release);
                        return true;
                }

                /** @brief Returns the oldest event without taking it (consumer), or null. */
                const MidiEvent *peek() const {
                        const size_t tail = _tail.load(std::memory_order_relaxed);
                        if (_head.load(std::memory_order_acquire) == tail) return nullptr;
                        return &_events[tail & _mask];
                }

                /** @brief Takes the oldest event into @p out (consumer). @return False if empty. */
                bool pop(MidiEvent &out) {
                        const MidiEvent *e = peek();
                        if (e == nullptr) return false;
                        out = *e;
                        _tail.store(_tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
                        return true;
                }

                /** @brief Throws away what waits (consumer). */
                void clear() {
                        _tail.store(_head.load(std::memory_order_acquire), std::memory_order_release);
                        return;
                }

        private:
                MidiEvent          *_events = nullptr;
                size_t              _mask = 0;
                std::atomic<size_t> _head{0}; // written by the producer
                std::atomic<size_t> _tail{0}; // written by the consumer
};

BMDS_NAMESPACE_END
