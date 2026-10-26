/**
 * @file      settingspage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Settings' pages (devplan/controlpanel.md §6): editors made from each key's type and row,
 * applied and saved at once; All Settings.
 */

#include <cstdio>
#include <cstring>
#include <bmds/colordialog.h>
#include <bmds/combobox.h>
#include <bmds/detailsview.h>
#include <bmds/formlayout.h>
#include <bmds/formmodels.h>
#include <bmds/lineedit.h>
#include <bmds/listeditor.h>
#include <bmds/numberfield.h>
#include <bmds/slider.h>
#include <bmds/string.h>

#include <bmds/guiapplication.h>
#include <bmds/netstatus.h>

#include "pagestatus.h"
#include "patterneditors.h"
#include "previews.h"
#include "settingspage.h"
#include "settingswindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Row = SettingsTable::Row;
        using Kind = EditorSet::Kind;
        using Binding = EditorSet::Binding;

        constexpr Row PlainRow{}; // a key no page places (All Settings)

        // The display's modes offered (display/mode's forms), then Custom.
        struct ModeChoice {
                        const char *value;
                        const char *label;
        };
        constexpr ModeChoice Modes[] = {
                {"720", "1280 x 720 at 60 Hz"},           {"1080", "1920 x 1080 at 60 Hz"},
                {"1280x1024@60", "1280 x 1024 at 60 Hz"}, {"1024x768@60", "1024 x 768 at 60 Hz"},
                {"800x600@60", "800 x 600 at 60 Hz"},     {"640x480@60", "640 x 480 at 60 Hz"},
        };
        constexpr size_t CustomMode = sizeof(Modes) / sizeof(Modes[0]); // the combo's last item

        // The word @p index of an enum's @p choices (empty past them).
        StringView choiceWord(StringView choices, size_t index) {
                return ListText::item(choices, '|', index);
        }

        // The label shown for @p b: its check box's text, or its row's label.
        StringView labelOf(const Binding &b) {
                if (b.kind == Kind::Check && b.editor != nullptr) return static_cast<CheckBox *>(b.editor)->text();
                return b.formRow != nullptr && b.formRow->label() != nullptr ? b.formRow->label()->text()
                                                                             : StringView();
        }

        // Where a pattern's keys are listed from: its components before the first `*`.
        StringView prefixOf(StringView pattern) {
                const size_t star = pattern.indexOf('*');
                if (star == StringView::NPos) return pattern;
                const StringView before = pattern.left(star);
                return before.size() > 0 ? before.left(before.size() - 1) : StringView();
        }

        // Calls @p fn(id) for each key @p row places, in key order.
        template <typename Fn> void eachKey(const Row &row, Fn fn) {
                const StringView pattern(row.key);
                if (pattern.indexOf('*') == StringView::NPos) {
                        const Result<SysConf::Id> id = SysConf::find(pattern);
                        if (id.isOk()) fn(id.value());
                        return;
                }
                const StringView prefix = prefixOf(pattern);
                SysConf::Id      after = SysConf::NoId;
                for (;;) {
                        const Result<SysConf::Id> next = SysConf::next(prefix, after);
                        if (next.isError()) return;
                        after = next.value();
                        // Only where it is placed: another row may name it (net/mdns/enabled).
                        StringView       star;
                        const StringView key = SysConf::keyOf(after);
                        if (SettingsTable::matches(pattern, key, star) && SettingsTable::place(key).row == &row)
                                fn(after);
                }
        }

} // namespace

// ---- EditorSet

EditorSet::EditorSet(SettingsWindow &window) : _window(window) {}

Error EditorSet::reserve(size_t count) {
        return _bindings.reserve(_bindings.size() + count);
}

EditorSet::Binding *EditorSet::find(SysConf::Id id) {
        for (Binding &b : _bindings)
                if (b.id == id) return &b;
        return nullptr;
}

