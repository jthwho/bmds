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

        /**
         * @brief Writes a row @p columns wide into @p out as `row()` does,
         *        from @p length of @p cells (a screen row, or a history line
         *        of any length): blanks past them, and a wide character cut
         *        at the last column blanked.
         * @return The cells written.
         * @hotpath
         */
        size_t cells(const TerminalCell *cells, uint16_t length, uint16_t columns, const WindowPalette &palette,
                     int cursor, TextCell *out, size_t max);

        /**
         * @brief Returns what shown row @p row is, scrolled back @p offset
         *        lines (clamped to the history): a history line or a screen
         *        row (§4i), @p length cells of it.
         */
        const TerminalCell *viewRow(const TerminalScreen &screen, size_t offset, uint16_t row, uint16_t &length);

        /** @brief Returns the shown row the cursor is on, scrolled back @p offset lines, or -1 (none). */
        int viewCursorRow(const TerminalScreen &screen, size_t offset);

} // namespace TerminalView

BMDS_NAMESPACE_END
