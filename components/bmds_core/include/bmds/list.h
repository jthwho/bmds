/**
 * @file      list.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <bmds/assert.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memory.h>
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

namespace Detail {

        /** @brief The capacity a list grows to: half again, at least @p needed, at least 4. */
        constexpr size_t listGrownCapacity(size_t current, size_t needed) {
                size_t cap = current + current / 2;
                if (cap < 4) cap = 4;
                return cap < needed ? needed : cap;
        }

        /** @brief Allocates storage for list elements of the given alignment. */
        inline void *listAlloc(MemSpace space, size_t bytes, size_t align) {
                // The heaps guarantee pointer alignment; stricter types ask for it.
                return align <= alignof(void *) ? Memory::alloc(space, bytes)
                                                : Memory::allocAligned(space, bytes, align);
        }

} // namespace Detail

/**
 * @brief Owning, growable array of `T` (BMDS's `std::vector`).
 *
 * Elements are stored contiguously in a block from the list's `MemSpace`
 * (`DefaultSpace`, PSRAM, unless one is given), so a List converts to
 * `Span<T>`. An empty list owns no block.
 *
 * As with `String`, allocation failure is reported, not hidden:
 * operations that may allocate return `Error` (`NoMem`, list unchanged),
 * lists are **move-only**, and `clone()` makes an explicit copy. `T` may be
 * move-only (e.g. `List<String>`).
 *
 * Indexing is unchecked, as in `Span`.
 *
 * @par Thread Safety
 * Distinct instances may be used concurrently. Not internally synchronized:
 * share by handoff.
 *
 * @tparam T Element type. Moving a `T` must not fail.
 */