EditorSet::Binding *EditorSet::add(FormLayout &form, SysConf::Id id, const Row &row, StringView label) {
        SysConf::Info info;
        if (SysConf::info(id, info).isError() || info.param == nullptr) return nullptr;
        if (_bindings.size() == _bindings.capacity()) return nullptr; // reserve() first: never moved
        const Config::Param &param = *info.param;

        Binding b;
        b.set = this;
        b.row = &row;
        b.id = id;
        b.type = param.type;
        const StringView key = SysConf::keyOf(id);
        const size_t     n = key.size() < sizeof(b.key) - 1 ? key.size() : sizeof(b.key) - 1;
        std::memcpy(b.key, key.data(), n);
        if (b.type == Config::Type::Secret) {
                b.kind = Kind::Secret;
        } else if (row.editor == SettingsTable::Editor::Mode && b.type == Config::Type::String) {
                b.kind = Kind::Mode;
        } else if (row.editor == SettingsTable::Editor::Pattern && b.type == Config::Type::String) {
                b.kind = Kind::Pattern;
        } else if (row.editor == SettingsTable::Editor::PatternColors && b.type == Config::Type::String) {
                b.kind = Kind::PatternColors;
        } else if (row.editor == SettingsTable::Editor::Slider && b.type == Config::Type::Int) {
                b.kind = Kind::Slider;
        } else if (b.type == Config::Type::Bool) {
                b.kind = Kind::Check;
        } else if (b.type == Config::Type::Enum) {
                b.kind = Kind::Choice;
        } else if (b.type == Config::Type::Int) {
                b.kind = Kind::Number;
        } else if (b.type == Config::Type::Color) {
                b.kind = Kind::Color;
        } else if (b.type == Config::Type::Paths || row.editor == SettingsTable::Editor::List) {
                b.kind = Kind::List;
        } else {
                b.kind = Kind::Text;
        }

        // The help: the row's, else the key's, and when it acts if not at once.
        char help[TextBlock::MaxText + 1];
        std::snprintf(help, sizeof(help), "%s%s", row.help != nullptr ? row.help : param.help,
                      (row.flags & SettingsTable::AtStart) != 0    ? " (from the next display start)"
                      : (row.flags & SettingsTable::AtBoot) != 0   ? " (from the next boot)"
                      : (row.flags & SettingsTable::NotSaved) != 0 ? " (until a restart, unless saved)"
                                                                   : "");
        FormRow *fr = form.addRow(b.kind == Kind::Check ? StringView() : label, StringView(help));
        if (fr == nullptr) return nullptr;
        b.formRow = fr;
        BoxLayout *field = fr->field();

        switch (b.kind) {
                case Kind::Check: b.editor = Widget::create<CheckBox>(field, label); break;
                case Kind::Choice: {
                        auto *box = Widget::create<ComboBox>(field);
                        b.editor = box;
                        if (box == nullptr) break;
                        const StringView choices(param.choices != nullptr ? param.choices : "");
                        for (size_t i = 0; i < ListText::count(choices, '|'); ++i) {
                                char         text[ComboBox::MaxText + 1];
                                const size_t len = SettingsTable::choiceLabel(row.choices, choiceWord(choices, i),
                                                                              Span<char>(text, sizeof(text)));
                                (void)box->addItem(StringView(text, len), i);
                        }
                        break;
                }
                case Kind::Number: {
                        auto *number = Widget::create<NumberField>(field);
                        b.editor = number;
                        if (number == nullptr) break;
                        number->setRange(param.min, param.max);
                        const int64_t span = int64_t(param.max) - param.min;
                        number->setSteps(1,
                                         uint32_t(span / 10 > 1 ? (span / 10 < INT32_MAX ? span / 10 : INT32_MAX) : 1));
                        if (row.unit != nullptr) (void)Widget::create<Label>(field, row.unit);
                        break;
                }
                case Kind::Slider: {
                        auto *slider = Widget::create<Slider>(field, Slider::Orientation::Horizontal);
                        b.editor = slider;
                        b.aside = Widget::create<Label>(field);
                        if (slider == nullptr) break;
                        slider->setRange(param.min, param.max);
                        const int32_t span = param.max - param.min;
                        slider->setSteps(1, span / 10 > 1 ? span / 10 : 1);
                        slider->setStretch(1);
                        if (b.aside != nullptr) b.aside->setMinimumWidth(80);
                        break;
                }
                case Kind::Text: {
                        auto *edit = Widget::create<LineEdit>(field);
                        b.editor = edit;
                        if (edit == nullptr) break;
                        edit->setStretch(1);
                        edit->setMaxLength(param.max > 0 ? size_t(param.max) : Config::MaxValue);
                        if (row.editor == SettingsTable::Editor::Folder || row.editor == SettingsTable::Editor::File ||
                            row.editor == SettingsTable::Editor::Zone || b.type == Config::Type::Path)
                                b.extra = Widget::create<Button>(field, "Choose...");
                        break;
                }
                case Kind::List: {
                        const char separator = b.type == Config::Type::Paths ? ';' : row.separator;
                        auto      *list = Widget::create<ListEditor>(field, separator);
                        b.editor = list;
                        if (list != nullptr) list->setStretch(1);
                        break;
                }
                case Kind::Color: b.editor = Widget::create<ColorSwatch>(field); break;
                case Kind::Secret: {
                        // The password's: Set Password... asks for it; another secret can only be removed.
                        if (row.editor == SettingsTable::Editor::Password)
                                b.editor = Widget::create<Button>(field, "Set Password...");
                        b.aside = Widget::create<Label>(field);
                        b.extra = Widget::create<Button>(field, "Remove");
                        if (b.editor == nullptr && row.editor != SettingsTable::Editor::Password) b.editor = b.aside;
                        break;
                }
                case Kind::Mode: {
                        auto *box = Widget::create<ComboBox>(field);
                        b.editor = box;
                        b.custom = Widget::create<LineEdit>(field);
                        if (box == nullptr || b.custom == nullptr) {
                                b.editor = nullptr;
                                break;
                        }
                        for (size_t i = 0; i < CustomMode; ++i) (void)box->addItem(Modes[i].label, i);
                        (void)box->addItem("Custom", CustomMode);
                        b.custom->setStretch(1);
                        b.custom->setMaxLength(param.max > 0 ? size_t(param.max) : Config::MaxValue);
                        b.custom->setVisible(false);
                        break;
                }
                case Kind::Pattern: {
                        // The current one's name and the folder in the row; the grid under it, the form's.
                        b.aside = Widget::create<Label>(field);
                        b.extra = Widget::create<Button>(field, "Open Folder");
                        b.editor = Widget::create<PatternChooser>(&form, &form, _window);
                        b.below = b.editor;
                        if (b.aside != nullptr) b.aside->setStretch(1);
                        if (b.aside == nullptr || b.extra == nullptr) b.editor = nullptr;
                        break;
                }
                case Kind::PatternColors: {
                        auto *colors = Widget::create<PatternColors>(field, _window);
                        if (colors != nullptr && colors->build().isError()) {
                                colors->destroy();
                                colors = nullptr;
                        }
                        if (colors != nullptr) colors->setStretch(1);
                        b.editor = colors;
                        break;
                }
        }
        b.save = Widget::create<Button>(field, "Save");
        b.revert = Widget::create<Button>(field, "Revert");
        if (b.editor == nullptr || b.save == nullptr || b.revert == nullptr || _bindings.append(b).isError()) {
                if (b.below != nullptr) b.below->destroy();
                fr->destroy();
                return nullptr;
        }
        Binding &bound = _bindings.back();

        // Told with the binding: it is never moved (reserved).
        for (Button *button : {bound.save, bound.revert, bound.extra}) {
                if (button == nullptr) continue;
                button->setClicked(buttonClicked, &bound);
        }
        bound.save->setVisible(false);
        bound.revert->setVisible(false);
        switch (bound.kind) {
                case Kind::Check: static_cast<CheckBox *>(bound.editor)->setToggled(toggled, &bound); break;
                case Kind::Choice: static_cast<ComboBox *>(bound.editor)->setChanged(chosen, &bound); break;
                case Kind::Number: static_cast<NumberField *>(bound.editor)->setChanged(numberChanged, &bound); break;
                case Kind::Slider: {
                        auto *slider = static_cast<Slider *>(bound.editor);
                        slider->setChanged(slid, &bound);
                        slider->setReleased(slidDone, &bound);
                        break;
                }
                case Kind::Text: {
                        auto *edit = static_cast<LineEdit *>(bound.editor);
                        edit->setAccepted(textTaken, &bound);
                        edit->setFocusLost(textTaken, &bound);
                        edit->setCancelled(textCancelled, &bound);
                        break;
                }
                case Kind::List: {
                        auto *list = static_cast<ListEditor *>(bound.editor);
                        list->setChanged(listChanged, &bound);
                        if (bound.type == Config::Type::Paths) list->setAddAsked(listAdd, &bound);
                        break;
                }
                case Kind::Color: static_cast<ColorSwatch *>(bound.editor)->setClicked(swatchClicked, &bound); break;
                case Kind::Secret:
                        if (bound.editor != bound.aside)
                                static_cast<Button *>(bound.editor)->setClicked(buttonClicked, &bound);
                        break;
                case Kind::Mode:
                        static_cast<ComboBox *>(bound.editor)->setChanged(modeChosen, &bound);
                        bound.custom->setAccepted(textTaken, &bound);
                        bound.custom->setFocusLost(textTaken, &bound);
                        bound.custom->setCancelled(textCancelled, &bound);
                        break;
                case Kind::Pattern:
                case Kind::PatternColors: break; // they set the settings themselves
        }
        // A label's mnemonic gives the keys to its editor.
        Widget *buddy = bound.editor;
        if (bound.kind == Kind::Number) buddy = static_cast<NumberField *>(bound.editor)->edit();
        if (bound.kind == Kind::List) buddy = static_cast<ListEditor *>(bound.editor)->view();
        if (fr->label() != nullptr && bound.kind != Kind::Check && bound.kind != Kind::Secret)
                fr->label()->setBuddy(buddy);
        load(bound);
        return &bound;
}

