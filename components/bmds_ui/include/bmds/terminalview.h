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
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/stringview.h>
#include <bmds/terminalscreen.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A terminal's colors, 0xRRGGBB (`devplan/gui.md` §4l): its default
 *        text, background and cursor, and the 16 ANSI colors (xterm's unless
 *        set). The 256-color palette's others and RGB colors are fixed
 *        (`Color::toRgb()`).
 *
 * @par Thread Safety
 * A plain value.
 */
struct TerminalColors {
                uint32_t text = 0xc0c0c0;
                uint32_t background = 0x000000;
                uint32_t cursor = 0xc0c0c0; ///< The cursor's cell; its text in the cell's background.
                uint32_t ansi[16] = {0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
                                     0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};

                /** @brief Returns @p color as 0xRRGGBB, @p fallback for the default color. @hotpath */
                uint32_t rgb(Color color, uint32_t fallback) const;

                /**
                 * @brief Returns the color named @p key (`Text`, `Background`, `Cursor`,
                 *        `Color0` to `Color15`; any case), or null.
                 */
                uint32_t *find(StringView key);

                /**
                 * @brief Sets the color named @p key (as `find()`) to @p value (`parse()`).
                 * @return `Ok`, `NotFound` (no such color), or `Invalid`.
                 */
                Error set(StringView key, StringView value);

                /** @brief Reads `RRGGBB` or `#RRGGBB` (hex digits, spaces around ignored). */
                static Result<uint32_t> parse(StringView value);

                constexpr bool operator==(const TerminalColors &other) const = default;
};

/**
 * @brief How a `TerminalScreen` row is drawn: its cells as the painter's
 *        `TextCell`s (`devplan/windows.md` §4e).
 *
 * Colors come from a `TerminalColors` (the default ones and the 16 ANSI
 * colors) or `Color::toRgb()`. Reverse swaps the
 * colors, dim halves the foreground, invisible draws it in the background;
 * bold and underline become the cell's style. A wide character is one cell
 * (its glyph is two wide): its tail is left out. The cursor's cell is drawn
 * in the cursor color, its text in the cell's background.
 *
 * @par Thread Safety
 * Pure functions; the screen read as the caller locked it.
 */
namespace TerminalView {

        /**
         * @brief Writes row @p row of @p screen into @p out (up to @p max
         *        cells), the cursor at column @p cursor (or none: -1).
         * @return The cells written.
         * @hotpath
         */
        size_t row(const TerminalScreen &screen, uint16_t row, const TerminalColors &colors, int cursor, TextCell *out,
                   size_t max);

        /**
         * @brief Writes a row @p columns wide into @p out as `row()` does,
         *        from @p length of @p cells (a screen row, or a history line
         *        of any length): blanks past them, and a wide character cut
         *        at the last column blanked. Columns [@p selectFrom,
         *        @p selectTo) are selected: drawn with their colors swapped.
         * @return The cells written.
         * @hotpath
         */
        size_t cells(const TerminalCell *cells, uint16_t length, uint16_t columns, const TerminalColors &colors,
                     int cursor, TextCell *out, size_t max, uint16_t selectFrom = 0, uint16_t selectTo = 0);

        /**
         * @brief Returns what shown row @p row is, scrolled back @p offset
         *        lines (clamped to the history): a history line or a screen
         *        row (§4i), @p length cells of it.
         */
        const TerminalCell *viewRow(const TerminalScreen &screen, size_t offset, uint16_t row, uint16_t &length);

        /** @brief Returns the shown row the cursor is on, scrolled back @p offset lines, or -1 (none). */
        int viewCursorRow(const TerminalScreen &screen, size_t offset);

        /**
         * @brief Returns line @p line (numbered from the history's start:
         *        `TerminalSelection`), @p length cells of it, or null (gone, or
         *        not yet there).
         */
        const TerminalCell *line(const TerminalScreen &screen, int64_t line, uint16_t &length);

        /** @brief Returns the number (as `line()`'s) of shown row @p row, scrolled back @p offset lines. */
        int64_t lineOfViewRow(const TerminalScreen &screen, size_t offset, uint16_t row);

} // namespace TerminalView

BMDS_NAMESPACE_END
