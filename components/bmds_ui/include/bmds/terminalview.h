/**
 * @file      terminalview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/glyphrenderer.h>
#include <bmds/terminalscreen.h>
#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief How a `TerminalScreen` row is drawn: its cells as the painter's
 *        `TextCell`s (`devplan/windows.md` §4e).
 *
 * Default colors come from the palette (`TerminalText`,
 * `TerminalBackground`), others through `Color::toRgb()`. Reverse swaps the
 * colors, dim halves the foreground, invisible draws it in the background;
 * bold and underline become the cell's style. A wide character is one cell
 * (its glyph is two wide): its tail is left out. The cursor's cell is drawn
 * in the cursor color, its text in the cell's background.
 *
 * @par Thread Safety
 * Pure functions; the screen read as the caller locked it.
 */
namespace TerminalView {

        /** @brief Returns @p color as 0xRRGGBB, @p fallback for the default color. */
        uint32_t rgb(Color color, uint32_t fallback);

        /**
         * @brief Writes row @p row of @p screen into @p out (up to @p max
         *        cells), the cursor at column @p cursor (or none: -1).
         * @return The cells written.
         * @hotpath
         */
        size_t row(const TerminalScreen &screen, uint16_t row, const WindowPalette &palette, int cursor, TextCell *out,
                   size_t max);

} // namespace TerminalView

BMDS_NAMESPACE_END
