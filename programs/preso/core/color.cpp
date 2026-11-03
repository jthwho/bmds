/**
 * @file      color.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "color.h"

namespace preso {

        namespace {

                struct Named {
                                const char *name;
                                Color       color;
                };

                // CSS's basic colours, and a few more decks use.
                constexpr Named Names[] = {
                        {"black", 0x000000},   {"silver", 0xC0C0C0}, {"gray", 0x808080},    {"grey", 0x808080},
                        {"white", 0xFFFFFF},   {"maroon", 0x800000}, {"red", 0xFF0000},     {"purple", 0x800080},
                        {"fuchsia", 0xFF00FF}, {"green", 0x008000},  {"lime", 0x00FF00},    {"olive", 0x808000},
                        {"yellow", 0xFFFF00},  {"navy", 0x000080},   {"blue", 0x0000FF},    {"teal", 0x008080},
                        {"aqua", 0x00FFFF},    {"orange", 0xFFA500}, {"magenta", 0xFF00FF}, {"cyan", 0x00FFFF},
                };

                int hexDigit(char c) {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                }

        } // namespace

        bool parseColor(StringView text, Color *color) {
                StringView t = text.trimmed();
                if (t.startsWith("#") && (t.length() == 4 || t.length() == 7)) {
                        uint32_t value = 0;
                        for (uint32_t i = 1; i < t.length(); i++) {
                                int d = hexDigit(t[i]);
                                if (d < 0) return false;
                                value = t.length() == 4 ? (value << 8) | uint32_t(d * 17) : (value << 4) | uint32_t(d);
                        }
                        *color = value;
                        return true;
                }
                for (const Named &n : Names) {
                        if (t.equalsIgnoringCase(n.name)) {
                                *color = n.color;
                                return true;
                        }
                }
                return false;
        }

        Color mixColors(Color a, Color b, uint32_t amount) {
                uint32_t keep = 256 - amount;
                uint32_t r = (((a >> 16) & 0xFF) * keep + ((b >> 16) & 0xFF) * amount) >> 8;
                uint32_t g = (((a >> 8) & 0xFF) * keep + ((b >> 8) & 0xFF) * amount) >> 8;
                uint32_t bl = ((a & 0xFF) * keep + (b & 0xFF) * amount) >> 8;
                return (r << 16) | (g << 8) | bl;
        }

} // namespace preso
