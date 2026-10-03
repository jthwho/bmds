/**
 * @file      bytering.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Lock-free single-producer / single-consumer byte FIFO.
 *
 * Built on caller-provided storage whose size is a power of two. The
 * indices run freely and are masked on access, so all `capacity()` bytes
 * are usable. One side may be an ISR and the other a task, on either core;
 * the indices are published with release/acquire ordering.
 *
 * Exactly one producer (`push()`, `writable()`, `produce()`) and one
 * consumer (`pop()`, `readable()`, `consume()`) at a time. If several
 * tasks write or read, serialize them externally (e.g. with a `Mutex`).
 *
 * @par Thread Safety
 * Safe for one producer and one consumer running concurrently. Every
 * method is `@isrsafe` and `@nonblocking`.
 */
class ByteRing {
        public:
                /**
                 * @brief Creates a ring over @p storage.
                 * @param storage Backing bytes; the size must be a non-zero power of two.
                 */
                explicit ByteRing(WritableByteSpan storage);

                ByteRing(const ByteRing &) = delete;
                ByteRing &operator=(const ByteRing &) = delete;

                /** @brief Returns the total capacity in bytes. */
                size_t capacity() const { return _mask + 1; }

                /** @brief Returns the number of bytes waiting to be read. */
                size_t available() const;

                /** @brief Returns the number of bytes that can be written now. */
                size_t freeSpace() const { return capacity() - available(); }

                /** @brief Returns true if nothing is waiting to be read. */
                bool isEmpty() const { return available() == 0; }

                /**
                 * @brief Copies as much of @p data in as fits (producer).
                 * @return The number of bytes written (0 if full).
                 */
                size_t push(ByteSpan data);

                /**
                 * @brief Copies out up to `buf.size()` bytes (consumer).
                 * @return The number of bytes read (0 if empty).
                 */
                size_t pop(WritableByteSpan buf);

                /**
                 * @brief Returns the largest contiguous run of waiting bytes (consumer).
                 *
                 * Read it in place, then call `consume()`. A second call after
                 * consuming returns the part that wrapped, if any.
                 */
                ByteSpan readable() const;

                /** @brief Discards @p count waiting bytes, at most `available()` (consumer). */
                void consume(size_t count);

                /**
                 * @brief Returns the largest contiguous run of free space (producer).
                 *
                 * Fill it in place, then call `produce()`.
                 */
                WritableByteSpan writable();

                /** @brief Publishes @p count bytes written into `writable()` (producer). */
                void produce(size_t count);

        private:
                uint8_t              *_data;
                uint32_t              _mask;
                std::atomic<uint32_t> _head{0}; // written by the producer
                std::atomic<uint32_t> _tail{0}; // written by the consumer
};

BMDS_NAMESPACE_END
