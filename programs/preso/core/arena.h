/**
 * @file      arena.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Memory that is all given back at once: a deck's model lives in one, and a reload frees it
 * with the old deck. Chunks come from malloc (PSRAM on the board).
 */

#ifndef PRESO_ARENA_H
#define PRESO_ARENA_H

#include <new>
#include "types.h"

namespace preso {

        /**
         * @brief A bump allocator over malloc'd chunks, freed together.
         *
         * Objects made in it are never destroyed one by one: only types with nothing to
         * destroy belong in it.
         *
         * @par Thread Safety
         * Not thread-safe: one owner.
         */
        class Arena {
                public:
                        /** @brief Chunks of @p chunkSize bytes (bigger requests get one of their own). */
                        explicit Arena(uint32_t chunkSize = 16384) : _chunkSize(chunkSize) {}
                        ~Arena() { clear(); }
                        Arena(const Arena &) = delete;
                        Arena &operator=(const Arena &) = delete;

                        /** @brief @p size bytes aligned to @p align, or nullptr if out of memory. */
                        void *allocate(size_t size, size_t align);

                        /** @brief A @p T made with @p args, or nullptr if out of memory. */
                        template <typename T, typename... Args> T *make(Args... args) {
                                void *p = allocate(sizeof(T), alignof(T));
                                return p != nullptr ? new (p) T(args...) : nullptr;
                        }

                        /**
                         * @brief A copy of @p text, NUL-terminated (the view's length excludes the
                         *        NUL); an empty view if out of memory (and @p ok false).
                         */
                        StringView copy(StringView text, bool *ok = nullptr);

                        /** @brief Frees every chunk: everything made in the arena is gone. */
                        void clear();

                        /** @brief Bytes taken from malloc, for the budget. */
                        size_t bytesReserved() const { return _reserved; }

                private:
                        struct Chunk {
                                        Chunk *next;
                                        size_t size;
                                        size_t used;
                        };

                        Chunk   *_chunks = nullptr;
                        size_t   _reserved = 0;
                        uint32_t _chunkSize;
        };

} // namespace preso

#endif // PRESO_ARENA_H