void EditorSet::showSlider(Binding &b, int32_t value) {
        if (b.aside == nullptr) return;
        char text[32];
        std::snprintf(text, sizeof(text), "%ld %s", long(value), b.row->unit != nullptr ? b.row->unit : "");
        b.aside->setText(StringView(text));
        return;
}

void EditorSet::marks(Binding &b) {
        SysConf::Info info;
        const bool    changed = SysConf::info(b.id, info).isOk() && info.isChanged();
        if (b.save->isVisible() == changed) return;
        b.save->setVisible(changed);
        b.revert->setVisible(changed);
        b.formRow->field()->relayout();
        return;
}

void EditorSet::load(Binding &b) {
        String value(MemSpace::Psram);
        if (SysConf::get(b.id, value).isError()) return;
        const StringView v = value.view();
        switch (b.kind) {
                case Kind::Check: static_cast<CheckBox *>(b.editor)->setChecked(Config::toBool(v)); break;
                case Kind::Choice: {
                        SysConf::Info info;
                        if (SysConf::info(b.id, info).isError() || info.param == nullptr) break;
                        const StringView choices(info.param->choices != nullptr ? info.param->choices : "");
                        for (size_t i = 0; i < ListText::count(choices, '|'); ++i)
                                if (choiceWord(choices, i) == v) static_cast<ComboBox *>(b.editor)->setCurrent(int(i));
                        break;
                }
                case Kind::Number: {
                        auto *number = static_cast<NumberField *>(b.editor);
                        if (number->edit() == nullptr || !number->edit()->hasFocus())
                                number->setValue(Config::toInt(v));
                        break;
                }
                case Kind::Slider: {
                        auto *slider = static_cast<Slider *>(b.editor);
                        if (!slider->isDragging()) {
                                slider->setValue(Config::toInt(v));
                                showSlider(b, Config::toInt(v));
                        }
                        break;
                }
                case Kind::Text: {
                        auto *edit = static_cast<LineEdit *>(b.editor);
                        if (!edit->hasFocus() && edit->text() != v) edit->setText(v);
                        break;
                }
                case Kind::List: {
                        auto *list = static_cast<ListEditor *>(b.editor);
                        if (list->text() != v) (void)list->setText(v);
                        break;
                }
                case Kind::Color: static_cast<ColorSwatch *>(b.editor)->setColor(Config::toColor(v)); break;
                case Kind::Secret: {
                        const bool set = !v.isEmpty();
                        b.aside->setText(set ? "Set" : "None");
                        if (b.extra != nullptr) b.extra->setEnabled(set);
                        break;
                }
                case Kind::Mode: {
                        // A mode offered, else Custom with its text.
                        size_t at = CustomMode;
                        for (size_t i = 0; i < CustomMode; ++i)
                                if (v == StringView(Modes[i].value)) at = i;
                        static_cast<ComboBox *>(b.editor)->setCurrent(int(at));
                        const bool custom = at == CustomMode;
                        if (!b.custom->hasFocus() && b.custom->text() != v)
                                b.custom->setText(custom ? v : StringView());
                        if (b.custom->isVisible() != custom) {
                                b.custom->setVisible(custom);
                                b.formRow->field()->relayout();
                        }
                        break;
                }
                case Kind::Pattern: {
                        static_cast<PatternChooser *>(b.editor)->reload();
                        char         label[64];
                        const size_t n = SettingsTable::labelOf(v, Span<char>(label, sizeof(label)));
                        b.aside->setText(StringView(label, n));
                        break;
                }
                case Kind::PatternColors: static_cast<PatternColors *>(b.editor)->reload(); break;
        }
        marks(b);
        return;
}

