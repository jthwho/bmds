/**
 * @file      themes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Color themes for the window manager and the terminal (devplan/controlpanel.md §12).
 */

#include "themes.h"

BMDS_NAMESPACE_BEGIN

namespace Themes {

        namespace {

                // The window manager's roles by their settings' names (WindowPalette's order).
                constexpr const char *SystemNames[] = {
                        "desktop",        "active-frame",  "inactive-frame", "active-title", "inactive-title",
                        "close-box",      "close-mark",    "client",         "outline",      "cursor-edge",
                        "cursor-fill",    "button",        "button-mark",    "face",         "face-text",
                        "face-highlight", "face-light",    "face-shadow",    "face-dark",    "highlight",
                        "highlight-text", "disabled-text", "desktop-text",   "tooltip",      "tooltip-text",
                };
                constexpr size_t SystemCount = sizeof(SystemNames) / sizeof(SystemNames[0]);

                // Desktop, frames and titles, the close box, client, outline, cursor, title buttons,
                // the face's five, highlight, disabled, desktop text, tooltip.
                constexpr uint32_t Standard[SystemCount] = {
                        0x404040, 0x3a6ea5, 0x808080, 0xffffff, 0xd8d8d8, 0xc85050, 0xffffff, 0xf0f0f0, 0xffffff,
                        0x000000, 0xffffff, 0xc0c0c0, 0x000000, 0xc0c0c0, 0x000000, 0xffffff, 0xdfdfdf, 0x808080,
                        0x000000, 0x000080, 0xffffff, 0x505050, 0xffffff, 0xffffe1, 0x000000,
                };
                constexpr uint32_t Retro[SystemCount] = {
                        0x008080, 0x000080, 0x808080, 0xffffff, 0xc0c0c0, 0xc0c0c0, 0x000000, 0xffffff, 0xffffff,
                        0x000000, 0xffffff, 0xc0c0c0, 0x000000, 0xc0c0c0, 0x000000, 0xffffff, 0xdfdfdf, 0x808080,
                        0x000000, 0x000080, 0xffffff, 0x505050, 0xffffff, 0xffffe1, 0x000000,
                };
                constexpr uint32_t Slate[SystemCount] = {
                        0x2f3b4c, 0x4a6fa5, 0x7b8794, 0xffffff, 0xe0e4ea, 0xd05a5a, 0xffffff, 0xf4f6f8, 0xffffff,
                        0x000000, 0xffffff, 0xd9dee5, 0x1d2733, 0xd9dee5, 0x1d2733, 0xffffff, 0xeef1f5, 0x8a96a6,
                        0x2a3442, 0x4a6fa5, 0xffffff, 0x4f5b6a, 0xffffff, 0xfffbe6, 0x1d2733,
                };
                // Dark: the lit edge (also fields' and lists' ground) lighter than the face, so bevels
                // still read as raised.
                constexpr uint32_t Dark[SystemCount] = {
                        0x1e1e1e, 0x2d5f8a, 0x3a3a3a, 0xffffff, 0x9a9a9a, 0xc04848, 0xffffff, 0x202020, 0xffffff,
                        0x000000, 0xffffff, 0x4a4a4a, 0xe0e0e0, 0x3c3c3c, 0xe6e6e6, 0x5a5a5a, 0x4a4a4a, 0x262626,
                        0x101010, 0x2d5f8a, 0xffffff, 0xa8a8a8, 0xffffff, 0x2a2a2a, 0xe6e6e6,
                };
                constexpr uint32_t HighContrast[SystemCount] = {
                        0x000000, 0x000080, 0x606060, 0xffffff, 0xffffff, 0x000000, 0xffffff, 0xffffff, 0xffffff,
                        0x000000, 0xffffff, 0xffffff, 0x000000, 0xffffff, 0x000000, 0xffffff, 0xe0e0e0, 0x000000,
                        0x000000, 0x000000, 0xffffff, 0x404040, 0xffffff, 0xffffff, 0x000000,
                };
                constexpr Theme SystemThemes[] = {
                        {"Standard", Standard},          {"Retro", Retro}, {"Slate", Slate}, {"Dark", Dark},
                        {"High contrast", HighContrast},
                };

