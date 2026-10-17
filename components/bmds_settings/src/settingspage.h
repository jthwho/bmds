/**
 * @file      settingspage.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/configtypes.h>
#include <bmds/itemmodel.h>
#include <bmds/list.h>
#include <bmds/sysconf.h>
#include <bmds/widget.h>

#include "settingstable.h"
#include "themes.h"

BMDS_NAMESPACE_BEGIN

class CheckBox;
class ColorSwatch;
class ComboBox;
class DetailsView;
class FormLayout;
class FormRow;
class FormSection;
class ItemView;
class LineEdit;
class ListEditor;
class NumberField;
class Slider;
class SettingsWindow;
class TextBlock;

/**
 * @brief The editors of a page's settings (`devplan/controlpanel.md` §6): each
 *        made from its key's type and its row, showing the current value, and
 *        applying and saving what the user chooses.
 *
 * @par Thread Safety
 * The application's loop, as the widgets.
 */
class EditorSet {
        public:
                /** @brief What a setting is edited with. */
                enum class Kind : uint8_t {
                        Check,
                        Choice,
                        Number,
                        Slider,
                        Text,
                        List,
                        Color,
                        Secret,
                        Mode, // a choice of modes, and Custom's field
                };

                /** @brief One setting's editor. */
                struct Binding {
                                EditorSet                *set = nullptr;
                                const SettingsTable::Row *row = nullptr;
                                FormRow                  *formRow = nullptr;
                                Widget                   *editor = nullptr; // the kind's widget
                                Label                    *aside = nullptr;  // a slider's value, a secret's state
                                Button                   *save = nullptr;   // shown while changed and not saved
                                Button                   *revert = nullptr;
                                Button                   *extra = nullptr;   // Choose..., a secret's Remove
                                LineEdit                 *custom = nullptr;  // a mode's Custom field
                                FormSection              *section = nullptr; // the heading over it
                                char                      star[16] = {};     // what its row's `*` stood for
                                SysConf::Id               id = SysConf::NoId;
                                Config::Type              type = Config::Type::String;
                                Kind                      kind = Kind::Text;
                                char                      key[Config::MaxKey + 1] = {};
                };

                explicit EditorSet(SettingsWindow &window);

                /** @brief Makes room for @p count editors (before `add()`: they are never moved). */
                Error reserve(size_t count);

                /**
                 * @brief Adds the editor of @p id to @p form, labelled @p label, placed by
                 *        @p row. @return Its binding, or null (no memory, no such key).
                 */
                Binding *add(FormLayout &form, SysConf::Id id, const SettingsTable::Row &row, StringView label);

                /** @brief Forgets the editors (their widgets are the form's). */
                void clear() { _bindings.clear(); }

                /** @brief Returns @p id's editor, or null. */
                Binding *find(SysConf::Id id);

                /** @brief Returns the editors. */
                List<Binding> &bindings() { return _bindings; }

                /** @brief Shows @p b's current value (not over text being typed). */
                void load(Binding &b);

                /** @brief Shows every editor's value. */
                void loadAll();

                /** @brief Gives @p b's editor the keys. */
                void focus(Binding &b);

                /** @brief Returns the window. */
                SettingsWindow &window() const { return _window; }

        private:
                enum class Apply : uint8_t {
                        AndSave, // set, then saved (or confirmed first)
                        Only,    // set, not saved (a slider dragged)
                };

                static void toggled(void *arg, CheckBox &box);
                static void chosen(void *arg, ComboBox &box, int index);
                static void numberChanged(void *arg, NumberField &field, int32_t value);
                static void slid(void *arg, Slider &slider, int32_t value);
                static void slidDone(void *arg, Slider &slider, int32_t value);
                static void textTaken(void *arg, LineEdit &edit);
                static void textCancelled(void *arg, LineEdit &edit);
                static void listChanged(void *arg, ListEditor &editor);
                static void listAdd(void *arg, ListEditor &editor);
                static void swatchClicked(void *arg, ColorSwatch &swatch);
                static void modeChosen(void *arg, ComboBox &box, int index);
                static void buttonClicked(void *arg, Button &button);