void EditorSet::loadAll() {
        for (Binding &b : _bindings) load(b);
        return;
}

void EditorSet::focus(Binding &b) {
        Widget *target = b.editor;
        if (b.kind == Kind::Number) target = static_cast<NumberField *>(b.editor)->edit();
        if (b.kind == Kind::List) target = static_cast<ListEditor *>(b.editor)->view();
        if (b.kind == Kind::Secret) target = b.editor != b.aside ? b.editor : b.extra;
        if (target != nullptr && target->isFocusable()) target->setFocus();
        return;
}

void EditorSet::commit(Binding &b, StringView value, Apply apply) {
        String before(MemSpace::Psram);
        (void)SysConf::get(b.id, before);
        char             labelText[64];
        const StringView l = labelOf(b);
        std::snprintf(labelText, sizeof(labelText), "%.*s", int(l.size()), l.data());
        const StringView label(labelText);
        if (before.view() == value) {
                if (b.kind == Kind::Text) load(b);
                return;
        }
        const Error err = SysConf::set(b.id, value);
        if (err.isError()) {
                _window.refused(label, b.id, err);
                if (b.kind == Kind::Text) {
                        static_cast<LineEdit *>(b.editor)->selectAll(); // kept, to be corrected
                } else if (b.kind == Kind::Mode) {
                        b.custom->selectAll();
                } else {
                        load(b);
                }
                return;
        }
        if (apply == Apply::Only) return;
        if ((b.row->flags & SettingsTable::NotSaved) != 0) {
                char text[96];
                std::snprintf(text, sizeof(text), "%.*s: applied, not saved", int(label.size()), label.data());
                _window.say(StringView(text));
        } else if ((b.row->flags & SettingsTable::Confirm) != 0) {
                _window.confirm(b.id, before.view(), label);
        } else {
                _window.save(b.id, label);
        }
        if (b.kind == Kind::Text) {
                // Its canonical form shown (a path normalized), though it has the keys.
                String now(MemSpace::Psram);
                if (SysConf::get(b.id, now).isOk()) static_cast<LineEdit *>(b.editor)->setText(now.view());
        }
        marks(b);
        return;
}

void EditorSet::toggled(void *arg, CheckBox &box) {
        auto *b = static_cast<Binding *>(arg);
        b->set->commit(*b, box.isChecked() ? StringView("true") : StringView("false"), Apply::AndSave);
        return;
}

void EditorSet::chosen(void *arg, ComboBox &box, int index) {
        auto         *b = static_cast<Binding *>(arg);
        SysConf::Info info;
        if (index < 0 || SysConf::info(b->id, info).isError() || info.param == nullptr) return;
        const StringView choices(info.param->choices != nullptr ? info.param->choices : "");
        b->set->commit(*b, choiceWord(choices, box.itemData(size_t(index))), Apply::AndSave);
        return;
}

void EditorSet::numberChanged(void *arg, NumberField &, int32_t value) {
        auto *b = static_cast<Binding *>(arg);
        char  text[16];
        std::snprintf(text, sizeof(text), "%ld", long(value));
        b->set->commit(*b, StringView(text), Apply::AndSave);
        return;
}

void EditorSet::slid(void *arg, Slider &slider, int32_t value) {
        auto *b = static_cast<Binding *>(arg);
        char  text[16];
        std::snprintf(text, sizeof(text), "%ld", long(value));
        b->set->showSlider(*b, value);
        // Live while dragged (the volume heard), saved when let go; a step by key or click saves now.
        b->set->commit(*b, StringView(text), slider.isDragging() ? Apply::Only : Apply::AndSave);
        return;
}

