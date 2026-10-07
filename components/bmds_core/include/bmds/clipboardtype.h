/**
 * @file      clipboardtype.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The clipboard's data types: MIME types (`devplan/clipboard.md` C5).
 *
 * A type's normal form is what types are compared by: the type, subtype and
 * parameter names in lowercase, no spaces, the `charset` parameter's value in
 * lowercase (`utf8` spelled `utf-8`), other values as written, parameters in
 * the order written. `text` is shorthand for `Text`.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct ClipboardType {
                /** @brief A type's longest normal form, in bytes. */
                static constexpr size_t MaxType = 63;

                /** @brief A buffer a normal form fits in (NUL included). */
                static constexpr size_t BufferBytes = MaxType + 1;

                /** @brief Text: what the clipboard's text always is (lines end with LF). */
                static constexpr StringView Text{"text/plain;charset=utf-8"};

                /**
                 * @brief Writes @p type's normal form into @p out (NUL-ended).
                 * @return The normal form (a view of @p out); `Invalid` (not a
                 *         MIME type), or `BufferTooSmall` (over `MaxType`, or
                 *         over @p out).
                 */
                static Result<StringView> normalize(StringView type, Span<char> out);

                /** @brief Returns true if @p type is a valid type (`normalize()` would take it). */
                static bool isValid(StringView type);

                /** @brief Returns true if @p a and @p b are the same type (false if either is invalid). */
                static bool equal(StringView a, StringView b);

                /**
                 * @brief Returns the type a file of name @p name is, by its extension
                 *        (any case), or an empty view (not known).
                 */
                static StringView fromFileName(StringView name);
};

BMDS_NAMESPACE_END