                Binding *bindingOf(const Widget *editor); // whose editor (or a part of it) this is
                void     commit(Binding &b, StringView value, Apply apply);
                void     showSlider(Binding &b, int32_t value);
                void     marks(Binding &b); // Save and Revert shown while changed and not saved

                SettingsWindow &_window;
                List<Binding>   _bindings{MemSpace::Psram};
};

/**
 * @brief A group's page: its title, then its sections and rows
 *        (`devplan/controlpanel.md` §4), from its table.
 *
 * @par Thread Safety
 * The application's loop, as the widgets.
 */
class SettingsPage final : public BoxLayout {
        public:
                SettingsPage(Widget *parent, SettingsWindow &window, size_t page);
                ~SettingsPage() override;

                /** @brief Makes the rows of the keys there are. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief Returns its page's index. */
                size_t page() const { return _page; }

                /**
                 * @brief @p id changed (`NoId`: anything may have).
                 * @return False if the page should be made again (a key it places appeared).
                 */
                bool changed(SysConf::Id id);

                /** @brief The second's tick: its status read again (if it changes by itself). */
                void tick();

                /**
                 * @brief Scrolls to @p key's row and lights its label (and gives its editor the keys
                 *        if @p focus). @return False if not shown.
                 */
                bool reveal(StringView key, bool focus);

        private:
                static void buttonClicked(void *arg, Button &button);
                static void themeChosen(void *arg, ComboBox &box, int index);

                void applyConditions(); // rows that don't apply hidden, and headings over none
                void showTheme();       // the theme the colors match, else Custom

                SettingsWindow    &_window;
                EditorSet          _editors;
                const Themes::Set *_themes = nullptr; // Appearance's or Terminal's
                ComboBox          *_themeBox = nullptr;
                Widget            *_preview = nullptr; // a PalettePreview or a TerminalPreview
                bool               _systemPreview = false;
                TextBlock         *_status = nullptr;
                Button            *_renew = nullptr;    // Network's Renew Lease
                Button            *_bindings = nullptr; // Keyboard & Gamepads' Edit Bindings...
                Label             *_litLabel = nullptr; // what a search lit
                CheckBox          *_litBox = nullptr;
                FormLayout        *_form = nullptr;
                size_t             _page;
};

/**
 * @brief Every key, in key order, its value and state, and the chosen one's
 *        editor, help, limits, default and saved value (`devplan/controlpanel.md` §6.5).
 *
 * @par Thread Safety
 * The application's loop, as the widgets.
 */
class AllSettingsPage final : public BoxLayout {
        public:
                AllSettingsPage(Widget *parent, SettingsWindow &window);
                ~AllSettingsPage() override;

                /** @brief Makes it. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief A key changed (`NoId`: any). */
                void changed(SysConf::Id id);

                /** @brief Chooses @p key (the list given the keys if @p focus). @return False if there is none. */
                bool reveal(StringView key, bool focus);

        private:
                static void rowChanged(void *arg, ItemView &view);
                static void clicked(void *arg, Button &button);

                void fill();   // the list from the registry, the choice kept
                void detail(); // the chosen key's editor and facts

                SettingsWindow   &_window;
                EditorSet         _editors;
                ListItemModel     _model;
                List<SysConf::Id> _ids{MemSpace::Psram}; // each row's
                DetailsView      *_view = nullptr;
                BoxLayout        *_detailBox = nullptr;
                FormLayout       *_detail = nullptr;
                TextBlock        *_facts = nullptr;
                Button           *_reset = nullptr;
                Button           *_revert = nullptr;
                SysConf::Id       _shown = SysConf::NoId;
};

BMDS_NAMESPACE_END
