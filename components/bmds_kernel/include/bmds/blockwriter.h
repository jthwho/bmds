/**
 * @file      blockwriter.h
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
#include <bmds/span.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Gathers a run of writes of any size into whole, aligned blocks of a file
 *        (devplan/storage.md §17).
 *
 * A body arriving from the network comes in pieces of any length at any offset. Written as they
 * come, each piece costs the card a read-modify-write of its partial sectors and a copy through
 * the SD driver's bounce buffer (its start isn't aligned for DMA): about a quarter more time for
 * 16 KB pieces. Gathered here, the file gets `BlockBytes` at a time from a `PsramDma` buffer, each
 * at an offset that is a multiple of `BlockBytes` (the first is shortened to reach one).
 *
 * Without memory for the buffer, writes go straight through. A failed write leaves the file
 * holding everything before the block that failed.
 *
 * @par Thread Safety
 * Not thread-safe; used by one task.
 */
class BlockWriter {
        public:
                /** @brief The size of a write, and the alignment of its offset. */
                static constexpr size_t BlockBytes = 64 * 1024;

                BlockWriter() = default;
                ~BlockWriter();

                BlockWriter(const BlockWriter &) = delete;
                BlockWriter &operator=(const BlockWriter &) = delete;

                /**
                 * @brief Starts over at file offset @p position (where the stream's next byte goes),
                 *        dropping anything gathered and not written.
                 */
                void reset(uint64_t position);

                /**
                 * @brief Takes @p data, writing each block to @p to as it fills (the buffer is
                 *        allocated at the first).
                 * @return `Ok`, or the stream's error (`IoError` if it took nothing).
                 * @blocking
                 */
                Error write(Stream &to, ByteSpan data);

                /**
                 * @brief Writes what is gathered to @p to.
                 * @return `Ok`, or the stream's error.
                 * @blocking
                 */
                Error flush(Stream &to);

        private:
                static Error writeAll(Stream &to, ByteSpan data);

                uint8_t *_buffer = nullptr;
                size_t   _used = 0;
                uint64_t _start = 0; ///< The file offset of `_buffer[0]`.
                bool     _noMemory = false;
};

BMDS_NAMESPACE_END
