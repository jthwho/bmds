/**
 * @file      theme.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A deck's look (devplan/preso.md §5): colours, fonts, spacing, decorations, effects. A theme is
 * an INI file; the built-in ones (paper, vga, amber, green, classic, bbs) are INI text in the
 * program, read the same way, so a theme file can do all they do. `[Class NAME]` sections change
 * the theme for slides of that class.
 */

#ifndef PRESO_THEME_H
#define PRESO_THEME_H

#include "arena.h"
#include "color.h"
#include "types.h"

namespace preso {

        class Deck;
        class Platform;

        /** @brief A colour that isn't there (no background, no frame): not a 0xRRGGBB. */
        constexpr Color NoColor = 0xFF000000u;

        /** @brief A font: a family, a pixel size and a style. Families live as long as the theme. */
        struct FontKey {
                        static constexpr uint8_t Bold = 0x1;
                        static constexpr uint8_t Italic = 0x2;

                        const char *family = "IBM Plex Sans";
                        uint16_t    size = 30;
                        uint8_t     style = 0;

                        bool operator==(const FontKey &other) const {
                                return size == other.size && style == other.style && strcmp(family, other.family) == 0;
                        }
        };

        struct TextStyle {
                        FontKey font;
                        Color   color = 0;
        };

        /** @brief A box's outline. */
        enum class Frame : uint8_t {
                None,
                Single, ///< A line.
                Double, ///< Two lines, as box drawing's ═.
                Raised, ///< The toolkit's 3D bevel, light above, dark below.
                Sunken, ///< The other way round.
        };

        /** @brief What a heading has under it. */
        enum class Underline : uint8_t {
                None,
                Line,
                Double,
                Shade, ///< Block shades fading out, █▓▒░ (a BBS's divider).
        };

        struct HeadingStyle {
                        TextStyle text;
                        Color     background = NoColor; ///< A band behind it.
                        bool      bar = false;          ///< The band across the whole slide (a title bar).
                        Frame     frame = Frame::None;
                        Color     frameColor = NoColor; ///< NoColor: its text's.
                        Underline underline = Underline::None;
                        Color     underlineColor = NoColor; ///< NoColor: the accent.
        };

        struct Theme {
                        const char *name = "paper";
                        int32_t     width = 1280; ///< The slide's size (16:9; the deck's `size` may change it).
                        int32_t     height = 720;
                        bool        nearest = false; ///< `Scale=nearest`: shown in whole multiples, sharp pixels.
                        Color       background = 0xFFFFFF;
                        const char *pattern =
                                nullptr; ///< A picture tiled over the background (from the deck's folder).
                        Frame slideFrame = Frame::None; ///< Around the whole slide (a window, `classic`).
                        Color accent = 0x1A5FB4;
                        // The content's box: the slide less these.
                        int32_t marginTop = 56;
                        int32_t marginRight = 80;
                        int32_t marginBottom = 64;
                        int32_t marginLeft = 80;
                        bool    middle = false; ///< The content in the slide's middle, top to bottom.
                        bool    center = false; ///< Its lines centred.

                        TextStyle    body;
                        HeadingStyle heading[3]; ///< H1, H2, H3 (H4-H6 as H3).
                        TextStyle    code;
                        TextStyle    header;                     ///< The header, footer and page number.
                        Color        headerBackground = NoColor; ///< A bar behind the header and footer.

                        Color link = 0x1A5FB4;
                        Color codeBackground = 0xF4F4F0;
                        Color inlineCodeBackground = 0xEDEDE8;
                        Frame codeFrame = Frame::None;
                        Color quoteBar = 0xC0C0C0;
                        Color quoteColor = 0x555555;
                        Color rule = 0xD0D0D0;
                        Color tableHeaderBackground = 0xF0F0EC;
                        Color placeholder = 0xB0B0B0; ///< A box drawn where a picture isn't.
                        Color syntax[16] = {};        ///< A colour per SyntaxStyle; 0 for Normal: the code's own.
                        /** @brief Pictures are mapped to it by ordered dithering (none: their own colours). */
                        Palette palette;

