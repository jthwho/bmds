/**
 * @file      programmemory.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief A U-mode program's memory (`devplan/protection.md` §15): where its blocks are made,
 *        with their records kept out of its reach. The loader's (`UserMemory`); a `Process`
 *        allocates from it instead of the kernel's heap while it has one, and a swap chain
 *        makes a U-mode program's surface buffers there.
 *
 * Each block has an owner: a process's own id (`Process::memoryOwner()`; a vfork child
 * allocates from its parent's memory under its own), or `KernelOwner` (what the kernel makes in a
 * program's memory for it: a program can't free those). Reference-counted: whoever keeps a
 * pointer to it keeps a reference.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class ProgramMemory {
        public:
                /** @brief The kernel's blocks in a program's memory. */
                static constexpr uint32_t KernelOwner = 0;

                /** @brief `alloc()`'s flags: `mem_alloc`'s (`BMDS_MEM_FAST`, `BMDS_MEM_DMA`). */
                enum Flags : uint32_t {
                        Fast = 0x1, ///< Internal RAM if it can be had.
                        Dma = 0x2,  ///< Whole cache lines, for DMA.
                };

                virtual void ref() = 0;
                virtual void unref() = 0;

                /**
                 * @brief A block of @p bytes aligned to @p align (4096 at most) for @p owner;
                 *        @p flags `Fast` (internal RAM if it can, else PSRAM, as a hint can be),
                 *        `Dma`.
                 * @return The block, or null.
                 */
                virtual void *alloc(uint32_t bytes, uint32_t align, uint32_t flags, uint32_t owner) = 0;

                /** @brief Frees @p owner's block at @p p: its size, or 0 (not one). */
                virtual uint32_t free(void *p, uint32_t owner) = 0;

                /**
                 * @brief Resizes @p owner's block at @p p (in place if it can, else a new one and a
                 *        copy), its contents kept up to the smaller size.
                 * @return The block, or null (nothing changed).
                 */
                virtual void *realloc(void *p, uint32_t bytes, uint32_t flags, uint32_t owner) = 0;

                /** @brief The size of @p owner's block at @p p, or 0. */
                virtual uint32_t sizeOf(const void *p, uint32_t owner) = 0;

                /** @brief True if @p bytes at @p p lie in one block of @p owner (a walk). */
                virtual bool contains(const void *p, size_t bytes, uint32_t owner) = 0;

                /** @brief Frees every block of @p owner. */
                virtual void freeOwner(uint32_t owner) = 0;

                /**
                 * @brief How many bytes from @p p the program could read (or with @p write, write)
                 *        itself, up to the end of the region holding @p p: 0 if none
                 *        (`devplan/protection.md` §16). The PMP's own rule.
                 */
                virtual size_t accessible(const void *p, bool write) = 0;

                /** @brief True if the program could make each access to [@p p, @p p + @p bytes). */
                bool isAccessible(const void *p, size_t bytes, bool write) {
                        auto at = reinterpret_cast<uintptr_t>(p);
                        if (bytes == 0) return true;
                        if (at + bytes < at) return false;
                        while (bytes > 0) {
                                const size_t n = accessible(reinterpret_cast<const void *>(at), write);
                                if (n == 0) return false;
                                if (n >= bytes) return true;
                                at += n;
                                bytes -= n;
                        }
                        return true;
                }

                /**
                 * @brief The length of the NUL-ended string at @p s, measured only in memory the
                 *        program could read: `BadAddress` if it runs into memory it couldn't,
                 *        `OutOfRange` if longer than @p max.
                 */
                Result<size_t> stringLength(const char *s, size_t max) {
                        size_t n = 0;
                        for (;;) {
                                const char  *at = s + n;
                                const size_t room = accessible(at, false);
                                if (room == 0) return makeError<size_t>(Error::BadAddress);
                                const size_t look = room < max + 1 - n ? room : max + 1 - n;
                                for (size_t i = 0; i < look; ++i)
                                        if (at[i] == '\0') return makeResult(n + i);
                                n += look;
                                if (n > max) return makeError<size_t>(Error::OutOfRange);
                        }
                }

        protected:
                ProgramMemory() = default;
                ~ProgramMemory() = default;
};

BMDS_NAMESPACE_END
