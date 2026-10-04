/**
 * @file      color.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/color.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // xterm's default ANSI colors.
        constexpr uint8_t Ansi[16][3] = {
                {0, 0, 0},     {205, 0, 0},     {0, 205, 0},     {205, 205, 0},   {0, 0, 238}, {205, 0, 205},
                {0, 205, 205}, {229, 229, 229}, {127, 127, 127}, {255, 0, 0},     {0, 255, 0}, {255, 255, 0},
                {92, 92, 255}, {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
        };

        constexpr uint8_t CubeLevels[6] = {0, 95, 135, 175, 215, 255};

        int distance(int r1, int g1, int b1, int r2, int g2, int b2) {
                const int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
                return dr * dr + dg * dg + db * db;
        }

        uint8_t nearestCubeLevel(uint8_t v) {
                if (v < 48) return 0;
                if (v < 115) return 1;
                return static_cast<uint8_t>((v - 35) / 40);
        }

} // namespace

const char *colorDepthName(ColorDepth depth) {
        switch (depth) {
                case ColorDepth::None: return "none";
                case ColorDepth::Ansi16: return "16";
                case ColorDepth::Ansi256: return "256";
                case ColorDepth::TrueColor: return "truecolor";
        }
        return "?";
}

Color Color::toRgb() const {
        switch (_kind) {
                case Kind::Default: return rgb(0, 0, 0);
                case Kind::Rgb: return *this;
                case Kind::Indexed: break;
        }
        if (_a < 16) return rgb(Ansi[_a][0], Ansi[_a][1], Ansi[_a][2]);
        if (_a < 232) {
                const int i = _a - 16;
                return rgb(CubeLevels[i / 36], CubeLevels[(i / 6) % 6], CubeLevels[i % 6]);
        }
        const uint8_t gray = static_cast<uint8_t>(8 + 10 * (_a - 232));
        return rgb(gray, gray, gray);
}

Color Color::downgrade(ColorDepth depth) const {
        if (_kind == Kind::Default || depth == ColorDepth::TrueColor)
                return depth == ColorDepth::None ? Color() : *this;
        if (depth == ColorDepth::None) return Color();
        if (_kind == Kind::Indexed && (depth == ColorDepth::Ansi256 || _a < 16)) return *this;
        const Color c = toRgb();
        const int   r = c.red(), g = c.green(), b = c.blue();
        if (depth == ColorDepth::Ansi256) {
                // The nearer of the cube and the gray ramp.
                const uint8_t ri = nearestCubeLevel(c.red()), gi = nearestCubeLevel(c.green()),
                              bi = nearestCubeLevel(c.blue());
                const int cube = distance(r, g, b, CubeLevels[ri], CubeLevels[gi], CubeLevels[bi]);
                const int avg = (r + g + b) / 3;
                int       grayIndex = avg > 238 ? 23 : (avg < 8 ? 0 : (avg - 8 + 5) / 10);
                if (grayIndex > 23) grayIndex = 23;
                const int level = 8 + 10 * grayIndex;
                const int gray = distance(r, g, b, level, level, level);
                if (gray < cube) return indexed(static_cast<uint8_t>(232 + grayIndex));
                return indexed(static_cast<uint8_t>(16 + 36 * ri + 6 * gi + bi));
        }
        uint8_t best = 0;
        int     bestDistance = 1 << 30;
        for (uint8_t i = 0; i < 16; ++i) {
                const int d = distance(r, g, b, Ansi[i][0], Ansi[i][1], Ansi[i][2]);
                if (d < bestDistance) {
                        bestDistance = d;
                        best = i;
                }
        }
        return indexed(best);
}

BMDS_NAMESPACE_END
