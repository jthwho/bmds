/**
 * @file      string.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Owning, growable, NUL-terminated byte string.
 *
 * Text up to `InlineCapacity` bytes is stored inside the object (no
 * allocation); longer text lives in a heap block from the string's
 * `MemSpace` (`DefaultSpace`, PSRAM, unless one is given). The contents are
 * bytes, conventionally UTF-8; nothing here interprets them.
 *
 * Allocation can fail, and failure is reported rather than hidden:
 * - Every operation that may allocate returns `Error` (`NoMem` on failure,
 *   with the string unchanged), and `Error` is `[[nodiscard]]`.
 * - Strings are **move-only**. A copy is an allocation, so it is explicit:
 *   `clone()` returns `Result<String>`. Constructors never allocate.
 *
 * Read-only operations (searching, slicing, comparing) live on
 * `StringView`; a String converts to one implicitly.
 *
 * @par Thread Safety
 * Distinct instances may be used concurrently. Not internally synchronized:
 * share by handoff (move it to the consumer and stop using it).
 *
 * @par Example
 * @code
 * String path;
 * if (Error err = path.appendf("%s/%s", dir, name); err.isError()) return err;
 * if (path.view().endsWith(".bat")) ...
 * @endcode
 */
class String {
        public:
                /** @brief Bytes stored inside the object before a heap block is needed. */
                static constexpr size_t InlineCapacity = 15;

                /** @brief The space used when none is given. */
                static constexpr MemSpace DefaultSpace = MemSpace::Psram;

                /** @brief "To the end" count, as in StringView. */
                static constexpr size_t NPos = StringView::NPos;

                /** @brief Iterator type. */
                using Iterator = char *;

                /** @brief Const iterator type. */
                using ConstIterator = const char *;

                /** @brief Constructs an empty string that allocates from `DefaultSpace`. */
                String() : _space(DefaultSpace), _isHeap(false) { _store.buf[0] = '\0'; }

                /**
                 * @brief Constructs an empty string that allocates from @p space.
                 * @param space Where a heap block, if ever needed, comes from.
                 */
                explicit String(MemSpace space) : _space(space), _isHeap(false) { _store.buf[0] = '\0'; }

                /** @brief Takes over @p other's contents; @p other is left empty (same space). */
                String(String &&other);

                /** @brief Takes over @p other's contents, freeing this string's own. */
                String &operator=(String &&other);

                String(const String &) = delete;
                String &operator=(const String &) = delete;

                /** @brief Frees the heap block, if any. */
                ~String() { freeHeap(); }

                /**
                 * @brief Makes a string holding a copy of @p text.
                 * @param text  The bytes to copy.
                 * @param space Where the string allocates from.
                 * @return The string, or `NoMem`.
                 */
                static Result<String> fromView(StringView text, MemSpace space = DefaultSpace);

