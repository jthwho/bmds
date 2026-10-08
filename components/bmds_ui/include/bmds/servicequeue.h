/**
 * @file      servicequeue.h
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
#include <bmds/list.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The messages sent to a window manager service and not yet taken
 *        (`devplan/filebrowser.md` §4h, FB6), oldest first, pure.
 *
 * Each is kept as two bytes of length and its bytes, in one PSRAM list that
 * grows as needed, never past `MaxPending` bytes.
 *
 * @par Thread Safety
 * A value with one owner (the window manager's lock).
 */
class ServiceQueue {
        public:
                /** @brief The longest message. */
                static constexpr size_t MaxMessage = 1024;

                /** @brief The bytes of messages (and their lengths) kept at most. */
                static constexpr size_t MaxPending = 4096;

                /**
                 * @brief Adds @p message after the others.
                 * @return `Ok`, `Invalid` (empty, or longer than `MaxMessage`), `Full`
                 *         (past `MaxPending`) or `NoMem`.
                 */
                Error push(ByteSpan message);

                /**
                 * @brief Moves the oldest message into @p out.
                 * @return Its bytes (0: none was waiting), or `BufferTooSmall` (it is
                 *         kept).
                 */
                Result<size_t> take(WritableByteSpan out);

                /** @brief Returns true if no message is waiting. */
                bool isEmpty() const { return _bytes.isEmpty(); }

                /** @brief Returns the bytes kept (messages and lengths). */
                size_t pendingBytes() const { return _bytes.size(); }

                /** @brief Forgets every message. */
                void clear() { _bytes.clear(); }

        private:
                List<uint8_t> _bytes{MemSpace::Psram};
};

BMDS_NAMESPACE_END