void EditorSet::slidDone(void *arg, Slider &, int32_t value) {
        auto *b = static_cast<Binding *>(arg);
        char  text[16];
        std::snprintf(text, sizeof(text), "%ld", long(value));
        SysConf::Info info;
        b->set->commit(*b, StringView(text), Apply::AndSave);
        // Set already while dragged: saved now if it differs from the store.
        if (SysConf::info(b->id, info).isOk() && info.isChanged()) b->set->_window.save(b->id, labelOf(*b));
        b->set->marks(*b);
        return;
}

void EditorSet::modeChosen(void *arg, ComboBox &box, int index) {
        auto *b = static_cast<Binding *>(arg);
        if (index < 0) return;
        const size_t at = box.itemData(size_t(index));
        if (at < CustomMode) {
                b->set->commit(*b, StringView(Modes[at].value), Apply::AndSave);
                return;
        }
        // Custom: its field, to be typed in (taken at Enter or when it loses the keys).
        b->custom->setVisible(true);
        b->formRow->field()->relayout();
        b->custom->setFocus();
        return;
}

void EditorSet::textTaken(void *arg, LineEdit &edit) {
        auto *b = static_cast<Binding *>(arg);
        b->set->commit(*b, edit.text(), Apply::AndSave);
        return;
}

void EditorSet::textCancelled(void *arg, LineEdit &edit) {
        auto  *b = static_cast<Binding *>(arg);
        String value(MemSpace::Psram);
        if (SysConf::get(b->id, value).isOk()) edit.setText(value.view());
        return;
}

void EditorSet::listChanged(void *arg, ListEditor &editor) {
        auto *b = static_cast<Binding *>(arg);
        b->set->commit(*b, editor.text(), Apply::AndSave);
        return;
}

void EditorSet::listAdd(void *arg, ListEditor &) {
        auto *b = static_cast<Binding *>(arg);
        b->set->_window.addFolder(b->id, labelOf(*b));
        return;
}

void EditorSet::swatchClicked(void *arg, ColorSwatch &) {
        auto *b = static_cast<Binding *>(arg);
        b->set->_window.chooseColor(b->id, labelOf(*b));
        return;
}

void EditorSet::buttonClicked(void *arg, Button &button) {
        auto           *b = static_cast<Binding *>(arg);
        SettingsWindow &w = b->set->_window;
        if (&button == b->save) {
                w.save(b->id, labelOf(*b));
        } else if (&button == b->revert) {
                (void)SysConf::revert(b->id);
                w.say("Put back as saved");
        } else if (&button == b->editor && b->kind == Kind::Secret) {
                w.setPassword(); // the dialog sets it
        } else if (&button == b->extra && b->kind == Kind::Secret) {
                b->set->commit(*b, StringView(), Apply::AndSave); // the password removed
        } else if (&button == b->extra && b->kind == Kind::Pattern) {
                PatternSettings::openFolder(w);
        } else if (&button == b->extra && b->row->editor == SettingsTable::Editor::Zone) {
                w.chooseZone(b->id, (b->row->flags & SettingsTable::Empty) != 0, labelOf(*b));
        } else if (&button == b->extra) {
                w.choosePath(b->id, b->row->editor != SettingsTable::Editor::File, labelOf(*b));
        }
        b->set->load(*b);
        return;
}

// ---- SettingsPage

SettingsPage::SettingsPage(Widget *parent, SettingsWindow &window, size_t page)
    : BoxLayout(parent, Direction::Vertical), _window(window), _editors(window), _page(page) {}

SettingsPage::~SettingsPage() = default;

Error SettingsPage::build() {
        const SettingsTable::Page &page = SettingsTable::pages()[_page];
        auto                      *title = Widget::create<Label>(this, page.title);
        _form = Widget::create<FormLayout>(this);
        if (title == nullptr || _form == nullptr) return Error::NoMem;
        _form->setStretch(1);
        _form->setMargins(12, 4, 12, 12);
        _form->setSpacing(6);
        setMargins(0, 8, 0, 0);

        // Its status over its settings, and its own buttons.
        if (PageStatus::has(StringView(page.id))) {
                _status = Widget::create<TextBlock>(_form);
                if (_status == nullptr) return Error::NoMem;
                tick();
        }
        // A color page: its theme, and its colors on real-looking things.
        const StringView id(page.id);
        if (id == StringView("appearance") || id == StringView("terminal")) {
                _systemPreview = id == StringView("appearance");
                _themes = _systemPreview ? &Themes::system() : &Themes::terminal();
                FormRow *row = _form->addRow("&Theme", "Sets every color below at once (Custom: colors of your own)");
                if (row == nullptr) return Error::NoMem;
                _themeBox = Widget::create<ComboBox>(row->field());
                if (_systemPreview)
                        _preview = Widget::create<PalettePreview>(_form);
                else
                        _preview = Widget::create<TerminalPreview>(_form);
                if (_themeBox == nullptr || _preview == nullptr) return Error::NoMem;
                row->label()->setBuddy(_themeBox);
                for (size_t i = 0; i < _themes->themes.size(); ++i)
                        (void)_themeBox->addItem(_themes->themes[i].name, i);
                (void)_themeBox->addItem("Custom", _themes->themes.size());
                _themeBox->setChanged(themeChosen, this);
                showTheme();
        }
        const bool network = StringView(page.id) == StringView("network");
        const bool input = StringView(page.id) == StringView("input");
        if (network || input) {
                auto *row = Widget::create<BoxLayout>(_form, BoxLayout::Direction::Horizontal);
                if (row == nullptr) return Error::NoMem;
                Button *&button = network ? _renew : _bindings;
                button = Widget::create<Button>(row, network ? "&Renew Lease" : "Edit &Bindings...");
                if (button == nullptr) return Error::NoMem;
                button->setClicked(buttonClicked, this);
        }

        // How many editors, for their bindings' room (never moved after).
        size_t count = 0;
        for (const Row &r : page.rows)
                if (r.key != nullptr) eachKey(r, [&count](SysConf::Id) { ++count; });
        if (Error e = _editors.reserve(count); e.isError()) return e;

        // The rows, a section's heading only over rows that are there.
        const Row   *heading = nullptr;
        FormSection *section = nullptr;
        for (const Row &r : page.rows) {
                if (r.key == nullptr) {
                        heading = &r;
                        continue;
                }
                Error err;
                eachKey(r, [&](SysConf::Id key) {
                        if (err.isError()) return;
                        if (heading != nullptr) {
                                section = _form->addSection(heading->label);
                                if (section == nullptr) err = Error::NoMem;
                                heading = nullptr;
                        }
                        char         text[64];
                        const size_t n = SettingsTable::label(r, SysConf::keyOf(key), Span<char>(text, sizeof(text)));
                        EditorSet::Binding *b = _editors.add(*_form, key, r, StringView(text, n));
                        if (b == nullptr) {
                                err = Error::NoMem;
                                return;
                        }
                        b->section = section;
                        StringView star;
                        (void)SettingsTable::matches(StringView(r.key), SysConf::keyOf(key), star);
                        std::snprintf(b->star, sizeof(b->star), "%.*s", int(star.size()), star.data());
                });
                if (err.isError()) return err;
        }
        applyConditions();
        _form->relayout();
        return Error::Ok;
}

