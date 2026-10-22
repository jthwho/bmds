/**
 * @file      streampair.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/bytering.h>
#include <bmds/config.h>
#include <bmds/filehandle.h>
#include <bmds/interrupt.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A two-way byte channel between tasks: two ends, each reading what the other writes
 *        (`devplan/guiprograms.md` §8: a GUI connection's local transport).
 *
 * Each direction is a ring as a `Pipe`'s, and each end behaves as both ends of a pipe at once:
 * - a read waits while nothing has come and the other end is open; once the other end is closed
 *   and what it wrote is drained, `read()` returns `EndOfFile`;
 * - a write waits while the ring towards the other end is full; once the other end is closed,
 *   `write()` fails with `BrokenPipe`;
 * - both return `Interrupted` when the calling task's interrupt is raised;
 * - an end is readable while data waits or the other end is closed (`HungUp`), writable while
 *   there is room or the other end is closed (`Broken`).
 *
 * The two ends and both rings are one `Psram` allocation, freed when both ends are closed.
 *
 * @par Thread Safety
 * Thread-safe. Concurrent readers (or writers) of one end are serialized.
 */
class StreamPair {
        public:
                /** @brief Default ring size in bytes, each way. */
                static constexpr size_t DefaultBytes = 16384;

                /** @brief The two ends, each `FileHandle::ReadWrite`. */
                struct Ends {
                                FileHandle::Ptr first;
                                FileHandle::Ptr second;
                };

                /**
                 * @brief Creates a pair.
                 * @param bytes Each ring's size, a power of two.
                 * @return The ends, `Invalid` for a bad size, or `NoMem`.
                 */
                static Result<Ends> create(size_t bytes = DefaultBytes);

        private:
                class End;
                struct Shared;
};

BMDS_NAMESPACE_END
