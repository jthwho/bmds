/**
 * @file      windowpalette.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The window manager's colors, by role (`devplan/windows.md` W12):
 *        what a user changes to restyle the desktop.
 *
 * Each role has a name (`name()`, `find()`) for the `win color` command and,
 * later, a settings file. Colors are 0xRRGGBB.
 *
 * @par Thread Safety
 * A plain value.
 */
class WindowPalette {
        public:
                /** @brief What a color is for. Append only: the names are an interface. */
                enum class Role : uint8_t {
                        Desktop,            ///< Where no window is.
                        ActiveFrame,        ///< The focused window's frame and title bar.
                        InactiveFrame,      ///< The other windows' frames and title bars.
                        ActiveTitle,        ///< The focused window's title text.
                        InactiveTitle,      ///< The other windows' title text.
                        CloseBox,           ///< The close button.
                        CloseMark,          ///< The glyph on the close button.
                        Client,             ///< A client area that draws nothing itself.
                        Outline,            ///< A window's outline while it is moved.
                        TerminalText,       ///< A terminal's default foreground.
                        TerminalBackground, ///< A terminal's default background.
                        TerminalCursor,     ///< A terminal's cursor (its text in the cell's background).
                        CursorEdge,         ///< The mouse pointer's outline.
                        CursorFill,         ///< The mouse pointer's inside.
                        Button,             ///< A title-bar button but close (menu, minimize, maximize).
                        ButtonMark,         ///< The glyph on such a button.
                };

                /** @brief The number of roles. */
                static constexpr size_t RoleCount = 16;

                /** @brief The standard colors: a dark grey desktop, blue for the focused window. */
                constexpr WindowPalette() = default;

                /** @brief Returns @p role's color. */
                constexpr uint32_t color(Role role) const { return _colors[static_cast<size_t>(role)]; }

                /** @brief Sets @p role's color to @p rgb (0xRRGGBB; higher bits dropped). */
                constexpr void setColor(Role role, uint32_t rgb) {
                        _colors[static_cast<size_t>(role)] = rgb & 0xffffffu;
                        return;
                }

                /** @brief Returns @p role's name (`active-frame`), or null past the roles. */
                static const char *name(Role role);

                /** @brief Returns the role named @p name, or `NotFound`. */
                static Result<Role> find(StringView name);

                constexpr bool operator==(const WindowPalette &other) const = default;

        private:
                uint32_t _colors[RoleCount] = {
                        0x404040, // Desktop
                        0x3a6ea5, // ActiveFrame
                        0x808080, // InactiveFrame
                        0xffffff, // ActiveTitle
                        0xd8d8d8, // InactiveTitle
                        0xc85050, // CloseBox
                        0xffffff, // CloseMark
                        0xf0f0f0, // Client
                        0xffffff, // Outline
                        0xc0c0c0, // TerminalText
                        0x000000, // TerminalBackground
                        0xc0c0c0, // TerminalCursor
                        0x000000, // CursorEdge
                        0xffffff, // CursorFill
                        0xd0d0d0, // Button
                        0x000000, // ButtonMark
                };
};

BMDS_NAMESPACE_END
