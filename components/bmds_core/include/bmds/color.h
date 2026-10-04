/**
 * @file      color.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/** @brief How many colors a terminal shows. */
enum class ColorDepth : uint8_t {
        None,      ///< No color (attributes such as bold still work).
        Ansi16,    ///< The 16 ANSI colors (SGR 30-37, 90-97).
        Ansi256,   ///< The xterm 256-color palette (SGR 38;5;n).
        TrueColor, ///< 24-bit RGB (SGR 38;2;r;g;b).
};

/** @brief Returns the depth's name: `none`, `16`, `256` or `truecolor`. */
const char *colorDepthName(ColorDepth depth);

/**
 * @brief A terminal color: the terminal's default, a palette index, or RGB.
 *
 * Four bytes. Indices 0-15 are the ANSI colors, 16-231 the 6x6x6 cube,
 * 232-255 the gray ramp (xterm's palette, which `toRgb()` uses).
 *
 * @par Thread Safety
 * A plain value.
 */
class Color {
        public:
                /** @brief What the color is. */
                enum class Kind : uint8_t {
                        Default,
                        Indexed,
                        Rgb
                };

                /** @brief The terminal's default color. */
                constexpr Color() = default;

                /** @brief Palette entry @p index. */
                static constexpr Color indexed(uint8_t index) { return Color(Kind::Indexed, index, 0, 0); }

                /** @brief An RGB color. */
                static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) { return Color(Kind::Rgb, r, g, b); }

                constexpr Kind    kind() const { return _kind; }
                constexpr bool    isDefault() const { return _kind == Kind::Default; }
                constexpr uint8_t index() const { return _a; } ///< `Indexed` only.
                constexpr uint8_t red() const { return _a; }   ///< `Rgb` only.
                constexpr uint8_t green() const { return _b; } ///< `Rgb` only.
                constexpr uint8_t blue() const { return _c; }  ///< `Rgb` only.

                constexpr bool operator==(const Color &other) const = default;

                /** @brief Returns the RGB value (palette colors through xterm's palette; default is black). */
                Color toRgb() const;

                /** @brief Returns the nearest color a terminal of @p depth shows (`Default` at `None`). */
                Color downgrade(ColorDepth depth) const;

        private:
                constexpr Color(Kind kind, uint8_t a, uint8_t b, uint8_t c) : _kind(kind), _a(a), _b(b), _c(c) {}

                Kind    _kind = Kind::Default;
                uint8_t _a = 0;
                uint8_t _b = 0;
                uint8_t _c = 0;
};

static_assert(sizeof(Color) == 4);

/**
 * @brief The look of text: colors and attributes (what SGR sets).
 */
struct TextStyle {
                /** @brief Attribute bits. */
                enum Attribute : uint8_t {
                        Bold = 1u << 0,
                        Dim = 1u << 1,
                        Italic = 1u << 2,
                        Underline = 1u << 3,
                        Blink = 1u << 4,
                        Reverse = 1u << 5,
                        Invisible = 1u << 6,
                        Strike = 1u << 7,
                };

                Color   foreground;
                Color   background;
                uint8_t attributes = 0;

                constexpr bool operator==(const TextStyle &other) const = default;
};

BMDS_NAMESPACE_END
