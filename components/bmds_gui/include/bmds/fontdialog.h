/**
 * @file      fontdialog.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/dialog.h>
#include <bmds/fontname.h>
#include <bmds/itemmodel.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class DetailsView;
class ItemView;
class LineEdit;
class NumberField;

/**
 * @brief Chooses a font (`devplan/fonts.md` §4i): its family (each name drawn in its own face,
 *        a search box over them), its style (the family's faces, and a made-up bold where it
 *        has none), its size (a list and a field; a bitmap face its multiples), and a preview
 *        in it.
 *
 * Each change is told to `Changed` at once (a live preview), and OK or Cancel to `Finished`
 * (`Accepted`, `Rejected`).
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class FontDialog : public Dialog {
        public:
                /** @brief Told each font the user chooses: its name (on the loop's thread). */
                using Changed = void (*)(void *arg, FontDialog &dialog, StringView name);

                /** @brief `Finished`'s results. */
                enum Choice : int {
                        Rejected = 0,
                        Accepted = 1,
                };

                /** @brief The families offered. */
                enum class Filter : uint8_t {
                        All,       ///< Every family.
                        Monospace, ///< Fixed-width ones (a terminal's, an editor's).
                        Scalable,  ///< TrueType ones (no bitmap font).
                };

                /** @brief The most families listed. */
                static constexpr size_t MaxFamilies = 64;

                FontDialog(GuiApplication &app, StringView title);
                ~FontDialog() override;

                /**
                 * @brief Opens it on @p name (a name or a role, which shows as the font it is).
                 * @return `Ok` or the window manager's error.
                 */
                Error open(StringView name);

                /** @brief Returns the font chosen, as a name ("IBM Plex Sans 13 bold"). */
                StringView fontName() const { return StringView(_name); }

                /** @brief Sets the families offered (all by default); before `open()`. */
                void setFilter(Filter filter) { _filter = filter; }

                /** @brief Sets the preview's text (cut at `Label::MaxText` bytes). */
                void setSample(StringView text);

                /** @brief Has @p fn told of each font chosen. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

        private:
                // A family's row: its name, drawn in itself.
                class Families final : public ListItemModel {
                        public:
                                Font rowFont(size_t row) const override {
                                        return row < MaxFamilies ? fonts[row] : Font();
                                }
                                Font fonts[MaxFamilies];
                };

                // A style's row: what it asks for.
                struct Style {
                                uint8_t flags = 0; // FontName::Bold, FontName::Italic
                };

                static void searchEdited(void *arg, LineEdit &edit);
                static void familyChanged(void *arg, ItemView &view);
                static void styleRowChanged(void *arg, ItemView &view);
                static void sizeListChanged(void *arg, ItemView &view);
                static void sizeFieldChanged(void *arg, NumberField &field, int32_t value);

                Error build();
                void  listFamilies();    // the search and the filter applied
                void  listStyles();      // the chosen family's
                void  listSizes();       // a bitmap face's multiples, or the usual sizes
                void  chosen(bool tell); // the name made, the preview shown, told
                bool  isBitmapFamily() const;

                LineEdit     *_search = nullptr;
                DetailsView  *_familyList = nullptr;
                DetailsView  *_styleList = nullptr;
                DetailsView  *_sizeList = nullptr;
                NumberField  *_size = nullptr;
                Label        *_preview = nullptr;
                Families      _families;
                ListItemModel _styles;
                ListItemModel _sizes;
                Style         _styleOf[8];
                uint16_t      _sizeOf[24] = {};
                char          _family[FontName::MaxFamily + 1] = {};
                uint8_t       _style = 0;
                uint16_t      _pixels = 13;
                char          _name[FontName::MaxText + 1] = {};
                Filter        _filter = Filter::All;
                bool          _filling = false; // the lists being refilled: their changes aren't the user's
                Changed       _changed = nullptr;
                void         *_changedArg = nullptr;
};

/**
 * @brief A font's name drawn in that font (`devplan/fonts.md` §4i), clicked to change it: it
 *        opens a `FontDialog`, and a font chosen there becomes its own and is told to
 *        `Changed`. The Settings app's font rows.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class FontButton : public Button {
        public:
                /** @brief Told the font the user chose (on the loop's thread). */
                using Changed = void (*)(void *arg, FontButton &button, StringView name);

                explicit FontButton(Widget *parent, StringView name = StringView());
                ~FontButton() override;

                /**
                 * @brief Shows @p name (a name or a role), drawn in it.
                 * @return `Ok`, `Invalid` (not a font name), `NoMem`.
                 */
                Error setFontName(StringView name);

                /** @brief Returns the font shown, as given ("" for none: the ui role's). */
                StringView fontName() const { return fontText(); }

                /** @brief Sets the families its dialog offers. */
                void setFilter(FontDialog::Filter filter) { _filter = filter; }

                /** @brief Returns the families its dialog offers. */
                FontDialog::Filter filter() const { return _filter; }

                /** @brief Has @p fn told of each font chosen. */
                void setChanged(Changed fn, void *arg) {
                        _changedFn = fn;
                        _changedArg = arg;
                        return;
                }

        private:
                static void clicked(void *arg, Button &button);
                static void finished(void *arg, Dialog &dialog, int result);
                void        dropDialog();

                FontDialog        *_dialog = nullptr;
                FontDialog::Filter _filter = FontDialog::Filter::All;
                Changed            _changedFn = nullptr;
                void              *_changedArg = nullptr;
};

BMDS_NAMESPACE_END
