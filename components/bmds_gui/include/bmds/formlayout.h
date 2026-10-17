/**
 * @file      formlayout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/scrollview.h>
#include <bmds/widget.h>
#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

class FormLayout;

/**
 * @brief Text in lines broken at its width (`devplan/gui.md` §4u): a
 *        setting's help under its field, a paragraph on a page.
 *
 * It asks for as many lines as the text takes at its width
 * (`heightForWidth()`), breaking as `TextWrap` does, at most `MaxLines`.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class TextBlock : public Widget {
        public:
                static constexpr size_t MaxText = 511;
                static constexpr size_t MaxLines = 16;

                explicit TextBlock(Widget *parent, StringView text = StringView());

                /** @brief Sets the text (cut at `MaxText` bytes on a character); call the layout's `relayout()`. */
                void setText(StringView text);

                /** @brief Returns the text. */
                StringView text() const { return StringView(_text); }

                /** @brief Sets the text's color (the face's text by default; help: `DisabledText`). */
                void setColor(WindowPalette::Role role);

                GuiSize  sizeHint() const override;
                uint16_t heightForWidth(uint16_t width) const override;
                void     paint(Painter &painter) override;

        private:
                size_t wrapped(uint16_t width, StringView (&lines)[MaxLines]) const;

                char                _text[MaxText + 1] = {};
                WindowPalette::Role _role = WindowPalette::Role::FaceText;
};

/**
 * @brief A form's heading over the rows that follow it (`devplan/gui.md` §4u):
 *        its title, then an etched line to the right edge.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class FormSection : public Widget {
        public:
                static constexpr size_t   MaxText = 63;
                static constexpr uint16_t Above = 8; ///< Room above the title, from the rows before.

                FormSection(Widget *parent, StringView title);

                /** @brief Sets the title (cut at `MaxText` bytes on a character). */
                void setTitle(StringView title);

                /** @brief Returns the title. */
                StringView title() const { return StringView(_title); }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;

        private:
                char _title[MaxText + 1] = {};
};

/**
 * @brief One row of a form (`devplan/gui.md` §4u): a label in the form's label
 *        column, a field beside it for the editor (or editors), and help under
 *        the field, wrapped.
 *
 * Made by `FormLayout::addRow()`. Make the editors in `field()` (a row, each
 * its hint wide unless it stretches). A row without a label (a check box,
 * whose text is its label) starts its field in the same column as the rest.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class FormRow : public Widget {
        public:
                static constexpr uint16_t Gap = 8;     ///< The label column to the field.
                static constexpr uint16_t HelpGap = 2; ///< The field to its help.

                FormRow(Widget *parent, StringView label, StringView help);
                ~FormRow() override;

                /** @brief Returns its label (a buddy for its mnemonic: `Label::setBuddy()`), or null (no memory). */
                Label *label() const { return _label; }

                /** @brief Returns the field to make the editors in, or null (no memory). */
                BoxLayout *field() const { return _field; }

                /** @brief Returns the help under the field, or null (none). */
                TextBlock *help() const { return _help; }

                /** @brief Sets the help (empty: none); call the form's `relayout()`. @return `Ok` or `NoMem`. */
                Error setHelp(StringView text);

                /** @brief Returns the width its label wants (0: no label). */
                uint16_t labelHint() const;

                /** @brief Sets the label column's width (the form's: its widest label). */
                void setLabelWidth(uint16_t width);

                GuiSize  sizeHint() const override;
                uint16_t heightForWidth(uint16_t width) const override;
                void     paint(Painter &painter) override;

        protected:
                void resized() override;

        private:
                friend class FormLayout;

                uint16_t fieldX(uint16_t width) const; // the field's start, from the row's left
                uint16_t fieldHeight() const;
                void     layout();

                FormLayout *_form = nullptr; // told when it goes
                Label      *_label = nullptr;
                BoxLayout  *_field = nullptr;
                TextBlock  *_help = nullptr;
                uint16_t    _labelWidth = 0;
};

/**
 * @brief Labelled editors in sections, scrolled by rows (`devplan/gui.md`
 *        §4u): a settings page.
 *
 * `addSection()` and `addRow()` add rows in order; any other widget made in
 * it is a row too, as wide as the rows (a page's status). The label column
 * is as wide as the widest label, at most two fifths of the width. As a
 * `ScrollView`, call `relayout()` after adding rows once it is shown.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class FormLayout : public ScrollView {
        public:
                explicit FormLayout(Widget *parent);
                ~FormLayout() override;

                /** @brief Adds a heading. @return It, or null (no memory). */
                FormSection *addSection(StringView title);

                /**
                 * @brief Adds a row labelled @p label (empty: none) with @p help (empty:
                 *        none) under its field.
                 * @return The row (make its editors in `field()`), or null (no memory).
                 */
                FormRow *addRow(StringView label, StringView help = StringView());

        protected:
                void layingOut(uint16_t width) override;

        private:
                friend class FormRow;

                void forget(FormRow *row);

                FormRow *_rows[MaxRows] = {}; // the rows made by addRow(), for the label column
                size_t   _rowCount = 0;
};

BMDS_NAMESPACE_END
