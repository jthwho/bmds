/**
 * @file      theme.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Themes (devplan/preso.md §5): paper's values as the start of every theme, the palettes, fonts
 * by name and the colours scripts name. Reading themes (themeset.cpp) is the deck's: angel's
 * stage (§8c) has paper and doesn't bring in the deck.
 */

#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include "images.h"
#include "platform.h"

namespace preso {

        namespace {

                constexpr const char *Sans = "IBM Plex Sans";
                constexpr const char *Mono = "IBM Plex Mono";

                constexpr Theme makePaper() {
                        Theme t;
                        t.name = "paper";
                        t.body = TextStyle{FontKey{Sans, 30, 0}, 0x222222};
                        t.heading[0].text = TextStyle{FontKey{Sans, 60, FontKey::Bold}, 0x111111};
                        t.heading[1].text = TextStyle{FontKey{Sans, 46, FontKey::Bold}, 0x111111};
                        t.heading[2].text = TextStyle{FontKey{Sans, 36, FontKey::Bold}, 0x222222};
                        t.code = TextStyle{FontKey{Mono, 24, 0}, 0x222222};
                        t.header = TextStyle{FontKey{Sans, 18, 0}, 0x888888};
                        // The editor's light colours, a touch deeper for a projector.
                        t.syntax[1] = 0x8B1E8B;  // keyword
                        t.syntax[2] = 0x1F5FA8;  // type
                        t.syntax[3] = 0xA04000;  // constant
                        t.syntax[4] = 0x006E7A;  // number
                        t.syntax[5] = 0x2E7D32;  // string
                        t.syntax[6] = 0x2E7D32;  // character
                        t.syntax[7] = 0x808080;  // comment
                        t.syntax[8] = 0x9E6A00;  // preprocessor
                        t.syntax[9] = 0x444444;  // operator
                        t.syntax[10] = 0x1F5FA8; // function
                        t.syntax[11] = 0x8B1E8B; // heading
                        t.syntax[12] = 0x444444; // emphasis
                        t.syntax[13] = 0x2E7D32; // added
                        t.syntax[14] = 0xB00020; // removed
                        t.syntax[15] = 0xB00020; // error
                        return t;
                }

                constexpr Theme Paper = makePaper();

                // ---- Palettes

                constexpr Color Ega[] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                         0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                         0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
                constexpr Color Cga[] = {0x000000, 0x55FFFF, 0xFF55FF, 0xFFFFFF};
                constexpr Color Amber[] = {0x120A00, 0x2E1C00, 0x523300, 0x7A4D00,
                                           0xA36800, 0xCC8400, 0xFFB000, 0xFFD480};
                constexpr Color Green[] = {0x001200, 0x002E08, 0x005214, 0x007A1F,
                                           0x00A32A, 0x10CC38, 0x33FF33, 0xA0FFA0};
                constexpr Color Gray[] = {0x000000, 0x242424, 0x494949, 0x6D6D6D,
                                          0x929292, 0xB6B6B6, 0xDBDBDB, 0xFFFFFF};
                constexpr Color Mono1[] = {0x000000, 0xFFFFFF};

                struct NamedPalette {
                                const char  *name;
                                const Color *colors;
                                uint32_t     count;
                };
                constexpr NamedPalette Palettes[] = {{"ega", Ega, 16},    {"cga", Cga, 4},   {"amber", Amber, 8},
                                                     {"green", Green, 8}, {"gray", Gray, 8}, {"mono", Mono1, 2}};

                // ---- The built-in themes (our own, drawn in the spirit of their machines)

        } // namespace

        // ---- Fonts and palettes

        bool parseFont(StringView text, Arena &arena, FontKey *font) {
                // From the end: a size, then styles; the words before them are the family.
                const char *start = nullptr;
                const char *end = nullptr;
                int32_t     size = 0;
                uint8_t     style = 0;
                bool        styled = false;
                bool        bad = false;
                // Collect the words first (at most a few).
                StringView words[12];
                uint32_t   n = 0;
                forEachWord(text, [&](StringView w) {
                        if (n < 12) {
                                words[n++] = w;
                        } else {
                                bad = true;
                        }
                });
                if (bad || n == 0) return false;
                uint32_t familyEnd = n;
                while (familyEnd > 0) {
                        StringView w = words[familyEnd - 1];
                        if (w.equalsIgnoringCase("bold")) {
                                style |= FontKey::Bold;
                                styled = true;
                        } else if (w.equalsIgnoringCase("italic")) {
                                style |= FontKey::Italic;
                                styled = true;
                        } else if (w.equalsIgnoringCase("regular")) {
                                styled = true;
                        } else if (w.toInt(-1) > 0 && size == 0) {
                                size = w.toInt(-1);
                        } else {
                                break;
                        }
                        familyEnd--;
                }
                if (size != 0 && (size < 4 || size > 255)) return false;
                if (familyEnd > 0) {
                        start = words[0].data();
                        end = words[familyEnd - 1].data() + words[familyEnd - 1].length();
                        bool       ok = false;
                        StringView family = arena.copy(StringView(start, uint32_t(end - start)), &ok);
                        if (!ok) return false;
                        font->family = family.data();
                }
                if (size != 0) font->size = uint16_t(size);
                if (styled) font->style = style;
                return true;
        }

        bool builtInPalette(StringView name, Palette *palette) {
                for (const NamedPalette &p : Palettes) {
                        if (name.equalsIgnoringCase(p.name)) {
                                palette->colors = p.colors;
                                palette->count = p.count;
                                return true;
                        }
                }
                return false;
        }

        const Theme &Theme::paper() {
                return Paper;
        }

        bool Theme::scriptColor(StringView wanted, Color *color) const {
                if (wanted.startsWith("Script.")) {
                        StringView own = wanted.mid(7);
                        for (uint32_t i = 0; i < scriptColorCount; i++) {
                                if (scriptColors[i].name.equals(own)) {
                                        *color = scriptColors[i].color;
                                        return true;
                                }
                        }
                        // The ones every theme has, from its own colours if it doesn't say.
                        if (own.equals("Accent")) {
                                *color = accent;
                        } else if (own.equals("Highlight")) {
                                *color = 0xE66100;
                        } else if (own.equals("Muted")) {
                                *color = quoteColor;
                        } else if (own.equals("Line")) {
                                *color = body.color;
                        } else if (own.equals("Fill")) {
                                *color = codeBackground;
                        } else {
                                return false;
                        }
                        return true;
                }
                struct Named {
                                const char *name;
                                Color       color;
                };
                const Named named[] = {
                        {"Background", background},
                        {"Text", body.color},
                        {"Body", body.color},
                        {"H1", heading[0].text.color},
                        {"H2", heading[1].text.color},
                        {"H3", heading[2].text.color},
                        {"Accent", accent},
                        {"Link", link},
                        {"Code", code.color},
                        {"CodeBackground", codeBackground},
                        {"Quote", quoteColor},
                        {"Rule", rule},
                };
                for (const Named &n : named) {
                        if (wanted.equals(n.name)) {
                                *color = n.color;
                                return true;
                        }
                }
                return false;
        }

} // namespace preso
