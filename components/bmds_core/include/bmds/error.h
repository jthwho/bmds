/**
 * @file      error.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Lightweight error code.
 *
 * A single byte holding a `Code`. Returned by value from any operation that
 * can fail; `Result<T>` pairs it with a value. Name and description strings
 * live in rodata.
 *
 * `Error` is `[[nodiscard]]`: ignoring a returned error is a compile error
 * under `-Werror`. Discard explicitly with `(void)` when that is really
 * intended.
 *
 * Kernel-internal: the numeric values are not part of the userspace ABI
 * (the SDK defines its own stable error constants).
 *
 * @par Thread Safety
 * Distinct instances may be used concurrently. Trivially copyable.
 *
 * @par Example
 * @code
 * Error err = device.open();
 * if (err.isError()) {
 *         bmdsErr("Device(%p): open failed: %s", &device, err.name());
 *         return err;
 * }
 * @endcode
 */
class [[nodiscard]] Error {
        public:
                /**
                 * @brief Error codes.
                 *
                 * Add a new code whenever an existing one would be an
                 * imprecise fit. Keep `name()` / `desc()` tables in
                 * error.cpp in sync (a static_assert checks the count).
                 */
                enum Code : uint8_t {
                        Ok = 0,           ///< No error.
                        Invalid,          ///< Invalid value or argument (EINVAL).
                        NoMem,            ///< Out of memory (ENOMEM).
                        Timeout,          ///< Operation timed out (ETIMEDOUT).
                        WouldBlock,       ///< Operation would block (EAGAIN); returned by `try*()` calls.
                        Busy,             ///< Resource is busy (EBUSY).
                        NotExist,         ///< Resource does not exist (ENOENT).
                        Exists,           ///< Resource already exists (EEXIST).
                        NotSupported,     ///< Operation is not supported (ENOTSUP).
                        IoError,          ///< Input/output error (EIO).
                        NoSpace,          ///< No space left on device (ENOSPC).
                        Full,             ///< Container or queue is full.
                        Empty,            ///< Container or queue is empty.
                        InvalidState,     ///< Object is not in a state that allows the operation.
                        NotReady,         ///< Resource exists but is not ready yet.
                        OutOfRange,       ///< Value or index is out of range (ERANGE).
                        BufferTooSmall,   ///< Supplied buffer is too small.
                        PermissionDenied, ///< Permission denied (EACCES / EPERM).
                        NotImplemented,   ///< Feature is not implemented (yet).
                        EndOfFile,        ///< End of file / stream reached.
                        Cancelled,        ///< Operation was cancelled.
                        CorruptData,      ///< Data is structurally corrupt / failed a checksum.
                        DeviceError,      ///< Hardware device reported an error.
                        NotFound,         ///< Requested element was not found in a container.
                        BadDescriptor,    ///< Bad file or object descriptor (EBADF).
                        TooManyOpen,      ///< Too many open files or handles (EMFILE / ENFILE).
                        IsDir,            ///< Is a directory (EISDIR).
                        NotDir,           ///< Not a directory (ENOTDIR).
                        NotEmpty,         ///< Directory not empty (ENOTEMPTY).
                        ReadOnly,         ///< Read-only filesystem or object (EROFS).
                        Interrupted,      ///< Interrupted (EINTR).
                        LibraryFailure,   ///< An underlying library (e.g. ESP-IDF) failed without a specific mapping.
                        UnsupportedSystemError, ///< A system (errno) error with no specific mapping.
                        BrokenPipe,             ///< Write with no reader left (EPIPE).
                        CodeCount               ///< Number of codes (not an error code).
                };

                /**
                 * @brief Translates an ESP-IDF `esp_err_t` value.
                 *
                 * Takes the value as `int32_t` so this header needs no IDF
                 * includes; the numeric constants are checked against
                 * `esp_err.h` in the kernel build.
                 * @param espErr The `esp_err_t` value.
                 * @return The matching Error, `LibraryFailure` if unmapped.
                 */
                static Error fromEspErr(int32_t espErr);

                /**
                 * @brief Translates the current value of `errno`.
                 * @return The matching Error, `UnsupportedSystemError` if unmapped.
                 */
                static Error syserr();

                /**
                 * @brief Translates an explicit errno value.
                 * @param errnum The errno value.
                 * @return The matching Error, `UnsupportedSystemError` if unmapped.
                 */
                static Error syserr(int errnum);

                /**
                 * @brief Constructs an Error.
                 * @param code The error code (default `Ok`).
                 */
                constexpr Error(Code code = Ok) : _code(code) {}

                /** @brief Compares two errors by code. */
                constexpr bool operator==(const Error &other) const = default;

                /** @brief Returns the error code. */
                constexpr Code code() const { return _code; }

                /** @brief Returns true if this is `Ok`. */
                constexpr bool isOk() const { return _code == Ok; }

                /** @brief Returns true if this is not `Ok`. */
                constexpr bool isError() const { return _code != Ok; }

                /**
                 * @brief Returns the code's identifier, e.g. `"NoMem"`.
                 * @return A static string (never null).
                 */
                const char *name() const;

                /**
                 * @brief Returns a short human-readable description, e.g. `"Out of memory"`.
                 * @return A static string (never null).
                 */
                const char *desc() const;

        private:
                Code _code;
};

static_assert(sizeof(Error) == 1, "Error must stay a single byte");

BMDS_NAMESPACE_END
