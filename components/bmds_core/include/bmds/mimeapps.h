/**
 * @file      mimeapps.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief freedesktop.org's `mimeapps.list`: which applications open a type
 *        (`devplan/filetypes.md` T6). Pure.
 *
 * Only `[Default Applications]` is read: `TYPE=ID.desktop;ID2.desktop`, an ID
 * a Desktop Entry file in the applications' directories. BMDS adds a key of
 * a media type, a slash and an asterisk, which matches every type of that
 * media (all of `text`'s, say).
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace MimeApps {

        /**
         * @brief Writes the IDs for @p type in @p text into @p out, in the order
         *        to try them: the type's line's, then its media's wildcard
         *        line's (types compared ignoring case).
         * @return How many were written (views of @p text).
         */
        size_t lookup(StringView text, StringView type, Span<StringView> out);

} // namespace MimeApps

BMDS_NAMESPACE_END
