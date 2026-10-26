/**
 * @file      patterneditors.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/list.h>
#include <bmds/patternpalettes.h>
#include <bmds/string.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class ColorSwatch;
class ComboBox;
class ScrollView;
class SettingsWindow;

/**
 * @brief The desktop's patterns as a grid (`devplan/wallpaper.md` §4f): each found on
 *        `panel/desktop/patterns` drawn as a small tile in its own colors (the current one in
 *        the colors it has now), its name under it, the current one framed. A click, or Enter
 *        or Space on the one with the keys, chooses it (`choose()`).
 *
 * @par Thread Safety
 * The application's loop, as the widgets.
 */
class PatternChooser final : public Widget {
        public:
                static constexpr uint16_t SwatchWidth = 64;
                static constexpr uint16_t SwatchHeight = 40;
                static constexpr uint16_t CellWidth = 84;
                static constexpr uint16_t CellHeight = 66;

                /** @brief A grid in @p parent, @p form (laid out again when its rows change). */
                PatternChooser(Widget *parent, ScrollView *form, SettingsWindow &window);
                ~PatternChooser() override;

                /** @brief Reads the settings again: the patterns listed again if their path changed. */
                void reload();

                GuiSize  sizeHint() const override;
                uint16_t heightForWidth(uint16_t width) const override;
                void     paint(Painter &painter) override;
                void     mousePressed(const WindowManager::MouseEvent &event) override;
                bool     keyPressed(const KeyEvent &event) override;
                void     focusIn() override { update(); }
                void     focusOut() override { update(); }

        private:
                struct Entry {
                                char     name[65] = {};
                                uint8_t *pixels = nullptr; // B, G, R: the swatch
                };

                void     clearEntries();
                void     list();                     // the patterns on the path, sorted, no swatches yet
                void     makeSwatch(Entry &e) const; // drawn in its colors (the current one's own)
                uint16_t columns(uint16_t width) const;
                int      indexOf(StringView name) const;
                void     choose(size_t index);

                static void found(void *arg, StringView name, StringView path);

                ScrollView     *_form;
                SettingsWindow &_window;
                List<Entry>     _entries{MemSpace::Psram};
                String          _paths{MemSpace::Psram};
                String          _current{MemSpace::Psram};
                String          _colors{MemSpace::Psram}; // the current one's, as its swatch has them
                uint32_t        _desktop = 0;             // the color transparent pixels are over
                int             _focus = 0;               // the one with the keys
};

/**
 * @brief The current pattern's colors and palettes (`devplan/wallpaper.md` §4c, §4f): a swatch
 *        for each of its palette's entries (a click asks for that color, shown as it changes);
 *        its palettes to choose from (Original, the built-in ones, the saved ones); Save As...
 *        and Delete (a saved one). What is chosen is set, saved and kept as the pattern's
 *        `Current` in the saved palettes' file.
 *
 * @par Thread Safety
 * The application's loop, as the widgets.
 */
class PatternColors final : public BoxLayout {
        public:
                static constexpr size_t MaxShown = 16; ///< The palette's entries offered.

                PatternColors(Widget *parent, SettingsWindow &window);
                ~PatternColors() override;

                /** @brief Builds its widgets. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief Reads the pattern, its colors and palettes again. */
                void reload();

        private:
                struct Palette {
                                char name[PatternPalettes::MaxName + 1] = {};
                                char colors[256] = {};
                                bool saved = false; // the user's (Delete applies)
                };

                static void swatchClicked(void *arg, ColorSwatch &swatch);
                static void colorTaken(void *arg, uint32_t rgb, bool finished, bool accepted);
                static void paletteChosen(void *arg, ComboBox &box, int index);
                static void buttonClicked(void *arg, Button &button);
                static void nameTaken(void *arg, StringView name);

                void apply(StringView colors, bool save); // set (and saved), kept as Current
                void colorsWith(size_t entry, uint32_t rgb, String &out) const;

                SettingsWindow &_window;
                BoxLayout      *_swatchRow = nullptr;
                ColorSwatch    *_swatches[MaxShown] = {};
                Label          *_fixed = nullptr; // a pattern without a palette says so
                ComboBox       *_choice = nullptr;
                Button         *_saveAs = nullptr;
                Button         *_delete = nullptr;
                List<Palette>   _palettes{MemSpace::Psram}; // the combo's, by index
                uint32_t        _colors[MaxShown] = {};     // shown now
                size_t          _entries = 0;               // the palette's (0: not recolorable)
                String          _pattern{MemSpace::Psram};
                String          _before{MemSpace::Psram}; // the colors before a color was asked for
                String          _chosen{MemSpace::Psram}; // the palette last chosen or saved, shown if it matches
                size_t          _asking = 0;              // the entry whose color is asked for
};

/** @brief The desktop's settings changed together (`devplan/wallpaper.md` §4c). */
namespace PatternSettings {

        /**
         * @brief Chooses pattern @p name: the current one's colors kept as its `Current`, @p name's
         *        `Current` (else its own colors) set with it, both saved; said in @p window.
         */
        void choose(SettingsWindow &window, StringView name);

        /** @brief Keeps @p colors as @p pattern's `Current` in the saved palettes' file. @return Why not. */
        Error storeCurrent(StringView pattern, StringView colors);

        /** @brief Opens the first folder of `panel/desktop/patterns` (made if missing) in Files. */
        void openFolder(SettingsWindow &window);

} // namespace PatternSettings

BMDS_NAMESPACE_END
