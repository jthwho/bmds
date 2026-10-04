/**
 * @file      filehandle.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An open stream with an access mode: POSIX's "open file description".
 *
 * Every file descriptor that refers to the same open (after `dup`, `2>&1`,
 * or a copied `FdTable`) shares one FileHandle, reference-counted through
 * `FileHandle::Ptr`. When the last reference goes, the stream is told
 * (`Stream::handleClosed()`); that is what gives a pipe's reader its EOF.
 *
 * Handles are allocated from `MemSpace::Psram` when opened, never on a hot
 * path. The FileHandle does not own its stream: pipes manage their own
 * lifetime through `handleClosed()`, and consoles are permanent.
 *
 * @par Thread Safety
 * Thread-safe reference counting; I/O thread safety is the stream's.
 */
class FileHandle {
        public:
                /** @brief Access mode bits. */
                enum Mode : uint8_t {
                        Read = 1u << 0,          ///< May be read.
                        Write = 1u << 1,         ///< May be written.
                        ReadWrite = Read | Write ///< Both.
                };

                /**
                 * @brief A counted reference to a FileHandle.
                 *
                 * Copying adds a reference (it can't fail); destroying or
                 * `reset()` drops one.
                 */
                class Ptr {
                        public:
                                /** @brief Constructs a null reference. */
                                constexpr Ptr() = default;

                                /** @brief Adds a reference to @p other's handle. */
                                Ptr(const Ptr &other) : _handle(other._handle) {
                                        if (_handle != nullptr) _handle->ref();
                                }

                                /** @brief Takes over @p other's reference. */
                                Ptr(Ptr &&other) : _handle(std::exchange(other._handle, nullptr)) {}

                                /** @brief Drops this reference, then adds one to @p other's handle. */
                                Ptr &operator=(const Ptr &other) {
                                        Ptr copy(other);
                                        std::swap(_handle, copy._handle);
                                        return *this;
                                }

                                /** @brief Drops this reference and takes over @p other's. */
                                Ptr &operator=(Ptr &&other) {
                                        Ptr taken(std::move(other));
                                        std::swap(_handle, taken._handle);
                                        return *this;
                                }

                                /** @brief Drops the reference. */
                                ~Ptr() { reset(); }

                                /** @brief Drops the reference, leaving this null. */
                                void reset() {
                                        if (_handle != nullptr) std::exchange(_handle, nullptr)->unref();
                                        return;
                                }

                                /** @brief Returns the handle, or null. */
                                FileHandle *get() const { return _handle; }

                                /** @brief Accesses the handle; must not be null. */
                                FileHandle *operator->() const { return _handle; }

                                /** @brief Returns the handle; must not be null. */
                                FileHandle &operator*() const { return *_handle; }

                                /** @brief Returns true if not null. */
                                explicit operator bool() const { return _handle != nullptr; }

                        private:
                                friend class FileHandle;
                                explicit Ptr(FileHandle *handle) : _handle(handle) {}

                                FileHandle *_handle = nullptr;
                };

                FileHandle(const FileHandle &) = delete;
                FileHandle &operator=(const FileHandle &) = delete;

                /**
                 * @brief Opens @p stream with @p mode.
                 * @param stream The stream; must outlive the handle (see class notes).
                 * @param mode   `Mode` bits; at least one, no others.
                 * @return The first reference, `Invalid` for a bad mode, or `NoMem`.
                 */
                static Result<Ptr> open(Stream &stream, uint8_t mode);

                /** @brief Returns the stream. */
                Stream &stream() const { return *_stream; }

                /** @brief Returns the `Mode` bits. */
                uint8_t mode() const { return _mode; }

                /** @brief Returns true if the handle was opened for reading. */
                bool canRead() const { return (_mode & Read) != 0; }

                /** @brief Returns true if the handle was opened for writing. */
                bool canWrite() const { return (_mode & Write) != 0; }

                /** @brief Returns the number of references (for tests and diagnostics). */
                uint32_t refCount() const { return _refs.load(std::memory_order_relaxed); }

                /**
                 * @brief Reads from the stream; `BadDescriptor` if not opened for reading.
                 * @blocking
                 */
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) {
                        if (!canRead()) return makeError<size_t>(Error::BadDescriptor);
                        return _stream->read(buf, timeoutMs);
                }

                /**
                 * @brief Writes to the stream; `BadDescriptor` if not opened for writing.
                 * @blocking
                 */
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) {
                        if (!canWrite()) return makeError<size_t>(Error::BadDescriptor);
                        return _stream->write(data, timeoutMs);
                }

                /** @copydoc write(ByteSpan, unsigned int) */
                Result<size_t> write(StringView text, unsigned int timeoutMs) {
                        return write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), timeoutMs);
                }

        private:
                FileHandle(Stream &stream, uint8_t mode) : _stream(&stream), _mode(mode) {}
                ~FileHandle() = default;

                void ref() {
                        _refs.fetch_add(1, std::memory_order_relaxed);
                        return;
                }
                void unref();

                std::atomic<uint32_t> _refs{1};
                Stream               *_stream;
                uint8_t               _mode;
};

BMDS_NAMESPACE_END
