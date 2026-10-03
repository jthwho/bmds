/**
 * @file      span.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Non-owning view of a contiguous sequence of `T`.
 *
 * Use `Span<const T>` for read-only access. A `Span<T>` converts
 * implicitly to `Span<const T>`. Constructible from C arrays,
 * `std::array`, and any contiguous container that has `data()` and
 * `size()` (such as `List<T>`).
 *
 * `operator[]` is unchecked (Spans are used on hot paths). Sub-span
 * operations (`first()`, `last()`, `mid()`) clamp out-of-range arguments.
 *
 * @par Thread Safety
 * Distinct instances may be used concurrently. A Span does not
 * synchronize access to the elements it views.
 *
 * @tparam T Element type (may be const-qualified).
 */
template <typename T> class Span {
        public:
                /** @brief Element type, including any const qualifier. */
                using ElementType = T;

                /** @brief Element type without cv-qualifiers. */
                using ValueType = std::remove_cv_t<T>;

                /** @brief Iterator type. */
                using Iterator = T *;

                /** @brief "To the end" count for `mid()`. */
                static constexpr size_t NPos = static_cast<size_t>(-1);

                /** @brief Constructs an empty span. */
                constexpr Span() = default;

                /**
                 * @brief Views @p size elements starting at @p data.
                 * @param data First element.
                 * @param size Number of elements.
                 */
                constexpr Span(T *data, size_t size) : _data(data), _size(size) {}

                /** @brief Views a C array. */
                template <size_t N> constexpr Span(T (&array)[N]) : _data(array), _size(N) {}

                /** @brief Views a `std::array`. */
                template <typename U, size_t N>
                        requires std::convertible_to<U (*)[], T (*)[]>
                constexpr Span(std::array<U, N> &array) : _data(array.data()), _size(N) {}

                /** @brief Views a const `std::array`. */
                template <typename U, size_t N>
                        requires std::convertible_to<const U (*)[], T (*)[]>
                constexpr Span(const std::array<U, N> &array) : _data(array.data()), _size(N) {}

                /** @brief Converts from a span of a compatible type (e.g. `Span<T>` to `Span<const T>`). */
                template <typename U>
                        requires(!std::same_as<U, T> && std::convertible_to<U (*)[], T (*)[]>)
                constexpr Span(const Span<U> &other) : _data(other.data()), _size(other.size()) {}

                /** @brief Views any contiguous container with compatible `data()` and `size()`. */
                template <typename C>
                        requires(!std::is_array_v<std::remove_reference_t<C>> &&
                                 !std::same_as<std::remove_cvref_t<C>, Span> &&
                                 requires(C &c) {
                                         { c.data() } -> std::convertible_to<T *>;
                                         { c.size() } -> std::convertible_to<size_t>;
                                 })
                constexpr Span(C &container) : _data(container.data()), _size(container.size()) {}

                /** @brief Returns the element at @p index (unchecked). */
                constexpr T &operator[](size_t index) const { return _data[index]; }

                /** @brief Returns a pointer to the first element. */
                constexpr T *data() const { return _data; }

                /** @brief Returns the number of elements. */
                constexpr size_t size() const { return _size; }

                /** @brief Returns the size of the viewed elements in bytes. */
                constexpr size_t sizeBytes() const { return _size * sizeof(T); }

                /** @brief Returns true if the span has no elements. */
                constexpr bool isEmpty() const { return _size == 0; }

                /** @brief Returns the first element; the span must not be empty. */
                constexpr T &front() const { return _data[0]; }

                /** @brief Returns the last element; the span must not be empty. */
                constexpr T &back() const { return _data[_size - 1]; }

                /** @brief Returns an iterator to the first element. */
                constexpr Iterator begin() const { return _data; }

                /** @brief Returns an iterator past the last element. */
                constexpr Iterator end() const { return _data + _size; }

                /** @brief Returns the first @p count elements (clamped). */
                constexpr Span first(size_t count) const { return Span(_data, count < _size ? count : _size); }

                /** @brief Returns the last @p count elements (clamped). */
                constexpr Span last(size_t count) const {
                        return count < _size ? Span(_data + (_size - count), count) : *this;
                }

                /**
                 * @brief Returns @p count elements starting at @p offset.
                 * @param offset Start index; past the end gives an empty span.
                 * @param count  Number of elements (clamped); `NPos` means "to the end".
                 */
                constexpr Span mid(size_t offset, size_t count = NPos) const {
                        if (offset >= _size) return Span();
                        size_t avail = _size - offset;
                        return Span(_data + offset, count < avail ? count : avail);
                }

                /** @brief Returns a read-only byte view of the elements. */
                Span<const uint8_t> asBytes() const {
                        return Span<const uint8_t>(reinterpret_cast<const uint8_t *>(_data), sizeBytes());
                }

                /** @brief Returns a writable byte view of the elements (only for non-const `T`). */
                Span<uint8_t> asWritableBytes() const
                        requires(!std::is_const_v<T>)
                {
                        return Span<uint8_t>(reinterpret_cast<uint8_t *>(_data), sizeBytes());
                }

        private:
                T     *_data = nullptr;
                size_t _size = 0;
};

/** @brief Read-only byte span. */
using ByteSpan = Span<const uint8_t>;

/** @brief Writable byte span. */
using WritableByteSpan = Span<uint8_t>;

BMDS_NAMESPACE_END