namespace {

        bool registryValue(void *, StringView key, char *out, size_t size) {
                String value(MemSpace::Psram);
                if (SysConf::get(key, value).isError()) return false;
                std::snprintf(out, size, "%s", value.cStr());
                return true;
        }

} // namespace

void SettingsPage::applyConditions() {
        bool moved = false;
        for (EditorSet::Binding &b : _editors.bindings()) {
                if (b.row->when == nullptr) continue;
                const bool on = SettingsTable::applies(b.row->when, StringView(b.star), registryValue, nullptr);
                if (b.formRow->isVisible() == on) continue;
                b.formRow->setVisible(on);
                if (b.below != nullptr) b.below->setVisible(on);
                moved = true;
        }
        if (!moved) return;
        // A heading shows while a row under it does.
        for (EditorSet::Binding &b : _editors.bindings()) {
                if (b.section == nullptr) continue;
                bool any = false;
                for (const EditorSet::Binding &o : _editors.bindings())
                        any = any || (o.section == b.section && o.formRow->isVisible());
                b.section->setVisible(any);
        }
        if (_form != nullptr) _form->relayout();
        return;
}

void SettingsPage::showTheme() {
        if (_themes == nullptr || _themeBox == nullptr) return;
        uint32_t     colors[32];
        const size_t n = _themes->names.size() < 32 ? _themes->names.size() : 32;
        for (size_t i = 0; i < n; ++i) {
                char key[64];
                std::snprintf(key, sizeof(key), "%s%s", _themes->prefix, _themes->names[i]);
                const Result<uint32_t> c = SysConf::getColor(StringView(key));
                colors[i] = c.isOk() ? c.value() : 0;
        }
        const int at = Themes::match(*_themes, Span<const uint32_t>(colors, n));
        _themeBox->setCurrent(at >= 0 ? at : int(_themes->themes.size()));
        return;
}

void SettingsPage::themeChosen(void *arg, ComboBox &box, int index) {
        auto              *self = static_cast<SettingsPage *>(arg);
        const Themes::Set *set = self->_themes;
        if (set == nullptr || index < 0 || box.itemData(size_t(index)) >= set->themes.size()) return; // Custom
        const Themes::Theme &theme = set->themes[box.itemData(size_t(index))];
        // Every color set, then saved together: the store written once, nothing else saved with it.
        SysConf::Id ids[32];
        size_t      n = 0;
        for (size_t i = 0; i < set->names.size() && n < 32; ++i) {
                char key[64];
                std::snprintf(key, sizeof(key), "%s%s", set->prefix, set->names[i]);
                const Result<SysConf::Id> id = SysConf::find(StringView(key));
                if (id.isError()) continue;
                char hex[8];
                std::snprintf(hex, sizeof(hex), "%06lx", static_cast<unsigned long>(theme.colors[i] & 0xffffffu));
                if (SysConf::set(id.value(), StringView(hex)).isOk()) ids[n++] = id.value();
        }
        const Error err = SysConf::save(Span<const SysConf::Id>(ids, n));
        char        text[96];
        std::snprintf(text, sizeof(text), "Theme %s: %s", theme.name, err.isOk() ? "saved" : err.desc());
        self->_window.say(StringView(text));
        return;
}

