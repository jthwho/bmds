/**
 * @file      windowplacement.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/rect.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where a window is when it isn't maximized, and whether it is (`devplan/editor.md`
 *        §18): what an application keeps to open its window there again, pure.
 *
 * As a setting's text: `X Y W H`, then `maximized` if it was.
 *
 * @par Thread Safety
 * A value.
 */
struct WindowPlacement {
                Rect normal;            ///< Outer, frame included; a maximized or snapped one's from before.
                bool maximized = false; ///< It fills the work area.

                /** @brief Reads @p text (`X Y W H`, then `maximized`) into @p out. @return False if it isn't one. */
                static bool parse(StringView text, WindowPlacement &out);

                /** @brief Returns it as `parse()` reads it. */
                String format() const;
};

BMDS_NAMESPACE_END
