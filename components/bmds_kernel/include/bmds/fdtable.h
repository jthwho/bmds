/**
 * @file      fdtable.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/result.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A file descriptor table: small integers mapped to FileHandles.
 *
 * Each job gets its own table, copied from the shell's when a command
 * starts. Copying never fails and never allocates: it adds one reference
 * per open descriptor. Redirections are applied to the copy, so the shell's
 * own table is never disturbed.
 *
 * Descriptors 0, 1 and 2 are stdin, stdout and stderr by convention.
 *
 * @par Thread Safety
 * Not thread-safe; a table belongs to one task. Copies may be handed to
 * other tasks.
 */
class FdTable {
        public:
                /** @brief Number of descriptors (0 .. MaxFds-1). */
                static constexpr int MaxFds = 16;

                /** @brief Standard input. */
                static constexpr int Stdin = 0;

                /** @brief Standard output. */
                static constexpr int Stdout = 1;

                /** @brief Standard error. */
                static constexpr int Stderr = 2;

                /** @brief Constructs a table with every descriptor closed. */
                FdTable() = default;

                /**
                 * @brief Returns the handle on @p fd.
                 * @return The handle, or null if @p fd is closed or out of range.
                 */
                FileHandle *handle(int fd) const { return isValid(fd) ? _fds[fd].get() : nullptr; }

                /** @brief Returns true if @p fd is open. */
                bool isOpen(int fd) const { return handle(fd) != nullptr; }

                /**
                 * @brief Puts @p handle on @p fd, closing what was there.
                 * @param handle A reference (null just closes @p fd).
                 * @return `Ok`, or `BadDescriptor` if @p fd is out of range.
                 */
                Error set(int fd, FileHandle::Ptr handle);

                /**
                 * @brief Puts @p handle on the lowest closed descriptor at or above @p minFd.
                 * @return The descriptor, `BadDescriptor` if @p minFd is out of range,
                 *         or `TooManyOpen`.
                 */
                Result<int> add(FileHandle::Ptr handle, int minFd = 0);

                /**
                 * @brief Makes @p to refer to the same handle as @p from (`to>&from`).
                 * @return `Ok`, or `BadDescriptor` if either is out of range or @p from is closed.
                 */
                Error dup2(int from, int to);

                /**
                 * @brief Closes @p fd.
                 * @return `Ok`, or `BadDescriptor` if out of range or already closed.
                 */
                Error close(int fd);

                /** @brief Closes every descriptor. */
                void closeAll();

        private:
                static bool isValid(int fd) { return fd >= 0 && fd < MaxFds; }

                FileHandle::Ptr _fds[MaxFds];
};

BMDS_NAMESPACE_END
