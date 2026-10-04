/**
 * @file      pipe.h
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
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A one-way byte channel between tasks: the `|` in a pipeline.
 *
 * `create()` returns two FileHandles: the read end and the write end. The
 * pipe lives until both are closed (their last references dropped), then
 * frees itself.
 *
 * - A reader waits while the pipe is empty and the write end is open; once
 *   the write end is closed and the data drained, `read()` returns
 *   `EndOfFile`.
 * - A writer waits while the pipe is full and the read end is open; once
 *   the read end is closed, `write()` fails with `BrokenPipe`.
 * - Both return `Interrupted` when the job's interrupt is raised.
 *
 * The pipe and its ring are one `Psram` allocation; there is no other
 * per-pipe cost.
 *
 * @par Thread Safety
 * Thread-safe. Concurrent readers (or writers) are serialized.
 */
class Pipe final : public Stream {
        public:
                /** @brief Default ring size in bytes. */
#if defined(CONFIG_BMDS_PIPE_BYTES)
                static constexpr size_t DefaultBytes = CONFIG_BMDS_PIPE_BYTES;
#else
                static constexpr size_t DefaultBytes = 4096;
#endif

                /** @brief The two ends of a new pipe. */
                struct Ends {
                                FileHandle::Ptr read;  ///< Read end (`FileHandle::Read`).
                                FileHandle::Ptr write; ///< Write end (`FileHandle::Write`).
                };

                /**
                 * @brief Creates a pipe.
                 * @param interrupt The job's interrupt (may be null: not interruptible).
                 * @param bytes     Ring size, a power of two.
                 * @return The ends, `Invalid` for a bad size, or `NoMem`.
                 */
                static Result<Ends> create(Interrupt *interrupt, size_t bytes = DefaultBytes);

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;

                using Stream::write;

                /** @brief Returns the bytes waiting to be read. */
                size_t available() const { return _ring.available(); }

                void handleClosed(uint8_t mode) override;

        private:
                Pipe(WritableByteSpan storage, Interrupt *interrupt);
                ~Pipe() = default;

                void endClosed();

                ByteRing             _ring;
                Interrupt           *_interrupt;
                Mutex                _readLock;
                Mutex                _writeLock;
                Semaphore            _readable{1, 0}; // released when data arrives or the writer goes
                Semaphore            _writable{1, 0}; // released when space frees or the reader goes
                std::atomic<bool>    _readerOpen{true};
                std::atomic<bool>    _writerOpen{true};
                std::atomic<uint8_t> _openEnds{2};
};

BMDS_NAMESPACE_END
