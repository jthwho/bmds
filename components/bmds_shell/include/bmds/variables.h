/**
 * @file      variables.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The shell's variables: names to string values, with attributes.
 *
 * A sorted list with binary search (shells hold tens of variables, not
 * thousands). A variable can exist without a value (`export X`, `local X`):
 * `IsSet` tells the difference that `${X-default}` sees.
 *
 * @par Thread Safety
 * Not thread-safe; owned by one shell (a pipeline stage works on a copy).
 */
class Variables {
        public:
                /** @brief Attribute bits. */
                enum Flag : uint8_t {
                        IsSet = 1u << 0,    ///< Has a value.
                        Exported = 1u << 1, ///< Passed to programs (Phase 3).
                        ReadOnly = 1u << 2, ///< May not be assigned or unset.
                };

                /** @brief One variable. */
                struct Entry {
                                String  name;
                                String  value;
                                uint8_t flags = 0;
                };

                Variables() = default;
                Variables(Variables &&) = default;
                Variables &operator=(Variables &&) = default;

                /** @brief Returns the variable called @p name, or null. */
                const Entry *find(StringView name) const;

                /** @brief Returns the value of @p name if it is set, or null. */
                const String *value(StringView name) const;

                /**
                 * @brief Assigns @p value to @p name, creating it if needed.
                 * @return `Ok`, `ReadOnly`, or `NoMem`.
                 */
                Error set(StringView name, StringView value);

                /**
                 * @brief Adds attribute @p flags (`Exported`, `ReadOnly`) to @p name, creating it unset if needed.
                 * @return `Ok` or `NoMem`.
                 */
                Error addFlags(StringView name, uint8_t flags);

                /**
                 * @brief Removes @p name.
                 * @return `Ok` (also if it didn't exist) or `ReadOnly`.
                 */
                Error unset(StringView name);

                /**
                 * @brief Puts @p name back exactly as saved, ignoring `ReadOnly` (`local` scope end).
                 * @param existed False to remove it.
                 */
                Error restore(StringView name, bool existed, StringView value, uint8_t flags);

                /** @brief Returns all variables, sorted by name. */
                Span<const Entry> entries() const { return _entries.span(); }

                /** @brief Returns a deep copy (for a subshell), or `NoMem`. */
                Result<Variables> clone() const;

        private:
                size_t          lowerBound(StringView name) const;
                Result<Entry *> findOrCreate(StringView name);

                List<Entry> _entries;
};

BMDS_NAMESPACE_END
