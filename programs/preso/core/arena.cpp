/**
 * @file      arena.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "arena.h"
#include <stdlib.h>

namespace preso {

        namespace {

                constexpr size_t HeaderSize = 32; // past the Chunk header, aligned for anything

                size_t alignUp(size_t value, size_t align) {
                        return (value + align - 1) & ~(align - 1);
                }

        } // namespace

        void *Arena::allocate(size_t size, size_t align) {
                if (align == 0 || (align & (align - 1)) != 0 || align > HeaderSize) return nullptr;
                if (_chunks != nullptr) {
                        size_t start = alignUp(_chunks->used, align);
                        if (start + size <= _chunks->size) {
                                _chunks->used = start + size;
                                return reinterpret_cast<char *>(_chunks) + start;
                        }
                }
                size_t want = HeaderSize + size;
                size_t chunkSize = want > _chunkSize ? want : _chunkSize;
                auto  *chunk = static_cast<Chunk *>(malloc(chunkSize));
                if (chunk == nullptr) return nullptr;
                chunk->size = chunkSize;
                chunk->used = HeaderSize + size;
                _reserved += chunkSize;
                // A big request's chunk goes behind the current one, which may still have room.
                if (_chunks != nullptr && want > _chunkSize) {
                        chunk->next = _chunks->next;
                        _chunks->next = chunk;
                } else {
                        chunk->next = _chunks;
                        _chunks = chunk;
                }
                return reinterpret_cast<char *>(chunk) + HeaderSize;
        }

        StringView Arena::copy(StringView text, bool *ok) {
                char *p = static_cast<char *>(allocate(text.length() + 1, 1));
                if (ok != nullptr) *ok = p != nullptr;
                if (p == nullptr) return StringView();
                if (text.length() > 0) memcpy(p, text.data(), text.length());
                p[text.length()] = '\0';
                return StringView(p, text.length());
        }

        void Arena::clear() {
                while (_chunks != nullptr) {
                        Chunk *next = _chunks->next;
                        free(_chunks);
                        _chunks = next;
                }
                _reserved = 0;
                return;
        }

} // namespace preso