                /**
                 * @brief Makes a string from a printf-style format.
                 * @param space Where the string allocates from.
                 * @param fmt   printf format.
                 * @return The string, or `NoMem`.
                 */
                static Result<String> format(MemSpace space, const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

                /**
                 * @brief Returns a copy, allocating from the same space.
                 * @return The copy, or `NoMem`.
                 */
                Result<String> clone() const;

                /** @brief Returns the bytes, always NUL-terminated. */
                const char *data() const { return _isHeap ? _store.heap.ptr : _store.buf; }

                /** @brief Returns the bytes, always NUL-terminated (writable up to `size()`). */
                char *data() { return _isHeap ? _store.heap.ptr : _store.buf; }

                /** @brief Returns the bytes as a C string (same as `data()`). */
                const char *cStr() const { return data(); }

                /** @brief Returns the number of bytes, excluding the terminator. */
                size_t size() const { return _size; }

                /** @brief Returns true if the string has no bytes. */
                bool isEmpty() const { return _size == 0; }

                /** @brief Returns how many bytes fit without allocating, excluding the terminator. */
                size_t capacity() const { return _isHeap ? _store.heap.capacity : InlineCapacity; }

                /** @brief Returns the space heap blocks come from. */
                MemSpace space() const { return _space; }

                /** @brief Returns a view of the contents, valid until the string is next modified. */
                StringView view() const { return StringView(data(), _size); }

                /** @brief Converts to a view of the contents. */
                operator StringView() const { return view(); }

                /** @brief Returns the byte at @p index (unchecked). */
                char operator[](size_t index) const { return data()[index]; }

                /** @brief Returns the byte at @p index (unchecked). */
                char &operator[](size_t index) { return data()[index]; }

                /** @brief Returns an iterator to the first byte. */
                Iterator begin() { return data(); }

                /** @brief Returns an iterator past the last byte. */
                Iterator end() { return data() + _size; }

                /** @brief Returns an iterator to the first byte. */
                ConstIterator begin() const { return data(); }

                /** @brief Returns an iterator past the last byte. */
                ConstIterator end() const { return data() + _size; }

                /** @brief Compares contents for equality. */
                bool operator==(const String &other) const { return view() == other.view(); }

                /** @brief Compares contents for equality. */
                bool operator==(StringView other) const { return view() == other; }

                /** @brief Lexicographic three-way comparison of contents. */
                std::strong_ordering operator<=>(const String &other) const { return view() <=> other.view(); }

                /** @brief Lexicographic three-way comparison of contents. */
                std::strong_ordering operator<=>(StringView other) const { return view() <=> other; }

                /**
                 * @brief Ensures at least @p capacity bytes fit without allocating.
                 * @return `Ok`, or `NoMem` (string unchanged).
                 */
                Error reserve(size_t capacity);

                /**
                 * @brief Replaces the contents with @p text (which may view this string).
                 * @return `Ok`, or `NoMem` (string unchanged).
                 */
                Error assign(StringView text);

                /**
                 * @brief Appends @p text (which may view this string).
                 * @return `Ok`, or `NoMem` (string unchanged).
                 */
                Error append(StringView text);

                /**
                 * @brief Appends one byte.
                 * @return `Ok`, or `NoMem` (string unchanged).
                 */
                Error append(char ch) {
                        if (_size < capacity()) {
                                char *p = data();
                                p[_size++] = ch;
                                p[_size] = '\0';
                                return Error::Ok;
                        }
                        return append(StringView(&ch, 1));
                }

                /**
                 * @brief Appends printf-style formatted text.
                 * @return `Ok`, or `NoMem` (string unchanged).
                 */
                Error appendf(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

                /** @brief `appendf()` taking a `va_list`. */
                Error vappendf(const char *fmt, va_list args);

                /**
                 * @brief Inserts @p text before byte @p pos (clamped to `size()`).
                 *
                 * @p text may view this string.
                 * @return `Ok`, or `NoMem` (string unchanged).
                 */
                Error insert(size_t pos, StringView text);

                /**
                 * @brief Removes @p count bytes starting at @p pos (both clamped).
                 *
                 * Never allocates.
                 */
                void remove(size_t pos, size_t count = NPos);

                /** @brief Shortens the string to @p size bytes; no effect if already shorter. */
                void truncate(size_t size);

                /** @brief Empties the string and frees its heap block. */
                void clear();

        private:
                // A heap block is used once the text no longer fits inline;
                // the block holds `capacity + 1` bytes (the terminator).
                struct Heap {
                                char  *ptr;
                                size_t capacity;
                };
                union Store {
                                char buf[InlineCapacity + 1];
                                Heap heap;
                };

                void  freeHeap();
                Error reallocate(size_t capacity, StringView extra);

                Store    _store;
                uint32_t _size = 0;
                MemSpace _space;
                bool     _isHeap;
};

static_assert(sizeof(void *) != 4 || sizeof(String) == 24, "String should stay 24 bytes on the target");

BMDS_NAMESPACE_END
