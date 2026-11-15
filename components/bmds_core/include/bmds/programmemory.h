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

        protected:
                ProgramMemory() = default;
                ~ProgramMemory() = default;
};

BMDS_NAMESPACE_END