                // The terminal's: text, background, cursor, then the 16 ANSI colors.
                constexpr const char *TerminalNames[] = {"text",    "background", "cursor",  "color0",  "color1",
                                                         "color2",  "color3",     "color4",  "color5",  "color6",
                                                         "color7",  "color8",     "color9",  "color10", "color11",
                                                         "color12", "color13",    "color14", "color15"};
                constexpr size_t      TerminalCount = sizeof(TerminalNames) / sizeof(TerminalNames[0]);

                constexpr uint32_t Xterm[TerminalCount] = {
                        0xc0c0c0, 0x000000, 0xc0c0c0, 0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
                        0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00,
                        0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
                };
                constexpr uint32_t Tango[TerminalCount] = {
                        0xd3d7cf, 0x000000, 0xd3d7cf, 0x2e3436, 0xcc0000, 0x4e9a06, 0xc4a000,
                        0x3465a4, 0x75507b, 0x06989a, 0xd3d7cf, 0x555753, 0xef2929, 0x8ae234,
                        0xfce94f, 0x729fcf, 0xad7fa8, 0x34e2e2, 0xeeeeec,
                };
                constexpr uint32_t SolarizedDark[TerminalCount] = {
                        0x839496, 0x002b36, 0x93a1a1, 0x073642, 0xdc322f, 0x859900, 0xb58900,
                        0x268bd2, 0xd33682, 0x2aa198, 0xeee8d5, 0x002b36, 0xcb4b16, 0x586e75,
                        0x657b83, 0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3,
                };
                constexpr uint32_t SolarizedLight[TerminalCount] = {
                        0x657b83, 0xfdf6e3, 0x586e75, 0x073642, 0xdc322f, 0x859900, 0xb58900,
                        0x268bd2, 0xd33682, 0x2aa198, 0xeee8d5, 0x002b36, 0xcb4b16, 0x586e75,
                        0x657b83, 0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3,
                };
                constexpr uint32_t Paper[TerminalCount] = {
                        0x202020, 0xffffff, 0x202020, 0x000000, 0xc4302b, 0x2e8b2e, 0xa07800,
                        0x1f5fbf, 0xa335a3, 0x1f8f8f, 0xbfbfbf, 0x666666, 0xe5483f, 0x3cb371,
                        0xc9a400, 0x3a7be0, 0xc25cc2, 0x2aa8a8, 0xffffff,
                };
                constexpr uint32_t GreenScreen[TerminalCount] = {
                        0x33ff33, 0x000000, 0x33ff33, 0x000000, 0x22aa22, 0x22aa22, 0x22aa22,
                        0x22aa22, 0x22aa22, 0x22aa22, 0x33dd33, 0x116611, 0x33ff33, 0x33ff33,
                        0x33ff33, 0x33ff33, 0x33ff33, 0x33ff33, 0xaaffaa,
                };
                constexpr Theme TerminalThemes[] = {
                        {"Standard", Xterm},
                        {"Tango", Tango},
                        {"Solarized dark", SolarizedDark},
                        {"Solarized light", SolarizedLight},
                        {"Paper", Paper},
                        {"Green screen", GreenScreen},
                };

                constexpr Set SystemSet{"wm/palette/", Span<const char *const>(SystemNames),
                                        Span<const Theme>(SystemThemes)};
                constexpr Set TerminalSet{"terminal/colors/", Span<const char *const>(TerminalNames),
                                          Span<const Theme>(TerminalThemes)};

        } // namespace

        const Set &system() {
                return SystemSet;
        }

        const Set &terminal() {
                return TerminalSet;
        }

        int match(const Set &set, Span<const uint32_t> colors) {
                if (colors.size() != set.names.size()) return -1;
                for (size_t t = 0; t < set.themes.size(); ++t) {
                        bool same = true;
                        for (size_t i = 0; i < colors.size() && same; ++i) same = set.themes[t].colors[i] == colors[i];
                        if (same) return int(t);
                }
                return -1;
        }

} // namespace Themes

BMDS_NAMESPACE_END
