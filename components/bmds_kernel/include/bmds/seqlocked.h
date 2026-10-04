/**
 * @file      seqlocked.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <bmds/config.h>
#include <bmds/detail/platform.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A small value read often and written rarely, without a lock on
 *        the read side (a sequence lock).
 *
 * `load()` copies the value; if a writer was busy meanwhile, it copies
 * again. `store()`/`update()` serialize writers, and run with task
 * switching suspended on their core, so a reader can't preempt a writer
 * and spin on it. The value is kept as 32-bit atomic words (acquire loads,
 * release stores, no standalone fences, which ThreadSanitizer can't
 * check), so readers never race with writers in the C++ sense.
 *
 * `T` must be trivially copyable, a multiple of 4 bytes and free of
 * padding (`std::has_unique_object_representations`), so it can be built
 * at compile time (`constinit`) from its initial value.
 *
 * @par Thread Safety
 * Thread-safe in tasks. **Not ISR-safe:** an interrupt on the writer's core
 * that read the value would spin forever.
 */
template <typename T> class SeqLocked {
                static_assert(std::is_trivially_copyable_v<T>, "SeqLocked<T>: T must be trivially copyable");
                static_assert(sizeof(T) % 4 == 0, "SeqLocked<T>: sizeof(T) must be a multiple of 4");
                static_assert(std::has_unique_object_representations_v<T>, "SeqLocked<T>: T must have no padding");

                static constexpr size_t Words = sizeof(T) / 4;
                using Raw = std::array<uint32_t, Words>;

        public:
                /** @brief Holds @p initial. */
                constexpr explicit SeqLocked(const T &initial = T())
                    : SeqLocked(std::bit_cast<Raw>(initial), std::make_index_sequence<Words>()) {}

                SeqLocked(const SeqLocked &) = delete;
                SeqLocked &operator=(const SeqLocked &) = delete;

                /** @brief Returns a consistent copy of the value. @nonblocking */
                T load() const {
                        Raw raw;
                        for (;;) {
                                const uint32_t before = _sequence.load(std::memory_order_acquire);
                                if ((before & 1) == 0) {
                                        // Acquire: the check below can't move above these.
                                        for (size_t i = 0; i < Words; ++i)
                                                raw[i] = _words[i].load(std::memory_order_acquire);
                                        if (_sequence.load(std::memory_order_relaxed) == before) break;
                                }
                        }
                        return std::bit_cast<T>(raw);
                }

                /**
                 * @brief Replaces the value with @p fn(current), atomically for readers.
                 *
                 * @p fn runs with task switching suspended: it must be
                 * short and must not block, allocate or log.
                 * @return What @p fn returned (it may return void).
                 */
                template <typename Fn> decltype(auto) update(Fn &&fn) {
                        Detail::suspendScheduler();
                        // Writers on the other core: wait for an even sequence and take it.
                        uint32_t seq = _sequence.load(std::memory_order_relaxed);
                        for (;;) {
                                if ((seq & 1) == 0 &&
                                    _sequence.compare_exchange_weak(seq, seq + 1, std::memory_order_acquire,
                                                                    std::memory_order_relaxed))
                                        break;
                                seq = _sequence.load(std::memory_order_relaxed);
                        }
                        Raw raw;
                        for (size_t i = 0; i < Words; ++i) raw[i] = _words[i].load(std::memory_order_relaxed);
                        T value = std::bit_cast<T>(raw);
                        if constexpr (std::is_void_v<decltype(fn(value))>) {
                                fn(value);
                                publish(value, seq);
                                return;
                        } else {
                                decltype(auto) result = fn(value);
                                publish(value, seq);
                                return result;
                        }
                }

                /** @brief Replaces the value. */
                void store(const T &value) {
                        update([&](T &v) { v = value; });
                        return;
                }

        private:
                template <size_t... I>
                constexpr SeqLocked(const Raw &raw, std::index_sequence<I...>) : _words{raw[I]...} {}

                void publish(const T &value, uint32_t seq) {
                        const Raw raw = std::bit_cast<Raw>(value);
                        // Release: a reader that sees any new word also sees the odd sequence.
                        for (size_t i = 0; i < Words; ++i) _words[i].store(raw[i], std::memory_order_release);
                        _sequence.store(seq + 2, std::memory_order_release);
                        Detail::resumeScheduler();
                        return;
                }

                std::atomic<uint32_t> _sequence = 0;
                std::atomic<uint32_t> _words[Words];
};

BMDS_NAMESPACE_END
