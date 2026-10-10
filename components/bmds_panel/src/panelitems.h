/**
 * @file      panelitems.h
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
 * @brief Edits of `panel/items`, pure (`devplan/gui.md` §4t).
 *
 * @par Thread Safety
 * Reentrant.
 */
struct PanelItems {
                /** @brief Returns true if @p list has an item @p kind (case ignored). */
                static bool has(StringView list, StringView kind);

                /**
                 * @brief Writes @p list into @p out (NUL ended) with item @p kind
                 *        shown or not: taken out (each, case ignored), or put
                 *        after the tray (`tray`, or `volume` read as it) if there
                 *        is one, so a clock shares its area, else at the end.
                 *        Already as asked: the list unchanged.
                 * @return The bytes written, or 0 if @p out is too small.
                 */
                static size_t withItem(StringView list, StringView kind, bool shown, Span<char> out);
};

BMDS_NAMESPACE_END
