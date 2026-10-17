/**
 * @file      themes.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Color themes (`devplan/controlpanel.md` §12): sets of the window
 *        manager's palette (`wm/palette/...`) and of the terminal's colors
 *        (`terminal/colors/...`), applied as a whole. A theme isn't a setting:
 *        the one shown is the one the colors match, else none (Custom). Pure.
 *
 * @par Thread Safety
 * Rodata and reentrant functions.
 */
namespace Themes {

        /** @brief A theme: its name and a color for each of its set's names, in their order. */
        struct Theme {
                        const char     *name;
                        const uint32_t *colors;
        };

        /** @brief A set of themes: the keys' prefix, their last components, the themes. */
        struct Set {
                        const char             *prefix; ///< "wm/palette/"
                        Span<const char *const> names;  ///< "desktop", "active-frame", ...
                        Span<const Theme>       themes;
        };

        /** @brief The window manager's palette themes (the first: the standard one). */
        const Set &system();

        /** @brief The terminal's color themes (the first: the standard one). */
        const Set &terminal();

        /** @brief Returns the theme @p colors (in @p set's names' order) match exactly, or -1. */
        int match(const Set &set, Span<const uint32_t> colors);

} // namespace Themes

BMDS_NAMESPACE_END
