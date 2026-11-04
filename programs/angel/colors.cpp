/**
 * @file      colors.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Colours as scripts write them, 0xRRGGBB in a uint: the named ones (`Color::Red`), and rgb(),
 * hsv() and mix(). Every host that draws has them: angel's graphics, Preso's decks
 * (devplan/preso.md §7.9).
 */

#include <math.h>
#include <stdio.h>
#include "angel.h"

namespace angel {

        namespace {

                struct NamedColor {
                                const char *name;
                                uint32_t    rgb;
                };
                constexpr NamedColor colors[] = {
                        {"Black", 0x000000},    {"White", 0xffffff},   {"Gray", 0x808080},   {"LightGray", 0xc0c0c0},
                        {"DarkGray", 0x404040}, {"Red", 0xff0000},     {"Green", 0x00a000},  {"Lime", 0x00ff00},
                        {"Blue", 0x0000ff},     {"Navy", 0x000080},    {"Sky", 0x87ceeb},    {"Yellow", 0xffff00},
                        {"Cyan", 0x00ffff},     {"Magenta", 0xff00ff}, {"Orange", 0xff8000}, {"Purple", 0x800080},
                        {"Brown", 0x8b4513},    {"Pink", 0xffc0cb},
                };
                uint32_t colorValues[sizeof(colors) / sizeof(colors[0])];

                uint32_t channel(int v) {
                        return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v);
                }
                void rgb(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(channel(int(gen->GetArgDWord(0))) << 16 |
                                            channel(int(gen->GetArgDWord(1))) << 8 | channel(int(gen->GetArgDWord(2))));
                }
                void hsv(asIScriptGeneric *gen) {
                        float       h = gen->GetArgFloat(0) / 60.0f;
                        const float s = fminf(fmaxf(gen->GetArgFloat(1), 0), 1),
                                    v = fminf(fmaxf(gen->GetArgFloat(2), 0), 1);
                        h -= 6.0f * floorf(h / 6.0f);
                        const int   sector = int(h) % 6;
                        const float f = h - floorf(h);
                        const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
                        float       r = v, g = t, b = p;
                        switch (sector) {
                                case 1: r = q, g = v, b = p; break;
                                case 2: r = p, g = v, b = t; break;
                                case 3: r = p, g = q, b = v; break;
                                case 4: r = t, g = p, b = v; break;
                                case 5: r = v, g = p, b = q; break;
                                default: break;
                        }
                        gen->SetReturnDWord(uint32_t(r * 255 + 0.5f) << 16 | uint32_t(g * 255 + 0.5f) << 8 |
                                            uint32_t(b * 255 + 0.5f));
                }
                void mix(asIScriptGeneric *gen) {
                        const uint32_t a = gen->GetArgDWord(0) & 0xffffffu, b = gen->GetArgDWord(1) & 0xffffffu;
                        const float    t = fminf(fmaxf(gen->GetArgFloat(2), 0), 1);
                        uint32_t       out = 0;
                        for (int shift = 0; shift <= 16; shift += 8) {
                                const float x = float((a >> shift) & 0xff), y = float((b >> shift) & 0xff);
                                out |= uint32_t(x + (y - x) * t + 0.5f) << shift;
                        }
                        gen->SetReturnDWord(out);
                }

        } // namespace

        void registerColors(asIScriptEngine *engine) {
                check(engine->SetDefaultNamespace("Color"), "namespace Color");
                for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
                        colorValues[i] = colors[i].rgb;
                        char decl[48];
                        snprintf(decl, sizeof(decl), "const uint %s", colors[i].name);
                        property(engine, decl, &colorValues[i], nullptr);
                }
                check(engine->SetDefaultNamespace(""), "namespace");
                def(engine, "uint rgb(int red, int green, int blue)", rgb,
                    "A colour from its red, green and blue, each 0 to 255.");
                def(engine, "uint hsv(float hue, float saturation, float value)", hsv,
                    "A colour from its hue (0 to 360 degrees: red, yellow, green, cyan, blue, magenta), saturation and "
                    "value "
                    "(0 to 1).");
                def(engine, "uint mix(uint color1, uint color2, float amount)", mix,
                    "A colour between two: amount 0 is the first, 1 the second.");
                return;
        }

} // namespace angel
