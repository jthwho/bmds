/**
 * @file      flashqueue.h
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
#include <bmds/memspace.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An ordered write cache for a NOR flash: erases and programs queued
 *        in a ring, done later in the same order (devplan/storage.md §13d).
 *
 * Keeping the order is what makes it safe under littlefs: the flash only
 * ever shows a state littlefs itself passed through, and a power cut loses
 * the queue's tail, as a cut at an earlier operation would. Nothing is
 * merged or reordered.
 *
 * Reads see the queue through `overlay()`: what the flash holds, then each
 * queued operation over it, oldest first. The owner takes `front()`, does
 * it on the flash, then `pop()`s it; a read between the two applies it
 * twice, which gives the same bytes.
 *
 * Records don't straddle the ring's end: one that doesn't fit before it
 * starts again at the front, the gap counted as used until it is passed.
 *
 * @par Thread Safety
 * Not thread-safe: the owner serializes every call. The data of the record
 * `front()` returned stays where it is until `pop()`, so the owner may do
 * it on the flash without holding its lock.
 */
class FlashQueue {
        public:
                /** @brief What a record does. */
                enum class Kind : uint8_t {
                        Erase,   ///< Sets `length` bytes at `address` to 0xFF.
                        Program, ///< Clears the bits of `data` that are 0, at `address`.
                };

                /** @brief A queued operation, as `front()` shows it. */
                struct Op {
                                Kind           kind = Kind::Erase;
                                uint32_t       address = 0;
                                uint32_t       length = 0;
                                const uint8_t *data = nullptr; ///< `Program`: `length` bytes, in the ring.
                };

                /** @brief The bytes a record takes before its data. */
                static constexpr size_t HeaderBytes = 12;

                constexpr FlashQueue() = default;
                ~FlashQueue();
                FlashQueue(const FlashQueue &) = delete;
                FlashQueue &operator=(const FlashQueue &) = delete;

                /**
                 * @brief Allocates a ring of @p bytes (a multiple of 4, at least 4 KB) in @p space.
                 * @return `Ok`, `Invalid`, `InvalidState` (done before), or `NoMem`.
                 */
                Error init(size_t bytes, MemSpace space);

                /** @brief Returns the largest program one record holds (a quarter of the ring). */
                size_t maxProgram() const { return _capacity / 4; }

                /**
                 * @brief Queues an erase of @p length bytes at @p address.
                 * @return `Ok`, or `Full` (no room: nothing queued).
                 * @nonblocking
                 */
                Error tryErase(uint32_t address, uint32_t length);

                /**
                 * @brief Queues a program of @p data at @p address (at most `maxProgram()` bytes).
                 * @return `Ok`, `Full` (no room: nothing queued), or `Invalid` (too big or empty).
                 * @nonblocking
                 */
                Error tryProgram(uint32_t address, ByteSpan data);

                /** @brief Applies every queued operation that overlaps @p buf (the bytes at @p address), oldest first. */
                void overlay(uint32_t address, WritableByteSpan buf) const;

                /** @brief Sets @p op to the oldest operation; false if there is none. */
                bool front(Op &op) const;

                /** @brief Removes the oldest operation (after it was done). */
                void pop();

                /** @brief Returns true when nothing is queued. */
                bool isEmpty() const { return _count == 0; }

                /** @brief Returns how many operations are queued. */
                size_t count() const { return _count; }

                /** @brief Returns true when at least half the ring is taken. */
                bool isHalfFull() const { return _used * 2 >= _capacity; }

        private:
                // The record's size in the ring, its data padded to 4 bytes.
                static size_t recordBytes(uint32_t length) { return HeaderBytes + ((length + 3u) & ~size_t(3)); }

                // The offset of the record at or after @p at, past a gap at the ring's end.
                size_t skipGap(size_t at) const;

                Error append(Kind kind, uint32_t address, uint32_t length, const uint8_t *data);

                uint8_t *_ring = nullptr;
                size_t   _capacity = 0;
                size_t   _head = 0;  // the oldest record
                size_t   _tail = 0;  // where the next one goes
                size_t   _used = 0;  // bytes taken, gaps at the end included
                size_t   _count = 0; // records
};

BMDS_NAMESPACE_END
