/**
 * @file      waitlist.h
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
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Semaphores to release when something happens: the waiters of an
 *        `Interrupt`, or the pollers of a stream.
 *
 * A waiter `add()`s its semaphore, checks its condition, and waits on the
 * semaphore; whoever changes the condition `notify()`s, releasing every
 * semaphore in the list. Adding before checking means a change in between
 * still releases it. The releases may leave a semaphore with a spare
 * count, so its taker must re-check its condition (the usual loop).
 *
 * The slots are fixed (`WaitListOf<N>`); `add()` fails when they are all
 * taken. No allocation, no FreeRTOS objects of its own;
 * constant-initializable.
 *
 * @par Thread Safety
 * Thread-safe. `notifyFromIsr()` and `hasWaiters()` are ISR-safe.
 */
class WaitList {
        public:
                /** @brief What `add()` returns when every slot is taken. */
                static constexpr size_t NoSlot = SIZE_MAX;

                WaitList(const WaitList &) = delete;
                WaitList &operator=(const WaitList &) = delete;

                /**
                 * @brief Puts @p semaphore in a free slot.
                 * @return The slot, for `remove()`; `NoSlot` if none is free.
                 * @nonblocking
                 */
                size_t add(Semaphore &semaphore);

                /**
                 * @brief Takes the semaphore in @p slot out (`NoSlot`: nothing). Once
                 *        it returns, no `notify()` will release that semaphore.
                 * @nonblocking
                 */
                void remove(size_t slot);

                /**
                 * @brief Releases every semaphore in the list (task context); does
                 *        nothing quickly when the list is empty.
                 * @nonblocking
                 */
                void notify();

                /**
                 * @brief Releases every semaphore in the list (ISR context).
                 * @isrsafe
                 */
                void notifyFromIsr();

                /**
                 * @brief Returns true if a semaphore is in the list.
                 * @isrsafe
                 */
                bool hasWaiters() const;

                /** @brief `add()` for a scope; `remove()` at its end. */
                class Entry {
                        public:
                                Entry(WaitList &list, Semaphore &semaphore) : _list(list), _slot(list.add(semaphore)) {}
                                ~Entry() { _list.remove(_slot); }
                                Entry(const Entry &) = delete;
                                Entry &operator=(const Entry &) = delete;

                                /** @brief Returns true if it got a slot. */
                                bool isAdded() const { return _slot != NoSlot; }

                        private:
                                WaitList &_list;
                                size_t    _slot;
                };

        protected:
                constexpr WaitList(std::atomic<Semaphore *> *slots, size_t count) : _slots(slots), _count(count) {}
                ~WaitList() = default;

        private:
                void release(bool fromIsr);

                std::atomic<Semaphore *> *_slots;
                size_t                    _count;
                std::atomic<uint32_t>     _busy{0}; // notifiers currently releasing
};

/** @brief A `WaitList` of @p N slots. */
template <size_t N> class WaitListOf final : public WaitList {
        public:
                constexpr WaitListOf() : WaitList(_storage, N) {}

        private:
                std::atomic<Semaphore *> _storage[N] = {};
};

BMDS_NAMESPACE_END