template <typename T> class List {
        public:
                /** @brief Element type. */
                using ValueType = T;

                /** @brief Iterator type. */
                using Iterator = T *;

                /** @brief Const iterator type. */
                using ConstIterator = const T *;

                /** @brief The space used when none is given. */
                static constexpr MemSpace DefaultSpace = MemSpace::Psram;

                /** @brief "Not found" index. */
                static constexpr size_t NPos = static_cast<size_t>(-1);

                /** @brief Constructs an empty list that allocates from `DefaultSpace`. */
                List() = default;

                /**
                 * @brief Constructs an empty list that allocates from @p space.
                 * @param space Where the element block comes from.
                 */
                explicit List(MemSpace space) : _space(space) {}

                /** @brief Takes over @p other's elements; @p other is left empty (same space). */
                List(List &&other)
                    : _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0)),
                      _capacity(std::exchange(other._capacity, 0)), _space(other._space) {}

                /** @brief Takes over @p other's elements, destroying this list's own. */
                List &operator=(List &&other) {
                        if (this != &other) {
                                clear();
                                _data = std::exchange(other._data, nullptr);
                                _size = std::exchange(other._size, 0);
                                _capacity = std::exchange(other._capacity, 0);
                                _space = other._space;
                        }
                        return *this;
                }

                List(const List &) = delete;
                List &operator=(const List &) = delete;

                /** @brief Destroys the elements and frees the block. */
                ~List() { clear(); }

                /**
                 * @brief Returns a copy, allocating from the same space.
                 * @return The copy, or `NoMem`.
                 */
                Result<List> clone() const
                        requires std::copy_constructible<T>
                {
                        List  copy(_space);
                        Error err = copy.reserve(_size);
                        if (err.isOk()) {
                                for (size_t i = 0; i < _size; ++i) new (copy._data + i) T(_data[i]);
                                copy._size = _size;
                        }
                        return Result<List>(std::move(copy), err);
                }

                /** @brief Returns the number of elements. */
                size_t size() const { return _size; }

                /** @brief Returns true if the list has no elements. */
                bool isEmpty() const { return _size == 0; }

                /** @brief Returns how many elements fit without allocating. */
                size_t capacity() const { return _capacity; }

                /** @brief Returns the space the element block comes from. */
                MemSpace space() const { return _space; }

                /** @brief Returns the first element, or null if empty. */
                T *data() { return _data; }

                /** @brief Returns the first element, or null if empty. */
                const T *data() const { return _data; }

                /** @brief Returns the element at @p index (unchecked). */
                T &operator[](size_t index) { return _data[index]; }

                /** @brief Returns the element at @p index (unchecked). */
                const T &operator[](size_t index) const { return _data[index]; }

                /** @brief Returns the first element; the list must not be empty. */
                T &front() { return _data[0]; }

                /** @brief Returns the first element; the list must not be empty. */
                const T &front() const { return _data[0]; }

                /** @brief Returns the last element; the list must not be empty. */
                T &back() { return _data[_size - 1]; }

                /** @brief Returns the last element; the list must not be empty. */
                const T &back() const { return _data[_size - 1]; }

                /** @brief Returns an iterator to the first element. */
                Iterator begin() { return _data; }

                /** @brief Returns an iterator past the last element. */
                Iterator end() { return _data + _size; }

                /** @brief Returns an iterator to the first element. */
                ConstIterator begin() const { return _data; }

                /** @brief Returns an iterator past the last element. */
                ConstIterator end() const { return _data + _size; }

                /** @brief Returns a span over the elements. */
                Span<T> span() { return Span<T>(_data, _size); }

                /** @brief Returns a read-only span over the elements. */
                Span<const T> span() const { return Span<const T>(_data, _size); }

                /**
                 * @brief Returns the index of the first element equal to @p value.
                 * @return The index, or `NPos`.
                 */
                size_t indexOf(const T &value) const {
                        for (size_t i = 0; i < _size; ++i) {
                                if (_data[i] == value) return i;
                        }
                        return NPos;
                }

                /** @brief Returns true if an element equals @p value. */
                bool contains(const T &value) const { return indexOf(value) != NPos; }

                /**
                 * @brief Ensures at least @p capacity elements fit without allocating.
                 * @return `Ok`, or `NoMem` (list unchanged).
                 */
                Error reserve(size_t capacity) {
                        if (capacity <= _capacity) return Error::Ok;
                        return reallocate(capacity);
                }

                /**
                 * @brief Appends a copy of @p value.
                 *
                 * @p value may be an element of this list.
                 * @return `Ok`, or `NoMem` (list unchanged).
                 */
                Error append(const T &value)
                        requires std::copy_constructible<T>
                {
                        return emplace(_size, value);
                }

                /**
                 * @brief Appends @p value, moving it in.
                 * @return `Ok`, or `NoMem` (list unchanged, @p value untouched).
                 */
                Error append(T &&value) { return emplace(_size, std::move(value)); }

                /**
                 * @brief Appends copies of @p values (not of this list), as one growth and one copy.
                 * @return `Ok`, or `NoMem` (list unchanged).
                 */
                Error append(Span<const T> values)
                        requires std::is_trivially_copyable_v<T>
                {
                        if (values.size() == 0) return Error::Ok;
                        if (_size + values.size() > _capacity) {
                                const Error err =
                                        reallocate(Detail::listGrownCapacity(_capacity, _size + values.size()));
                                if (err.isError()) return err;
                        }
                        std::memcpy(static_cast<void *>(_data + _size), values.data(), values.size() * sizeof(T));
                        _size += values.size();
                        return Error::Ok;
                }

                /**
                 * @brief Inserts @p value before element @p index (clamped to `size()`).
                 * @return `Ok`, or `NoMem` (list unchanged, @p value untouched).
                 */
                Error insert(size_t index, T &&value) { return emplace(index, std::move(value)); }

                /**
                 * @brief Constructs an element in place before element @p index (clamped).
                 *
                 * The arguments may refer to elements of this list.
                 * @return `Ok`, or `NoMem` (list unchanged).
                 */
                template <typename... Args> Error emplace(size_t index, Args &&...args) {
                        if (index > _size) index = _size;
                        if (_size == _capacity) {
                                // Build the new element in the new block before
                                // moving the old ones: the arguments may refer to them.
                                const size_t capacity = Detail::listGrownCapacity(_capacity, _size + 1);
                                T           *block = allocBlock(capacity);
                                if (block == nullptr) return Error::NoMem;
                                new (block + index) T(std::forward<Args>(args)...);
                                relocate(block, _data, index);
                                relocate(block + index + 1, _data + index, _size - index);
                                adopt(block, capacity);
                        } else if (index == _size) {
                                new (_data + _size) T(std::forward<Args>(args)...);
                        } else {
                                T value(std::forward<Args>(args)...);
                                new (_data + _size) T(std::move(_data[_size - 1]));
                                for (size_t i = _size - 1; i > index; --i) _data[i] = std::move(_data[i - 1]);
                                _data[index] = std::move(value);
                        }
                        ++_size;
                        return Error::Ok;
                }

                /** @brief Removes the element at @p index; out of range is ignored. Never allocates. */
                void removeAt(size_t index) {
                        if (index >= _size) return;
                        for (size_t i = index + 1; i < _size; ++i) _data[i - 1] = std::move(_data[i]);
                        _data[--_size].~T();
                        return;
                }

                /** @brief Removes and returns the last element; the list must not be empty. */
                T takeLast() {
                        BMDS_ASSERT(_size > 0);
                        T value(std::move(_data[_size - 1]));
                        _data[--_size].~T();
                        return value;
                }

                /** @brief Destroys elements past @p size; no effect if already shorter. Keeps the block. */
                void truncate(size_t size) {
                        if constexpr (std::is_trivially_destructible_v<T>) {
                                if (_size > size) _size = size;
                        } else {
                                while (_size > size) _data[--_size].~T();
                        }
                        return;
                }

                /** @brief Destroys all elements and frees the block. */
                void clear() {
                        truncate(0);
                        Memory::free(_data);
                        _data = nullptr;
                        _capacity = 0;
                        return;
                }

        private:
                T *allocBlock(size_t capacity) {
                        return static_cast<T *>(Detail::listAlloc(_space, capacity * sizeof(T), alignof(T)));
                }

                // Moves @p count elements into uninitialized @p dst, destroying the sources.
                static void relocate(T *dst, T *src, size_t count) {
                        if constexpr (std::is_trivially_copyable_v<T>) {
                                if (count != 0) std::memcpy(static_cast<void *>(dst), src, count * sizeof(T));
                        } else {
                                for (size_t i = 0; i < count; ++i) {
                                        new (dst + i) T(std::move(src[i]));
                                        src[i].~T();
                                }
                        }
                        return;
                }

                void adopt(T *block, size_t capacity) {
                        Memory::free(_data);
                        _data = block;
                        _capacity = capacity;
                        return;
                }

                Error reallocate(size_t capacity) {
                        T *block = allocBlock(capacity);
                        if (block == nullptr) return Error::NoMem;
                        relocate(block, _data, _size);
                        adopt(block, capacity);
                        return Error::Ok;
                }

                T       *_data = nullptr;
                size_t   _size = 0;
                size_t   _capacity = 0;
                MemSpace _space = DefaultSpace;
};

BMDS_NAMESPACE_END
