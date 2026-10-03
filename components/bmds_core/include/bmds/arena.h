/**
 * @file      arena.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <bmds/config.h>
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Bump allocator: many small allocations, freed all at once.
 *
 * Memory comes in chunks of `chunkBytes` from the arena's `MemSpace`; an
 * allocation larger than a chunk gets a chunk of its own. Nothing is
 * allocated until the first `alloc()`. Individual allocations are never
 * freed: `rewind()` to a `mark()`, `reset()`, or destruction releases them
 * together. Used for short-lived, tree-shaped data such as the shell's
 * syntax trees and expansion temporaries.
 *
 * Destructors of arena objects are never run, so `create()` accepts only
 * trivially destructible types.
 *
 * @par Thread Safety
 * Not thread-safe. Distinct instances may be used concurrently.
 *
 * @par Example
 * @code
 * Arena arena(MemSpace::Psram, 16 * 1024);
 * Node *node = arena.create<Node>();
 * if (node == nullptr) return Error::NoMem;
 * ...
 * arena.reset(); // every node at once
 * @endcode
 */
class Arena {
        private:
                struct Chunk;

        public:
                /** @brief Alignment used when none is given. */
                static constexpr size_t DefaultAlign = alignof(std::max_align_t);

                /** @brief A position in the arena, for `rewind()`. */
                struct Mark {
                                Chunk *chunk = nullptr; ///< Chunk in use at the mark (null: none yet).
                                size_t used = 0;        ///< Bytes used in that chunk at the mark.
                };

                /**
                 * @brief Constructs an empty arena (no allocation).
                 * @param space      Where chunks come from.
                 * @param chunkBytes Size of a regular chunk, including its small header.
                 */
                Arena(MemSpace space, size_t chunkBytes) : _space(space), _chunkBytes(chunkBytes) {}

                Arena(const Arena &) = delete;
                Arena &operator=(const Arena &) = delete;

                /** @brief Frees every chunk. */
                ~Arena() { release(); }

                /**
                 * @brief Allocates uninitialized memory.
                 * @param bytes Number of bytes (0 is treated as 1).
                 * @param align Alignment, a power of two.
                 * @return The memory, or null if out of memory.
                 */
                void *alloc(size_t bytes, size_t align = DefaultAlign);

                /**
                 * @brief Constructs a `T` in the arena.
                 * @return The object, or null if out of memory.
                 */
                template <typename T, typename... Args> T *create(Args &&...args) {
                        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
                        void *p = alloc(sizeof(T), alignof(T));
                        return p == nullptr ? nullptr : new (p) T(std::forward<Args>(args)...);
                }

                /**
                 * @brief Constructs @p count value-initialized `T`s in the arena.
                 * @return The first, or null if out of memory (or @p count is 0).
                 */
                template <typename T> T *createArray(size_t count) {
                        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
                        if (count == 0 || count > static_cast<size_t>(-1) / sizeof(T)) return nullptr;
                        void *p = alloc(sizeof(T) * count, alignof(T));
                        if (p == nullptr) return nullptr;
                        T *first = static_cast<T *>(p);
                        for (size_t i = 0; i < count; ++i) new (first + i) T();
                        return first;
                }

                /**
                 * @brief Copies @p text into the arena, NUL-terminated.
                 * @return A view of the copy (its `data()` is a C string), or `NoMem`.
                 */
                Result<StringView> copyString(StringView text);

                /** @brief Returns the current position, for `rewind()`. */
                Mark mark() const;

                /**
                 * @brief Releases everything allocated since @p mark.
                 *
                 * Chunks started after the mark are freed. @p mark must come
                 * from this arena and not predate an earlier rewind past it.
                 */
                void rewind(const Mark &mark);

                /** @brief Releases every allocation; keeps the first chunk (if regular-sized) for reuse. */
                void reset();

                /** @brief Releases every allocation and frees every chunk. */
                void release();

                /** @brief Returns the bytes handed out (including alignment padding). */
                size_t bytesUsed() const;

                /** @brief Returns the bytes held in chunks, including headers. */
                size_t bytesReserved() const;

                /** @brief Returns the number of chunks held. */
                size_t chunkCount() const;

        private:
                struct Chunk {
                                Chunk *next;
                                size_t size; // whole chunk, header included
                                size_t used; // offset of the first free byte, header included
                };

                void freeAfter(Chunk *chunk);

                Chunk   *_first = nullptr;
                Chunk   *_current = nullptr;
                MemSpace _space;
                size_t   _chunkBytes;
};

BMDS_NAMESPACE_END
