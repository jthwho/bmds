/**
 * @file      string.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <cstring>
#include <bmds/memory.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Grows by half again, and sizes blocks (capacity + terminator) in
        // multiples of 16 so small appends don't each reallocate.
        size_t grownCapacity(size_t current, size_t needed) {
                size_t cap = current + current / 2;
                if (cap < needed) cap = needed;
                return ((cap + 1 + 15) & ~static_cast<size_t>(15)) - 1;
        }

        bool aliases(const char *buf, size_t size, StringView text) {
                return !text.isEmpty() && text.data() >= buf && text.data() <= buf + size;
        }

} // namespace

String::String(String &&other) : _size(other._size), _space(other._space), _isHeap(other._isHeap) {
        if (_isHeap) {
                _store.heap = other._store.heap;
        } else {
                std::memcpy(_store.buf, other._store.buf, _size + 1);
        }
        other._isHeap = false;
        other._size = 0;
        other._store.buf[0] = '\0';
}

String &String::operator=(String &&other) {
        if (this == &other) return *this;
        freeHeap();
        _size = other._size;
        _space = other._space;
        _isHeap = other._isHeap;
        if (_isHeap) {
                _store.heap = other._store.heap;
        } else {
                std::memcpy(_store.buf, other._store.buf, _size + 1);
        }
        other._isHeap = false;
        other._size = 0;
        other._store.buf[0] = '\0';
        return *this;
}

Result<String> String::fromView(StringView text, MemSpace space) {
        String str(space);
        Error  err = str.assign(text);
        return Result<String>(std::move(str), err);
}

Result<String> String::format(MemSpace space, const char *fmt, ...) {
        String  str(space);
        va_list args;
        va_start(args, fmt);
        Error err = str.vappendf(fmt, args);
        va_end(args);
        return Result<String>(std::move(str), err);
}

Result<String> String::clone() const {
        return fromView(view(), _space);
}

Error String::reserve(size_t cap) {
        if (cap <= capacity()) return Error::Ok;
        return reallocate(cap, StringView());
}

Error String::assign(StringView text) {
        if (text.size() <= capacity()) {
                // A view of this string is never longer than it, so it
                // always takes this path; memmove handles the overlap.
                char *p = data();
                if (!text.isEmpty()) std::memmove(p, text.data(), text.size());
                _size = static_cast<uint32_t>(text.size());
                p[_size] = '\0';
                return Error::Ok;
        }
        const uint32_t oldSize = _size;
        _size = 0;
        Error err = reallocate(grownCapacity(0, text.size()), text);
        if (err.isError()) _size = oldSize;
        return err;
}

Error String::append(StringView text) {
        if (text.isEmpty()) return Error::Ok;
        const size_t needed = _size + text.size();
        if (needed > capacity()) return reallocate(grownCapacity(capacity(), needed), text);
        char *p = data();
        // The destination starts past the current text, so even a view of
        // this string doesn't overlap it.
        std::memcpy(p + _size, text.data(), text.size());
        _size = static_cast<uint32_t>(needed);
        p[_size] = '\0';
        return Error::Ok;
}

Error String::appendf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        Error err = vappendf(fmt, args);
        va_end(args);
        return err;
}

Error String::vappendf(const char *fmt, va_list args) {
        // Format straight into the spare capacity; only if it doesn't fit,
        // grow to the exact length and format again.
        va_list again;
        va_copy(again, args);
        const size_t spare = capacity() - _size;
        const int    len = std::vsnprintf(data() + _size, spare + 1, fmt, args);
        if (len < 0) {
                data()[_size] = '\0';
                va_end(again);
                return Error::Invalid;
        }
        const size_t needed = _size + static_cast<size_t>(len);
        if (static_cast<size_t>(len) <= spare) {
                _size = static_cast<uint32_t>(needed);
                va_end(again);
                return Error::Ok;
        }
        data()[_size] = '\0';
        Error err = reserve(grownCapacity(capacity(), needed));
        if (err.isOk()) {
                std::vsnprintf(data() + _size, static_cast<size_t>(len) + 1, fmt, again);
                _size = static_cast<uint32_t>(needed);
        }
        va_end(again);
        return err;
}

Error String::insert(size_t pos, StringView text) {
        if (pos > _size) pos = _size;
        if (text.isEmpty()) return Error::Ok;
        if (aliases(data(), _size, text)) {
                // Moving the tail would shift the bytes being inserted.
                auto [copy, err] = fromView(text, _space);
                if (err.isError()) return err;
                return insert(pos, copy.view());
        }
        const size_t needed = _size + text.size();
        if (needed > capacity()) {
                Error err = reserve(grownCapacity(capacity(), needed));
                if (err.isError()) return err;
        }
        char *p = data();
        std::memmove(p + pos + text.size(), p + pos, _size - pos + 1);
        std::memcpy(p + pos, text.data(), text.size());
        _size = static_cast<uint32_t>(needed);
        return Error::Ok;
}

void String::remove(size_t pos, size_t count) {
        if (pos >= _size) return;
        if (count > _size - pos) count = _size - pos;
        char *p = data();
        std::memmove(p + pos, p + pos + count, _size - pos - count + 1);
        _size -= static_cast<uint32_t>(count);
        return;
}

void String::truncate(size_t size) {
        if (size >= _size) return;
        _size = static_cast<uint32_t>(size);
        data()[_size] = '\0';
        return;
}

void String::clear() {
        freeHeap();
        _size = 0;
        _store.buf[0] = '\0';
        return;
}

void String::freeHeap() {
        if (_isHeap) {
                Memory::free(_store.heap.ptr);
                _isHeap = false;
        }
        return;
}

// Moves the text into a new block of @p cap bytes and appends @p extra,
// which may point into the old block (it is freed last).
Error String::reallocate(size_t cap, StringView extra) {
        char *block = static_cast<char *>(Memory::alloc(_space, cap + 1));
        if (block == nullptr) return Error::NoMem;
        std::memcpy(block, data(), _size);
        if (!extra.isEmpty()) std::memcpy(block + _size, extra.data(), extra.size());
        _size += static_cast<uint32_t>(extra.size());
        block[_size] = '\0';
        freeHeap();
        _store.heap.ptr = block;
        _store.heap.capacity = cap;
        _isHeap = true;
        return Error::Ok;
}

BMDS_NAMESPACE_END
