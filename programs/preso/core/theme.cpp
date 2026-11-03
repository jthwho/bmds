/**
 * @file      theme.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Themes (devplan/preso.md §5): paper's values as the start of every theme, the INI format read
 * over them, the built-in themes as INI text, and class variants made as slides ask for them.
 */

#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include "deck.h"
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

                constexpr char PaperText[] = "[Theme]\nName=Paper\n"
                                             "[Class title]\nAlign=middle\nCenter=true\n"
                                             "[Class lead]\nAlign=middle\nCenter=true\n";

                // A PC's text mode: blue, unscii, the EGA palette, a double frame round the title, a
                // grey status line, at 640×360 in sharp whole pixels.
                constexpr char VgaText[] =
                        "[Theme]\nName=VGA\nSize=640x360\nScale=nearest\nPalette=ega\n"
                        "[Slide]\nBackground=#0000AA\nColor=#AAAAAA\nFont=unscii 16\nMargin=28 32 32\n"
                        "Accent=#55FFFF\nGap=0.5\n"
                        "[H1]\nFont=unscii 32\nColor=#FFFF55\nFrame=double\nFrameColor=#FFFFFF\n"
                        "[H2]\nFont=unscii 16\nColor=#FFFFFF\nUnderline=line\nUnderlineColor=#AAAAAA\n"
                        "[H3]\nFont=unscii 16\nColor=#55FFFF\n"
                        "[Bullet]\nGlyphs=\xE2\x96\xA0 \xE2\x96\xAA \xC2\xB7\nColor=#55FFFF\n"
                        "[List]\nIndent=24\n"
                        "[Code]\nFont=unscii 16\nBackground=#000000\nColor=#AAAAAA\nInlineBackground=#000088\n"
                        "Padding=8\nKeyword=#FFFF55\nType=#55FFFF\nConstant=#FF55FF\nNumber=#55FF55\n"
                        "String=#55FF55\nCharacter=#55FF55\nComment=#5555FF\nPreprocessor=#FF5555\n"
                        "Operator=#FFFFFF\nFunction=#55FFFF\nAdded=#55FF55\nRemoved=#FF5555\n"
                        "[Quote]\nBar=#55FFFF\nColor=#FFFFFF\nIndent=16\n"
                        "[Table]\nHeader=#0000FF\nRule=#5555FF\n"
                        "[Link]\nColor=#55FFFF\n"
                        "[Header]\nFont=unscii 16\nColor=#000000\nBackground=#AAAAAA\n"
                        "[Class title]\nAlign=middle\nCenter=true\nH1.Font=unscii 48\n"
                        "[Class lead]\nAlign=middle\nCenter=true\n";

                // A terminal's phosphor: one colour on black, its shades the palette, scanlines and a glow.
                constexpr char AmberText[] =
                        "[Theme]\nName=Amber\nPalette=amber\nEffects=scanlines glow\nScanlines=30\nGlow=55\n"
                        "[Slide]\nBackground=#120A00\nColor=#FFB000\nFont=unscii 32\nAccent=#FFD480\n"
                        "[H1]\nFont=unscii 64\nColor=#FFD480\nUnderline=line\n"
                        "[H2]\nFont=unscii 48\nColor=#FFC040\n"
                        "[H3]\nFont=unscii 32\nColor=#FFD480\n"
                        "[Bullet]\nGlyphs=> - *\n"
                        "[Code]\nFont=unscii 32\nBackground=#1E1200\nColor=#FFB000\n"
                        "InlineBackground=#2E1C00\nKeyword=#FFD480\nType=#FFD480\nComment=#A36800\n"
                        "String=#FFC040\nNumber=#FFC040\nConstant=#FFC040\nOperator=#CC8400\n"
                        "Function=#FFD480\nPreprocessor=#CC8400\n"
                        "[Quote]\nBar=#A36800\nColor=#CC8400\n"
                        "[Table]\nHeader=#2E1C00\nRule=#7A4D00\n"
                        "[Link]\nColor=#FFD480\n"
                        "[Header]\nFont=unscii 16\nColor=#A36800\n"
                        "[Class title]\nAlign=middle\nCenter=true\nH1.Font=unscii 96\n"
                        "[Class lead]\nAlign=middle\nCenter=true\n";

                constexpr char GreenText[] =
                        "[Theme]\nName=Green\nExtends=amber\nPalette=green\n"
                        "[Slide]\nBackground=#001200\nColor=#33FF33\nAccent=#A0FFA0\n"
                        "[H1]\nColor=#A0FFA0\n[H2]\nColor=#66FF66\n[H3]\nColor=#A0FFA0\n"
                        "[Code]\nBackground=#002408\nColor=#33FF33\nInlineBackground=#002E08\n"
                        "Keyword=#A0FFA0\nType=#A0FFA0\nComment=#00A32A\nString=#66FF66\nNumber=#66FF66\n"
                        "Constant=#66FF66\nOperator=#10CC38\nFunction=#A0FFA0\nPreprocessor=#10CC38\n"
                        "[Quote]\nBar=#00A32A\nColor=#10CC38\n"
                        "[Table]\nHeader=#002E08\nRule=#007A1F\n"
                        "[Link]\nColor=#A0FFA0\n"
                        "[Header]\nColor=#00A32A\n";

                // The toolkit's own look: grey, a navy title bar on each slide, a raised window.
                constexpr char ClassicText[] =
                        "[Theme]\nName=Classic\n"
                        "[Slide]\nBackground=#C0C0C0\nColor=#000000\nFont=IBM Plex Sans 28\n"
                        "Accent=#000080\nFrame=raised\nMargin=40 64 56\n"
                        "[H1]\nFont=IBM Plex Sans 32 bold\nColor=#FFFFFF\nBackground=#000080\nBar=true\n"
                        "[H2]\nFont=IBM Plex Sans 30 bold\nColor=#000080\n"
                        "[H3]\nFont=IBM Plex Sans 28 bold\nColor=#000000\n"
                        "[Bullet]\nColor=#000000\n"
                        "[Code]\nFont=IBM Plex Mono 22\nBackground=#FFFFFF\nFrame=sunken\n"
                        "InlineBackground=#FFFFFF\n"
                        "[Quote]\nBar=#808080\nColor=#404040\n"
                        "[Table]\nHeader=#D4D0C8\nRule=#808080\n"
                        "[Link]\nColor=#0000FF\n"
                        "[Header]\nFont=IBM Plex Sans 16\nColor=#404040\n"
                        "[Class title]\nAlign=middle\nCenter=true\nH1.Bar=false\nH1.Background=none\n"
                        "H1.Color=#000080\nH1.Font=IBM Plex Sans 60 bold\n"
                        "[Class lead]\nAlign=middle\nCenter=true\n";

                // A bulletin board: ANSI colours on black, block-shade dividers, 640×360 sharp.
                constexpr char BbsText[] =
                        "[Theme]\nName=BBS\nSize=640x360\nScale=nearest\nPalette=ega\n"
                        "[Slide]\nBackground=#000000\nColor=#AAAAAA\nFont=unscii 16\nMargin=24 32 32\n"
                        "Accent=#FF55FF\nGap=0.5\n"
                        "[H1]\nFont=unscii 32\nColor=#55FFFF\nUnderline=shade\nUnderlineColor=#5555FF\n"
                        "[H2]\nFont=unscii 16\nColor=#FF55FF\nUnderline=shade\nUnderlineColor=#AA00AA\n"
                        "[H3]\nFont=unscii 16\nColor=#FFFF55\n"
                        "[Bullet]\nGlyphs=\xC2\xBB \xE2\x80\xBA \xC2\xB7\n"
                        "[List]\nIndent=24\n"
                        "[Code]\nFont=unscii 16\nBackground=#000000\nColor=#55FF55\nFrame=single\nPadding=8\n"
                        "InlineBackground=#000000\nKeyword=#FFFF55\nType=#55FFFF\nComment=#555555\n"
                        "String=#FF55FF\nNumber=#FF5555\nConstant=#FF5555\nOperator=#AAAAAA\n"
                        "Function=#55FFFF\nPreprocessor=#FF5555\n"
                        "[Quote]\nBar=#FF55FF\nColor=#FFFFFF\nIndent=16\n"
                        "[Table]\nHeader=#0000AA\nRule=#555555\n"
                        "[Link]\nColor=#55FFFF\n"
                        "[Header]\nFont=unscii 16\nColor=#555555\n"
                        "[Class title]\nAlign=middle\nCenter=true\nH1.Font=unscii 48\n"
                        "[Class lead]\nAlign=middle\nCenter=true\n";

                struct BuiltIn {
                                const char *name;
                                const char *text;
                };
                constexpr BuiltIn BuiltIns[] = {{"paper", PaperText}, {"vga", VgaText},         {"amber", AmberText},
                                                {"green", GreenText}, {"classic", ClassicText}, {"bbs", BbsText}};
                constexpr const char *BuiltInNames[] = {"paper", "vga", "amber", "green", "classic", "bbs", nullptr};

                const char *builtIn(StringView name) {
                        if (name.isEmpty() || name.equals("default")) return PaperText;
                        for (const BuiltIn &b : BuiltIns) {
                                if (name.equals(b.name)) return b.text;
                        }
                        return nullptr;
                }

                bool parseBool(StringView v, bool *out) {
                        if (v.equalsIgnoringCase("true") || v.equalsIgnoringCase("yes") || v.equals("1")) {
                                *out = true;
                                return true;
                        }
                        if (v.equalsIgnoringCase("false") || v.equalsIgnoringCase("no") || v.equals("0")) {
                                *out = false;
                                return true;
                        }
                        return false;
                }

                bool parseColorOrNone(StringView v, Color *out) {
                        if (v.equalsIgnoringCase("none") || v.equalsIgnoringCase("transparent")) {
                                *out = NoColor;
                                return true;
                        }
                        return parseColor(v, out);
                }

                bool parseFrame(StringView v, Frame *out) {
                        static const char *const names[] = {"none", "single", "double", "raised", "sunken"};
                        for (uint32_t i = 0; i < 5; i++) {
                                if (v.equalsIgnoringCase(names[i])) {
                                        *out = Frame(i);
                                        return true;
                                }
                        }
                        return false;
                }

                bool parseUnderline(StringView v, Underline *out) {
                        static const char *const names[] = {"none", "line", "double", "shade"};
                        for (uint32_t i = 0; i < 4; i++) {
                                if (v.equalsIgnoringCase(names[i])) {
                                        *out = Underline(i);
                                        return true;
                                }
                        }
                        return false;
                }

                // A percentage (0-100) as a fraction of 255.
                bool parseStrength(StringView v, uint8_t *out) {
                        if (v.endsWith("%")) v = v.left(v.length() - 1);
                        int32_t n = v.toInt(-1);
                        if (n < 0 || n > 100) return false;
                        *out = uint8_t(n * 255 / 100);
                        return true;
                }

                bool parseSize(StringView v, int32_t *w, int32_t *h) {
                        if (v.equals("16:9")) {
                                *w = 1280;
                                *h = 720;
                                return true;
                        }
                        if (v.equals("4:3")) {
                                *w = 960;
                                *h = 720;
                                return true;
                        }
                        int32_t x = v.indexOf('x');
                        if (x <= 0) return false;
                        int32_t a = v.left(uint32_t(x)).toInt(-1), b = v.mid(uint32_t(x) + 1).toInt(-1);
                        if (a < 64 || a > 1280 || b < 64 || b > 720) return false;
                        *w = a & ~3;
                        *h = b;
                        return true;
                }

                constexpr const char *SyntaxNames[16] = {"Normal",       "Keyword",  "Type",      "Constant",
                                                         "Number",       "String",   "Character", "Comment",
                                                         "Preprocessor", "Operator", "Function",  "Heading",
                                                         "Emphasis",     "Added",    "Removed",   "Error"};

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

        const char *const *ThemeSet::builtInNames() {
                return BuiltInNames;
        }

        const char *ThemeSet::builtInText(StringView name) {
                return name.isEmpty() ? nullptr : builtIn(name);
        }

        // ---- Reading

        /** @brief A `[Class NAME]` line, applied when a slide of that class asks. */
        struct ThemeSet::Line {
                        StringView className;
                        StringView section;
                        StringView key;
                        StringView value;
                        Line      *next = nullptr;
        };

        struct ThemeSet::Variant {
                        StringView classes;
                        Theme      theme;
                        Variant   *next = nullptr;
        };

        bool ThemeSet::apply(Theme &t, StringView section, StringView key, StringView v, const char **why) {
                *why = "isn't a key of its section";
                Color   c = 0;
                int32_t n = 0;
                if (section.equals("Theme")) {
                        if (key.equals("Name")) {
                                bool ok = false;
                                t.name = _arena.copy(v, &ok).data();
                                return ok;
                        }
                        if (key.equals("Extends")) return true; // read before the rest (read())
                        if (key.equals("Size")) {
                                *why = "is WIDTHxHEIGHT (at most 1280x720), 16:9 or 4:3";
                                return parseSize(v, &t.width, &t.height);
                        }
                        if (key.equals("Scale")) {
                                *why = "is smooth or nearest";
                                if (v.equals("nearest")) {
                                        t.nearest = true;
                                        return true;
                                }
                                if (v.equals("smooth")) {
                                        t.nearest = false;
                                        return true;
                                }
                                return false;
                        }
                        if (key.equals("Palette")) {
                                *why = "is ega, cga, amber, green, gray, mono, none, or colours (#rrggbb ...)";
                                if (v.equalsIgnoringCase("none")) {
                                        t.palette = Palette();
                                        return true;
                                }
                                if (builtInPalette(v, &t.palette)) return true;
                                uint32_t count = 0;
                                bool     ok = true;
                                forEachWord(v, [&](StringView) { count++; });
                                if (count < 2 || count > 256) return false;
                                auto *colors =
                                        static_cast<Color *>(_arena.allocate(count * sizeof(Color), alignof(Color)));
                                if (colors == nullptr) return false;
                                uint32_t i = 0;
                                forEachWord(v, [&](StringView w) {
                                        if (!parseColor(w, &colors[i++])) ok = false;
                                });
                                if (!ok) return false;
                                t.palette = Palette{colors, count};
                                return true;
                        }
                        if (key.equals("Effects")) {
                                *why = "lists scanlines and glow (or none)";
                                bool ok = true, scan = false, glow = false;
                                forEachWord(v, [&](StringView w) {
                                        if (w.equals("scanlines")) {
                                                scan = true;
                                        } else if (w.equals("glow")) {
                                                glow = true;
                                        } else if (!w.equals("none") && !w.equals("dither")) {
                                                ok = false;
                                        }
                                });
                                if (!ok) return false;
                                // Named without a strength: a moderate one; not named: none.
                                t.scanlines = scan ? (t.scanlines != 0 ? t.scanlines : 64) : 0;
                                t.glow = glow ? (t.glow != 0 ? t.glow : 128) : 0;
                                return true;
                        }
                        if (key.equals("Scanlines")) {
                                *why = "is a percentage, 0 to 100";
                                return parseStrength(v, &t.scanlines);
                        }
                        if (key.equals("Glow")) {
                                *why = "is a percentage, 0 to 100";
                                return parseStrength(v, &t.glow);
                        }
                        if (key.equals("GlowRadius")) {
                                *why = "is pixels, 1 to 32";
                                n = v.toInt(-1);
                                if (n < 1 || n > 32) return false;
                                t.glowRadius = uint8_t(n);
                                return true;
                        }
                        return false;
                }
                if (section.equals("Slide")) {
                        if (key.equals("Background")) {
                                *why = "is a colour";
                                return parseColor(v, &t.background);
                        }
                        if (key.equals("Color")) {
                                *why = "is a colour";
                                return parseColor(v, &t.body.color);
                        }
                        if (key.equals("Accent")) {
                                *why = "is a colour";
                                return parseColor(v, &t.accent);
                        }
                        if (key.equals("Font")) {
                                *why = "is a font: a family, a size, a style (unscii 32, IBM Plex Sans 30 bold)";
                                return parseFont(v, _arena, &t.body.font);
                        }
                        if (key.equals("Pattern")) {
                                // Named from the theme file's folder; kept as the deck's folder names it.
                                if (v.isEmpty() || v.equalsIgnoringCase("none")) {
                                        t.pattern = nullptr;
                                        return true;
                                }
                                char path[1024];
                                int  written = v.startsWith("/") || _relative[0] == '\0'
                                                       ? snprintf(path, sizeof path, "%.*s", int(v.length()), v.data())
                                                       : snprintf(path, sizeof path, "%s/%.*s", _relative,
                                                                  int(v.length()), v.data());
                                if (written <= 0 || size_t(written) >= sizeof path) return false;
                                bool ok = false;
                                t.pattern = _arena.copy(StringView(path, uint32_t(written)), &ok).data();
                                return ok;
                        }
                        if (key.equals("Frame")) {
                                *why = "is none, single, double, raised or sunken";
                                return parseFrame(v, &t.slideFrame);
                        }
                        if (key.equals("Margin")) {
                                // CSS's order: top, right, bottom, left (one, two, three or four).
                                *why = "is 1 to 4 numbers of pixels: top, right, bottom, left";
                                int32_t  m[4] = {0, 0, 0, 0};
                                uint32_t count = 0;
                                bool     ok = true;
                                forEachWord(v, [&](StringView w) {
                                        int32_t x = w.toInt(-1);
                                        if (count >= 4 || x < 0 || x > 400) ok = false;
                                        if (count < 4) m[count++] = x;
                                });
                                if (!ok || count == 0) return false;
                                t.marginTop = m[0];
                                t.marginRight = count >= 2 ? m[1] : m[0];
                                t.marginBottom = count >= 3 ? m[2] : m[0];
                                t.marginLeft = count >= 4 ? m[3] : t.marginRight;
                                return true;
                        }
                        if (key.equals("Align")) {
                                *why = "is top or middle";
                                if (v.equals("middle")) {
                                        t.middle = true;
                                        return true;
                                }
                                if (v.equals("top")) {
                                        t.middle = false;
                                        return true;
                                }
                                return false;
                        }
                        if (key.equals("Center")) {
                                *why = "is true or false";
                                return parseBool(v, &t.center);
                        }
                        if (key.equals("Gap")) {
                                *why = "is the space between blocks, in lines (0 to 4)";
                                float g = v.toFloat(-1.0f);
                                if (g < 0.0f || g > 4.0f) return false;
                                t.blockGap16 = int32_t(g * 16.0f + 0.5f);
                                return true;
                        }
                        if (key.equals("Placeholder")) {
                                *why = "is a colour";
                                return parseColor(v, &t.placeholder);
                        }
                        return false;
                }
                if (section.equals("H1") || section.equals("H2") || section.equals("H3")) {
                        HeadingStyle &h = t.heading[section[1] - '1'];
                        if (key.equals("Font")) {
                                *why = "is a font: a family, a size, a style";
                                return parseFont(v, _arena, &h.text.font);
                        }
                        if (key.equals("Color")) {
                                *why = "is a colour";
                                return parseColor(v, &h.text.color);
                        }
                        if (key.equals("Background")) {
                                *why = "is a colour, or none";
                                return parseColorOrNone(v, &h.background);
                        }
                        if (key.equals("Bar")) {
                                *why = "is true or false";
                                return parseBool(v, &h.bar);
                        }
                        if (key.equals("Frame")) {
                                *why = "is none, single, double, raised or sunken";
                                return parseFrame(v, &h.frame);
                        }
                        if (key.equals("FrameColor")) {
                                *why = "is a colour";
                                return parseColorOrNone(v, &h.frameColor);
                        }
                        if (key.equals("Underline")) {
                                *why = "is none, line, double or shade";
                                return parseUnderline(v, &h.underline);
                        }
                        if (key.equals("UnderlineColor")) {
                                *why = "is a colour";
                                return parseColorOrNone(v, &h.underlineColor);
                        }
                        return false;
                }
                if (section.equals("Bullet")) {
                        if (key.equals("Glyphs")) {
                                *why = "is up to three glyphs, by depth";
                                uint32_t i = 0;
                                bool     ok = true;
                                forEachWord(v, [&](StringView w) {
                                        if (i >= 3) {
                                                ok = false;
                                                return;
                                        }
                                        bool copied = false;
                                        t.bullets[i++] = _arena.copy(w, &copied).data();
                                        if (!copied) ok = false;
                                });
                                // Fewer than three: the last repeats.
                                for (uint32_t k = i; ok && i > 0 && k < 3; k++) t.bullets[k] = t.bullets[i - 1];
                                return ok && i > 0;
                        }
                        if (key.equals("Color")) {
                                *why = "is a colour";
                                return parseColorOrNone(v, &t.bulletColor);
                        }
                        return false;
                }
                if (section.equals("List")) {
                        if (key.equals("Indent")) {
                                *why = "is pixels";
                                n = v.toInt(-1);
                                if (n < 0 || n > 400) return false;
                                t.listIndent = n;
                                return true;
                        }
                        return false;
                }
                if (section.equals("Code")) {
                        if (key.equals("Font")) {
                                *why = "is a font: a family, a size, a style";
                                return parseFont(v, _arena, &t.code.font);
                        }
                        if (key.equals("Color")) {
                                *why = "is a colour";
                                return parseColor(v, &t.code.color);
                        }
                        if (key.equals("Background")) {
                                *why = "is a colour";
                                return parseColor(v, &t.codeBackground);
                        }
                        if (key.equals("InlineBackground")) {
                                *why = "is a colour";
                                return parseColor(v, &t.inlineCodeBackground);
                        }
                        if (key.equals("Frame")) {
                                *why = "is none, single, double, raised or sunken";
                                return parseFrame(v, &t.codeFrame);
                        }
                        if (key.equals("Padding")) {
                                *why = "is pixels";
                                n = v.toInt(-1);
                                if (n < 0 || n > 200) return false;
                                t.codePadding = n;
                                return true;
                        }
                        for (uint32_t i = 1; i < 16; i++) {
                                if (key.equals(SyntaxNames[i])) {
                                        *why = "is a colour";
                                        return parseColor(v, &t.syntax[i]);
                                }
                        }
                        return false;
                }
                if (section.equals("Quote")) {
                        if (key.equals("Bar")) {
                                *why = "is a colour";
                                return parseColor(v, &t.quoteBar);
                        }
                        if (key.equals("Color")) {
                                *why = "is a colour";
                                return parseColor(v, &t.quoteColor);
                        }
                        if (key.equals("Indent")) {
                                *why = "is pixels";
                                n = v.toInt(-1);
                                if (n < 0 || n > 400) return false;
                                t.quoteIndent = n;
                                return true;
                        }
                        return false;
                }
                if (section.equals("Table")) {
                        *why = "is a colour";
                        if (key.equals("Header")) return parseColor(v, &t.tableHeaderBackground);
                        if (key.equals("Rule")) return parseColor(v, &t.rule);
                        *why = "isn't a key of its section";
                        return false;
                }
                if (section.equals("Link")) {
                        *why = "is a colour";
                        if (key.equals("Color")) return parseColor(v, &t.link);
                        *why = "isn't a key of its section";
                        return false;
                }
                if (section.equals("Header") || section.equals("Footer")) {
                        if (key.equals("Font")) {
                                *why = "is a font: a family, a size, a style";
                                return parseFont(v, _arena, &t.header.font);
                        }
                        if (key.equals("Color")) {
                                *why = "is a colour";
                                return parseColor(v, &t.header.color);
                        }
                        if (key.equals("Background")) {
                                *why = "is a colour, or none";
                                return parseColorOrNone(v, &t.headerBackground);
                        }
                        return false;
                }
                if (section.equals("Script")) return true; // the scripts' colours and fonts (step 6)
                (void)c;
                *why = "is in a section themes don't have";
                return false;
        }

        Error ThemeSet::read(Platform *platform, StringView name, StringView text, const char *file, Deck *deck,
                             uint32_t depth) {
                auto say = [&](uint32_t line, const char *format, StringView a, StringView b) {
                        if (deck == nullptr) return;
                        deck->report(Severity::Warning, 0, "%s:%u: %s `%.*s` %.*s", file, unsigned(line), format,
                                     int(a.length()), a.data(), int(b.length()), b.data());
                };
                // Extends= first: this theme is read over the one it names.
                StringView section;
                uint32_t   lineNumber = 0;
                Error      error = Error::Ok;
                forEachLine(text, [&](StringView raw, uint32_t) {
                        lineNumber++;
                        StringView l = raw.trimmed();
                        if (l.startsWith("[")) {
                                section = l.mid(1, l.length() - 2);
                                return;
                        }
                        if (!section.equals("Theme") || !l.startsWith("Extends")) return;
                        int32_t eq = l.indexOf('=');
                        if (eq < 0) return;
                        StringView base = l.mid(uint32_t(eq) + 1).trimmed();
                        if (depth >= 4 || base.equals(name)) {
                                say(lineNumber, "Extends", base, StringView("goes round in a circle: not read"));
                                return;
                        }
                        const char *builtText = builtIn(base);
                        if (builtText != nullptr) {
                                bool        ok = false;
                                const char *baseName = _arena.copy(base, &ok).data();
                                if (!ok) {
                                        error = Error::NoMemory;
                                        return;
                                }
                                error = read(platform, base, StringView(builtText), baseName, deck, depth + 1);
                        } else {
                                say(lineNumber, "Extends", base, StringView("isn't a built-in theme"));
                        }
                });
                if (error != Error::Ok) return error;
                (void)platform;
                section = StringView();
                StringView className;
                lineNumber = 0;
                forEachLine(text, [&](StringView raw, uint32_t) {
                        lineNumber++;
                        if (error != Error::Ok) return;
                        StringView l = raw.trimmed();
                        if (l.isEmpty() || l.startsWith(";") || l.startsWith("#")) return;
                        if (l.startsWith("[")) {
                                if (!l.endsWith("]")) {
                                        say(lineNumber, "section", l, StringView("has no closing ]"));
                                        section = StringView();
                                        return;
                                }
                                section = l.mid(1, l.length() - 2).trimmed();
                                className = StringView();
                                if (section.startsWith("Class ")) className = section.mid(6).trimmed();
                                return;
                        }
                        int32_t eq = l.indexOf('=');
                        if (eq <= 0) {
                                say(lineNumber, "line", l, StringView("isn't KEY=VALUE"));
                                return;
                        }
                        StringView key = l.left(uint32_t(eq)).trimmed();
                        StringView value = l.mid(uint32_t(eq) + 1);
                        // A comment after the value (` ; ...`).
                        int32_t semi = value.indexOf(" ;");
                        if (semi >= 0) value = value.left(uint32_t(semi));
                        value = value.trimmed();
                        if (!className.isEmpty()) {
                                // `Key=` is the slide's, `Section.Key=` that section's: kept for the
                                // slides of this class, checked now on a copy.
                                StringView sec("Slide");
                                int32_t    dot = key.indexOf('.');
                                if (dot > 0) {
                                        sec = key.left(uint32_t(dot));
                                        key = key.mid(uint32_t(dot) + 1);
                                }
                                Theme       scratch = _base;
                                const char *why = "";
                                if (!apply(scratch, sec, key, value, &why)) {
                                        say(lineNumber, "key", key, StringView(why));
                                        return;
                                }
                                Line *line = _arena.make<Line>();
                                bool  ok1 = false, ok2 = false, ok3 = false, ok4 = false;
                                if (line == nullptr) {
                                        error = Error::NoMemory;
                                        return;
                                }
                                line->className = _arena.copy(className, &ok1);
                                line->section = _arena.copy(sec, &ok2);
                                line->key = _arena.copy(key, &ok3);
                                line->value = _arena.copy(value, &ok4);
                                if (!(ok1 && ok2 && ok3 && ok4)) {
                                        error = Error::NoMemory;
                                        return;
                                }
                                *_classTail = line;
                                _classTail = &line->next;
                                return;
                        }
                        if (section.isEmpty()) {
                                say(lineNumber, "key", key, StringView("is outside a section"));
                                return;
                        }
                        const char *why = "";
                        if (!apply(_base, section, key, value, &why)) say(lineNumber, "key", key, StringView(why));
                });
                return error;
        }

        Error ThemeSet::loadText(StringView text, const char *file, Deck *deck) {
                _base = Paper;
                return read(nullptr, StringView(file), text, file, deck, 0);
        }

        Error ThemeSet::resolve(Platform &platform, StringView name, const char *deckPath, const char *dataFolder,
                                Deck &deck, uint32_t depth) {
                (void)depth;
                folderOf(deckPath, _folder, sizeof _folder);
                char path[1024];
                // A path, or a name: beside the deck, then in the data folder, then built in.
                bool isPath = name.indexOf('/') >= 0 || name.endsWith(".theme");
                for (int where = 0; where < 2; where++) {
                        if (where == 1 && (isPath || dataFolder == nullptr)) break;
                        int n = 0;
                        if (isPath) {
                                n = name.startsWith("/")
                                            ? snprintf(path, sizeof path, "%.*s", int(name.length()), name.data())
                                            : snprintf(path, sizeof path, "%s/%.*s", _folder, int(name.length()),
                                                       name.data());
                        } else if (where == 0) {
                                n = snprintf(path, sizeof path, "%s/%.*s.theme", _folder, int(name.length()),
                                             name.data());
                        } else {
                                n = snprintf(path, sizeof path, "%s/themes/%.*s.theme", dataFolder, int(name.length()),
                                             name.data());
                        }
                        if (n <= 0 || size_t(n) >= sizeof path) continue;
                        char    *text = nullptr;
                        uint32_t length = 0;
                        Error    e = platform.readFile(path, 256u << 10, &text, &length);
                        if (e == Error::NotFound) continue;
                        if (e != Error::Ok) {
                                deck.report(Severity::Warning, 0, "theme %s: %s", path, errorName(e));
                                continue;
                        }
                        // The theme's pictures (a pattern) are named from its own folder.
                        folderOf(path, _folder, sizeof _folder);
                        _relative[0] = '\0';
                        if (where == 1) {
                                snprintf(_relative, sizeof _relative, "%s/themes", dataFolder);
                        } else if (isPath && name.indexOf('/') >= 0) {
                                char named[512];
                                snprintf(named, sizeof named, "%.*s", int(name.length()), name.data());
                                folderOf(named, _relative, sizeof _relative);
                        }
                        _base = Paper;
                        // The file's text is kept: values point into it.
                        bool       ok = false;
                        StringView kept = _arena.copy(StringView(text, length), &ok);
                        free(text);
                        if (!ok) return Error::NoMemory;
                        const char *file = _arena.copy(StringView(path), &ok).data();
                        if (!ok) return Error::NoMemory;
                        return read(&platform, name, kept, file, &deck, 0);
                }
                if (isPath)
                        deck.report(Severity::Warning, 0, "theme `%.*s` isn't there: paper is used", int(name.length()),
                                    name.data());
                const char *text = builtIn(name);
                if (text == nullptr) {
                        if (!isPath) {
                                deck.report(Severity::Warning, 0,
                                            "theme `%.*s` isn't known (built in: paper, vga, amber, green, "
                                            "classic, bbs): paper is used",
                                            int(name.length()), name.data());
                        }
                        text = PaperText;
                }
                _base = Paper;
                return read(&platform, name, StringView(text), name.isEmpty() ? "paper" : "built-in", &deck, 0);
        }

        Error ThemeSet::load(Platform &platform, StringView name, const char *deckPath, const char *dataFolder,
                             Deck &deck) {
                return resolve(platform, name.trimmed(), deckPath, dataFolder, deck, 0);
        }

        const Theme &ThemeSet::forClasses(StringView classes) {
                if (classes.trimmed().isEmpty() || _classLines == nullptr) return _base;
                for (Variant *v = _variants; v != nullptr; v = v->next) {
                        if (v->classes.equals(classes)) return v->theme;
                }
                Variant *v = _arena.make<Variant>();
                bool     ok = false;
                if (v == nullptr) return _base;
                v->classes = _arena.copy(classes, &ok);
                if (!ok) return _base;
                v->theme = _base;
                // Each class in the order written, its lines in theirs.
                bool any = false;
                forEachWord(classes, [&](StringView c) {
                        for (Line *l = _classLines; l != nullptr; l = l->next) {
                                if (!l->className.equals(c)) continue;
                                const char *why = "";
                                (void)apply(v->theme, l->section, l->key, l->value, &why);
                                any = true;
                        }
                });
                if (!any) return _base;
                v->next = _variants;
                _variants = v;
                return v->theme;
        }

} // namespace preso
