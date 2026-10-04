/**
 * @file      stream.h
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
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class FileHandle;

/**
 * @brief A byte stream: what a file descriptor ultimately refers to.
 *
 * Consoles, pipes, memory buffers, `/dev/null` and (Phase 2) files are
 * streams. Commands reach them through a `FdTable` of `FileHandle`s, never
 * directly.
 *
 * Semantics shared by every stream:
 * - `read()` waits until at least one byte is available, then returns what
 *   is there (up to the buffer size). At the end of the stream it returns
 *   0 bytes with `Error::EndOfFile`.
 * - `write()` waits until all of @p data is taken, or fails part-way with
 *   the count written so far and the error: `Timeout`, `Interrupted`, or
 *   `BrokenPipe` when nobody will ever read it.
 * - Blocking streams return `Error::Interrupted` when the interrupt they
 *   were created with (the job's) is raised.
 *
 * Kernel-internal; programs see file descriptors (Phase 3 ABI).
 *
 * @par Thread Safety
 * Defined by each stream; consoles and pipes are thread-safe.
 */
class Stream {
        public:
                Stream(const Stream &) = delete;
                Stream &operator=(const Stream &) = delete;

                /**
                 * @brief Reads at least one byte, waiting up to @p timeoutMs.
                 * @return Bytes read; 0 with `EndOfFile` at the end; or an error.
                 * @blocking
                 */
                virtual Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) = 0;

                /**
                 * @brief Writes all of @p data, waiting up to @p timeoutMs.
                 * @return Bytes written; less than `data.size()` only with an error.
                 * @blocking
                 */
                virtual Result<size_t> write(ByteSpan data, unsigned int timeoutMs) = 0;

                /**
                 * @brief Waits until buffered output has gone out.
                 * @blocking
                 */
                virtual Error flush(unsigned int timeoutMs) {
                        (void)timeoutMs;
                        return Error::Ok;
                }

                /** @brief Where `seek()` counts from. */
                enum class SeekFrom : uint8_t {
                        Start,   ///< From the beginning.
                        Current, ///< From the current position.
                        End,     ///< From the end.
                };

                /**
                 * @brief Moves the read/write position (files only).
                 *
                 * `seek(0, SeekFrom::Current)` tells the position;
                 * `seek(0, SeekFrom::End)` the size. A file opened for
                 * writing may be positioned past its end; it grows when
                 * written there.
                 * @return The new position from the start, or `NotSupported`
                 *         (pipes, consoles), `Invalid` (before the start), or the
                 *         stream's error.
                 */
                virtual Result<uint64_t> seek(int64_t offset, SeekFrom from) {
                        (void)offset;
                        (void)from;
                        return makeError<uint64_t>(Error::NotSupported);
                }

                /**
                 * @brief Sets the size of a file: cut, or grown with zeros. The
                 *        position doesn't change (it may be past the new end).
                 * @return `Ok`, or `NotSupported` (pipes, consoles, and streams
                 *         that aren't files), `NoSpace`, or the stream's error.
                 */
                virtual Error truncate(uint64_t size) {
                        (void)size;
                        return Error::NotSupported;
                }

                /** @brief Returns true if this is an interactive terminal (a console). */
                virtual bool isTerminal() const { return false; }

                /** @brief Writes @p text (see `write(ByteSpan, unsigned int)`). */
                Result<size_t> write(StringView text, unsigned int timeoutMs) {
                        return write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), timeoutMs);
                }

                /**
                 * @brief Called by FileHandle when the last handle opened on this stream with @p mode goes away.
                 *
                 * A pipe learns here that its reader or writer is gone, and
                 * may free itself; most streams ignore it. Only FileHandle
                 * calls this.
                 * @param mode The handle's `FileHandle::Mode` bits.
                 */
                virtual void handleClosed(uint8_t mode) {
                        (void)mode;
                        return;
                }

        protected:
                constexpr Stream() = default;
                ~Stream() = default;
};

BMDS_NAMESPACE_END
