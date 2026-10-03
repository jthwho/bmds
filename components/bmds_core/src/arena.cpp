/**
 * @file      arena.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/arena.h>
#include <bmds/assert.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

void *Arena::alloc(size_t bytes, size_t align) {
        BMDS_ASSERT(align != 0 && (align & (align - 1)) == 0);
        if (bytes == 0) bytes = 1;
        if (_current != nullptr) {
                const uintptr_t base = reinterpret_cast<uintptr_t>(_current);
                const uintptr_t start = (base + _current->used + align - 1) & ~(align - 1);
                if (start - base <= _current->size && bytes <= _current->size - (start - base)) {
                        _current->used = start - base + bytes;
                        return reinterpret_cast<void *>(start);
                }
        }

        // Start a new chunk; one that wouldn't fit a regular chunk gets its own.
        const size_t overhead = sizeof(Chunk) + align - 1;
        if (bytes > static_cast<size_t>(-1) - overhead) return nullptr;
        const size_t size = bytes + overhead > _chunkBytes ? bytes + overhead : _chunkBytes;
        Chunk       *chunk = static_cast<Chunk *>(Memory::alloc(_space, size));
        if (chunk == nullptr) return nullptr;
        chunk->next = nullptr;
        chunk->size = size;
        chunk->used = sizeof(Chunk);
        if (_current != nullptr) {
                _current->next = chunk;
        } else {
                _first = chunk;
        }
        _current = chunk;
        return alloc(bytes, align);
}

Result<StringView> Arena::copyString(StringView text) {
        char *p = static_cast<char *>(alloc(text.size() + 1, 1));
        if (p == nullptr) return makeError<StringView>(Error::NoMem);
        if (!text.isEmpty()) std::memcpy(p, text.data(), text.size());
        p[text.size()] = '\0';
        return makeResult(StringView(p, text.size()));
}

Arena::Mark Arena::mark() const {
        Mark m;
        if (_current != nullptr) {
                m.chunk = _current;
                m.used = _current->used;
        }
        return m;
}

void Arena::rewind(const Mark &mark) {
        if (mark.chunk == nullptr) {
                reset();
                return;
        }
        freeAfter(mark.chunk);
        mark.chunk->used = mark.used;
        _current = mark.chunk;
        return;
}

void Arena::reset() {
        if (_first == nullptr) return;
        if (_first->size != _chunkBytes) {
                release();
                return;
        }
        freeAfter(_first);
        _first->used = sizeof(Chunk);
        _current = _first;
        return;
}

void Arena::release() {
        Chunk *chunk = _first;
        while (chunk != nullptr) {
                Chunk *next = chunk->next;
                Memory::free(chunk);
                chunk = next;
        }
        _first = nullptr;
        _current = nullptr;
        return;
}

size_t Arena::bytesUsed() const {
        size_t total = 0;
        for (const Chunk *c = _first; c != nullptr; c = c->next) total += c->used - sizeof(Chunk);
        return total;
}

size_t Arena::bytesReserved() const {
        size_t total = 0;
        for (const Chunk *c = _first; c != nullptr; c = c->next) total += c->size;
        return total;
}

size_t Arena::chunkCount() const {
        size_t count = 0;
        for (const Chunk *c = _first; c != nullptr; c = c->next) ++count;
        return count;
}

void Arena::freeAfter(Chunk *chunk) {
        Chunk *next = chunk->next;
        chunk->next = nullptr;
        while (next != nullptr) {
                Chunk *after = next->next;
                Memory::free(next);
                next = after;
        }
        return;
}

BMDS_NAMESPACE_END
