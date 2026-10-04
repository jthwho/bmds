/**
 * @file      process.h
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
#include <bmds/fdtable.h>
#include <bmds/memspace.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A running program's resources and its system calls.
 *
 * Everything a program gets from the kernel belongs to its Process: its
 * file descriptors (a copy of the shell's, so closing one doesn't close
 * the shell's), the memory it allocates, its argument and environment
 * block. When the program ends, however it ends, `release()` returns all
 * of it, so a program can't leak past its exit.
 *
 * The methods are the system calls behind the SDK's table (bmds/sys.h):
 * same arguments, results >= 0 or a negated `BMDS_E...` number.
 *
 * @par Thread Safety
 * A program has one thread, so a Process is used by its program's task
 * only (and by the loader before it starts and after it ends).
 */
class Process {
        public:
                /** @brief Largest alignment `memAlloc()` accepts. */
                static constexpr uint32_t MaxAlign = 4096;

                /** @brief Constructs a process with its own copy of @p fds. */
                explicit Process(const FdTable &fds);
                ~Process();
                Process(const Process &) = delete;
                Process &operator=(const Process &) = delete;

                /**
                 * @brief Builds the argument block: `argv` (with its NULL) and `envp`
                 *        ("NAME=value" strings, NULL-terminated), in one allocation.
                 * @return `Ok`, `NoMem`, or `Invalid` (too many or too long).
                 */
                Error setArguments(Span<const StringView> args, Span<const StringView> environment);

                /** @brief Returns the program's `argc`. */
                int32_t argc() const { return _argc; }

                /** @brief Returns its `argv`. */
                char **argv() const { return _argv; }

                /** @brief Returns its `envp`. */
                char **envp() const { return _envp; }

                /** @brief Returns its file descriptors. */
                FdTable &fds() { return _fds; }

                /** @brief Returns the bytes it has allocated and not freed. */
                size_t allocatedBytes() const { return _allocatedBytes; }

                /** @brief Returns how many allocations it holds. */
                size_t allocations() const { return _allocations; }

                /** @brief Closes its fds and frees everything it allocated. Idempotent. */
                void release();

                // --- System calls (bmds/sys.h) ------------------------------------

                int32_t read(int32_t fd, void *buf, uint32_t size);
                int32_t write(int32_t fd, const void *buf, uint32_t size);
                int32_t close(int32_t fd);
                int32_t seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position);
                int32_t isatty(int32_t fd);
                void   *memAlloc(uint32_t size, uint32_t align, uint32_t flags);
                void    memFree(void *ptr);
                void   *memRealloc(void *ptr, uint32_t size, uint32_t flags);

                /** @brief Returns the negated `BMDS_E...` number for @p err. */
                static int32_t errnoFor(Error err);

        private:
                struct Block; // an allocation's header, just before what the program gets

                FdTable _fds;
                Block  *_blocks = nullptr;
                size_t  _allocatedBytes = 0;
                size_t  _allocations = 0;
                void   *_argBlock = nullptr;
                char  **_argv = nullptr;
                char  **_envp = nullptr;
                int32_t _argc = 0;
};

BMDS_NAMESPACE_END