                        /** @brief By depth: • – › (IBM Plex has no ◦ or ▪). */
                        const char *bullets[3] = {"\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x80\xBA"};
                        Color       bulletColor = NoColor; ///< NoColor: the accent.
                        int32_t     listIndent = 44;
                        int32_t     quoteIndent = 28;
                        int32_t     codePadding = 18;
                        /** @brief Space between blocks, in body lines (as sixteenths). */
                        int32_t blockGap16 = 8;

                        /**
                         * @brief Colours scripts name (`theme.color("Script.Highlight")`), from the
                         *        theme's `[Script]` section; Script.Accent, Highlight, Muted, Line and
                         *        Fill have defaults (scriptColor()).
                         */
                        static constexpr uint32_t MaxScriptColors = 16;
                        struct ScriptColor {
                                        StringView name;
                                        Color      color = 0;
                        };
                        ScriptColor scriptColors[MaxScriptColors];
                        uint32_t    scriptColorCount = 0;

                        // ---- Effects, drawn over the finished slide.
                        uint8_t scanlines = 0; ///< Every other row darkened by this (of 255).

                        /** @brief Paper as built in, without its class rules (the start of every theme). */
                        static const Theme &paper();

                        /**
                         * @brief A colour scripts name: `Script.NAME` (the `[Script]` section's, or a
                         *        default), or the theme's own (`Accent`, `Text`, `H1`...). False if there
                         *        is none of that name.
                         */
                        bool scriptColor(StringView name, Color *color) const;
        };

        /**
         * @brief A deck's theme: the one it names, read from a file or built in, and the variants
         *        its slides' classes make of it.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class ThemeSet {
                public:
                        ThemeSet() = default;
                        ThemeSet(const ThemeSet &) = delete;
                        ThemeSet &operator=(const ThemeSet &) = delete;

                        /**
                         * @brief Reads theme @p name (empty: paper) for the deck at @p deckPath: a
                         *        path (`themes/mine.theme`, from the deck's folder), else `NAME.theme`
                         *        beside the deck, then in @p dataFolder's `themes` (nullptr: none),
                         *        then the built-in one. What is wrong in it is reported into @p deck;
                         *        an unknown theme is paper, reported.
                         * @return Error::NoMemory, or Ok.
                         */
                        [[nodiscard]] Error load(Platform &platform, StringView name, const char *deckPath,
                                                 const char *dataFolder, Deck &deck);

                        /** @brief Reads @p text (a theme file's, @p file for messages) over paper. */
                        [[nodiscard]] Error loadText(StringView text, const char *file, Deck *deck);

                        const Theme &base() const { return _base; }
                        /** @brief The theme for a slide of classes @p classes (`title invert`): base if none apply. */
                        const Theme &forClasses(StringView classes);
                        /** @brief The folder the theme's files (a pattern) are named from. */
                        const char *folder() const { return _folder; }

                        /** @brief The built-in themes' names, nullptr-terminated. */
                        static const char *const *builtInNames();
                        /** @brief A built-in theme's INI text (`preso theme NAME`: a start for one's own), or nullptr. */
                        static const char *builtInText(StringView name);

                private:
                        struct Line;
                        struct Variant;

                        Error read(Platform *platform, StringView name, StringView text, const char *file, Deck *deck,
                                   uint32_t depth);
                        Error resolve(Platform &platform, StringView name, const char *deckPath, const char *dataFolder,
                                      Deck &deck, uint32_t depth);
                        bool  apply(Theme &theme, StringView section, StringView key, StringView value,
                                    const char **why);

                        Arena    _arena;
                        Theme    _base;
                        Line    *_classLines = nullptr; ///< `[Class NAME]` lines, in order.
                        Line   **_classTail = &_classLines;
                        Variant *_variants = nullptr;
                        char     _folder[512] = ".";
                        char     _relative[512] = ""; ///< The theme file's folder from the deck's ("": the same).
        };

        /**
         * @brief Reads a font as themes write it: a family, then optionally a pixel size and
         *        `bold`, `italic` or `regular` (`unscii 32`, `IBM Plex Sans 30 bold`). Without a
         *        size or family, @p font's are kept. The family is copied into @p arena.
         */
        bool parseFont(StringView text, Arena &arena, FontKey *font);

        /** @brief A built-in palette by name (`ega`, `cga`, `amber`, `green`, `gray`, `mono`), or false. */
        bool builtInPalette(StringView name, Palette *palette);

} // namespace preso

#endif // PRESO_THEME_H