void SettingsPage::tick() {
        if (_status == nullptr) return;
        String text(MemSpace::Psram);
        PageStatus::text(StringView(SettingsTable::pages()[_page].id), _window.application().windowManager().screen(),
                         text);
        if (text.view() == _status->text()) return;
        const uint16_t width = _status->geometry().width;
        const uint16_t before = _status->heightForWidth(width);
        _status->setText(text.view());
        if (_renew != nullptr) _renew->setEnabled(PageStatus::leasedInterface() >= 0);
        if (_form != nullptr && _status->heightForWidth(width) != before) _form->relayout(); // its lines changed
        return;
}

void SettingsPage::buttonClicked(void *arg, Button &button) {
        auto *self = static_cast<SettingsPage *>(arg);
        if (&button == self->_renew) {
                const int   at = PageStatus::leasedInterface();
                const Error err = at >= 0 ? NetStatus::renewLease(size_t(at)) : Error::NotExist;
                self->_window.say(err.isOk() ? StringView("Asked DHCP to renew the lease")
                                             : StringView("No lease to renew"));
        } else if (&button == self->_bindings) {
                self->_window.run(StringView("controls"));
        }
        return;
}

bool SettingsPage::changed(SysConf::Id id) {
        // A color of its theme's: the preview and the theme shown follow.
        if (_themes != nullptr && (id == SysConf::NoId || SysConf::keyOf(id).startsWith(StringView(_themes->prefix)))) {
                if (_systemPreview)
                        static_cast<PalettePreview *>(_preview)->reload();
                else
                        static_cast<TerminalPreview *>(_preview)->reload();
                showTheme();
        }
        applyConditions(); // a row's condition may have changed with it
        // The desktop's: the pattern's editors and the preview read them all (a pattern chosen sets
        // its colors too; the desktop color shows through).
        const StringView key = id == SysConf::NoId ? StringView() : SysConf::keyOf(id);
        if (id == SysConf::NoId || key.startsWith(StringView("panel/desktop/")) ||
            key == StringView("wm/palette/desktop")) {
                for (EditorSet::Binding &b : _editors.bindings())
                        if (b.kind == Kind::Pattern || b.kind == Kind::PatternColors) _editors.load(b);
                if (_systemPreview && _preview != nullptr) static_cast<PalettePreview *>(_preview)->reload();
        }
        if (id == SysConf::NoId) {
                _editors.loadAll();
                return true;
        }
        EditorSet::Binding *b = _editors.find(id);
        if (b != nullptr) {
                _editors.load(*b);
                return true;
        }
        // A key it places that it doesn't show yet (an interface come): made again.
        return SettingsTable::place(SysConf::keyOf(id)).page != int(_page);
}

bool SettingsPage::reveal(StringView key, bool focus) {
        for (EditorSet::Binding &b : _editors.bindings()) {
                if (StringView(b.key) != key) continue;
                if (!b.formRow->isVisible()) {
                        // Hidden: say when it applies (its condition, the row's `*` filled in).
                        char             text[160];
                        const StringView label = b.kind == Kind::Check ? static_cast<CheckBox *>(b.editor)->text()
                                                                       : b.formRow->label()->text();
                        int n = std::snprintf(text, sizeof(text), "%.*s applies only when ", int(label.size()),
                                              label.data());
                        for (const char *c = b.row->when; *c != '\0' && n > 0 && size_t(n) + 16 < sizeof(text); ++c)
                                n += *c == '*'   ? std::snprintf(text + n, sizeof(text) - size_t(n), "%s", b.star)
                                     : *c == ',' ? std::snprintf(text + n, sizeof(text) - size_t(n), " and ")
                                                 : std::snprintf(text + n, sizeof(text) - size_t(n), "%c", *c);
                        _window.say(StringView(text));
                        return true;
                }
                _form->reveal(*b.formRow);
                // Lit, the last one put out: its label, or a check box's own text.
                if (_litLabel != nullptr) _litLabel->setHighlighted(false);
                if (_litBox != nullptr) _litBox->setHighlighted(false);
                _litLabel = nullptr;
                _litBox = nullptr;
                if (b.kind == Kind::Check) {
                        _litBox = static_cast<CheckBox *>(b.editor);
                        _litBox->setHighlighted(true);
                } else if (b.formRow->label() != nullptr) {
                        _litLabel = b.formRow->label();
                        _litLabel->setHighlighted(true);
                }
                if (focus) _editors.focus(b);
                return true;
        }
        return false;
}

// ---- AllSettingsPage

AllSettingsPage::AllSettingsPage(Widget *parent, SettingsWindow &window)
    : BoxLayout(parent, Direction::Vertical), _window(window), _editors(window) {}

AllSettingsPage::~AllSettingsPage() = default;

Error AllSettingsPage::build() {
        setMargins(8, 8, 8, 8);
        setSpacing(8);
        auto *title = Widget::create<Label>(this, "All Settings");
        _view = Widget::create<DetailsView>(this);
        if (title == nullptr || _view == nullptr) return Error::NoMem;
        const StringView       titles[] = {"Key", "Value", "State"};
        const uint16_t         widths[] = {260, 180, 72};
        const ItemModel::Align aligns[] = {ItemModel::Align::Left, ItemModel::Align::Left, ItemModel::Align::Left};
        _model.setColumns(Span<const StringView>(titles), Span<const uint16_t>(widths),
                          Span<const ItemModel::Align>(aligns));
        _view->setStretch(1);
        _view->setSingleSelection(true);
        _view->setModel(&_model);
        fill();
        _view->setChanged(rowChanged, this);
        if (!_ids.isEmpty()) {
                _view->selectOnly(0);
                detail();
        }
        return Error::Ok;
}

