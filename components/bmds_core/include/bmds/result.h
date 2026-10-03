/**
 * @file      result.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A value paired with an Error, for fallible operations that produce a value.
 *
 * Supports structured bindings:
 * @code
 * auto [path, err] = Path::fromString(str);
 * if (err.isError()) return err;
 * @endcode
 *
 * Construct with `makeResult(value)` or `makeError<T>(err)`. There is
 * deliberately no implicit constructor from `Error` alone: `Error::Code` is
 * an unscoped enum and would silently convert to an integral `T`.
 *
 * On error the value is default-constructed, so `T` must be
 * default-constructible. `Result` is `[[nodiscard]]`.
 *
 * @par Thread Safety
 * Distinct instances may be used concurrently; a single instance is as
 * thread-safe as `T`.
 *
 * @tparam T The value type.
 */
template <typename T> class [[nodiscard]] Result {
        public:
                /** @brief The value type. */
                using ValueType = T;

                /** @brief Constructs a successful Result holding a default-constructed value. */
                constexpr Result() = default;

                /**
                 * @brief Constructs a Result from a value and an error.
                 * @param value The value.
                 * @param err   The error (default `Ok`).
                 */
                constexpr explicit Result(T value, Error err = Error::Ok) : _value(std::move(value)), _error(err) {}

                /** @brief Returns true if the error is `Ok`. */
                constexpr bool isOk() const { return _error.isOk(); }

                /** @brief Returns true if the error is not `Ok`. */
                constexpr bool isError() const { return _error.isError(); }

                /** @brief Returns the error. */
                constexpr Error error() const { return _error; }

                /** @brief Returns the value (default-constructed on error). */
                constexpr const T &value() const & { return _value; }

                /** @brief Returns the value (default-constructed on error). */
                constexpr T &value() & { return _value; }

                /** @brief Moves the value out (default-constructed on error). */
                constexpr T &&value() && { return std::move(_value); }

                /** @brief Tuple-protocol access for structured bindings: 0 = value, 1 = error. */
                template <std::size_t I> constexpr decltype(auto) get() const & {
                        static_assert(I < 2);
                        if constexpr (I == 0) {
                                return (_value);
                        } else {
                                return (_error);
                        }
                }

                /** @copydoc get() const & */
                template <std::size_t I> constexpr decltype(auto) get() & {
                        static_assert(I < 2);
                        if constexpr (I == 0) {
                                return (_value);
                        } else {
                                return (_error);
                        }
                }

                /** @copydoc get() const & */
                template <std::size_t I> constexpr decltype(auto) get() && {
                        static_assert(I < 2);
                        if constexpr (I == 0) {
                                return std::move(_value);
                        } else {
                                return std::move(_error);
                        }
                }

        private:
                T     _value{};
                Error _error;
};

/**
 * @brief Creates a successful Result.
 * @param value The value.
 * @return A Result holding @p value and `Error::Ok`.
 */
template <typename T> constexpr Result<std::decay_t<T>> makeResult(T &&value) {
        return Result<std::decay_t<T>>(std::forward<T>(value), Error::Ok);
}

/**
 * @brief Creates a failed Result with a default-constructed value.
 * @param err The error.
 * @return A Result holding `T{}` and @p err.
 */
template <typename T> constexpr Result<T> makeError(Error err) {
        return Result<T>(T{}, err);
}

BMDS_NAMESPACE_END

template <typename T> struct std::tuple_size<bmds::Result<T>> : std::integral_constant<std::size_t, 2> {};
template <typename T> struct std::tuple_element<0, bmds::Result<T>> {
                using type = T;
};
template <typename T> struct std::tuple_element<1, bmds::Result<T>> {
                using type = bmds::Error;
};