void AllSettingsPage::fill() {
        const int         keep = _view != nullptr ? _view->current() : -1;
        const SysConf::Id shown = keep >= 0 && size_t(keep) < _ids.size() ? _ids[size_t(keep)] : _shown;
        _model.clear();
        _ids.clear();
        SysConf::Id after = SysConf::NoId;
        int         select = -1;
        for (;;) {
                const Result<SysConf::Id> next = SysConf::next(StringView(), after);
                if (next.isError()) break;
                after = next.value();
                SysConf::Info info;
                if (SysConf::info(after, info).isError()) continue;
                const StringView columns[] = {info.value.view(), StringView(info.stateName())};
                if (_model.add(SysConf::keyOf(after), nullptr, nullptr, Span<const StringView>(columns)).isError() ||
                    _ids.append(after).isError())
                        break;
                if (after == shown) select = int(_ids.size() - 1);
        }
        if (_view == nullptr) return;
        void (*told)(void *, ItemView &) = rowChanged;
        _view->setChanged(nullptr, nullptr); // not told of its own reset
        const size_t row = select >= 0 ? size_t(select) : 0;
        _view->resetTo(Span<const size_t>(&row, select >= 0 ? 1 : 0), select);
        _view->setChanged(told, this);
        return;
}

void AllSettingsPage::detail() {
        const int cur = _view->current();
        if (cur < 0 || size_t(cur) >= _ids.size()) return;
        const SysConf::Id id = _ids[size_t(cur)];
        SysConf::Info     info;
        if (SysConf::info(id, info).isError() || info.param == nullptr) return;
        if (id != _shown || _detailBox == nullptr) {
                // Made again for another key: its editor, its facts, its buttons.
                _editors.clear();
                if (_detailBox != nullptr) _detailBox->destroy();
                _detailBox = Widget::create<BoxLayout>(this, Direction::Vertical);
                if (_detailBox == nullptr) return;
                _detailBox->setSpacing(4);
                _detail = Widget::create<FormLayout>(_detailBox);
                _facts = Widget::create<TextBlock>(_detailBox);
                auto *buttons = Widget::create<BoxLayout>(_detailBox, Direction::Horizontal);
                if (_detail == nullptr || _facts == nullptr || buttons == nullptr) return;
                _detail->setMargins(0, 0, 0, 0);
                buttons->setSpacing(8);
                _reset = Widget::create<Button>(buttons, "Reset to Default");
                _revert = Widget::create<Button>(buttons, "Revert");
                for (Button *b : {_reset, _revert})
                        if (b != nullptr) b->setClicked(clicked, this);
                const SettingsTable::Place at = SettingsTable::place(SysConf::keyOf(id));
                if (_editors.reserve(1).isOk())
                        (void)_editors.add(*_detail, id, at.row != nullptr ? *at.row : PlainRow, "Value");
                _shown = id;
        }
        // Its facts: the type and limits, the default, the saved value.
        char        text[TextBlock::MaxText + 1];
        const auto &p = *info.param;
        int         n = std::snprintf(text, sizeof(text), "%s", Config::typeName(p.type));
        if (p.type == Config::Type::Int)
                n += std::snprintf(text + n, sizeof(text) - size_t(n), " from %ld to %ld", long(p.min), long(p.max));
        if (p.type == Config::Type::Enum && p.choices != nullptr)
                n += std::snprintf(text + n, sizeof(text) - size_t(n), ": %s", p.choices);
        n += std::snprintf(text + n, sizeof(text) - size_t(n), ". Default: %s. %s%.*s%s", info.defaultValue.cStr(),
                           info.hasSaved ? "Saved: " : "Not saved", int(info.hasSaved ? info.saved.size() : 0),
                           info.saved.cStr(), info.runtime ? ". Declared at run time" : "");
        (void)n;
        _facts->setText(StringView(text));
        if (_reset != nullptr) _reset->setEnabled(!info.isDefault);
        if (_revert != nullptr) _revert->setEnabled(info.isChanged());
        relayout();
        relayout(); // the editor's form asks its height at the width the first gave it
        return;
}

void AllSettingsPage::changed(SysConf::Id id) {
        fill();
        if (id == SysConf::NoId || id == _shown) {
                _editors.loadAll();
                detail();
        }
        return;
}

bool AllSettingsPage::reveal(StringView key, bool focus) {
        for (size_t i = 0; i < _ids.size(); ++i) {
                if (SysConf::keyOf(_ids[i]) != key) continue;
                _view->selectOnly(int(i)); // told: the detail follows
                if (focus) _view->setFocus();
                return true;
        }
        return false;
}

void AllSettingsPage::rowChanged(void *arg, ItemView &) {
        static_cast<AllSettingsPage *>(arg)->detail();
        return;
}

void AllSettingsPage::clicked(void *arg, Button &button) {
        auto *self = static_cast<AllSettingsPage *>(arg);
        if (self->_shown == SysConf::NoId) return;
        const StringView key = SysConf::keyOf(self->_shown);
        if (&button == self->_reset) {
                if (SysConf::setDefault(self->_shown).isOk()) self->_window.save(self->_shown, key);
        } else if (&button == self->_revert) {
                if (SysConf::revert(self->_shown).isOk()) self->_window.say("Put back as saved");
        }
        return;
}

BMDS_NAMESPACE_END
