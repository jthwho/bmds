/**
 * @file      formlayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Forms: wrapped text, sections, labelled rows, scrolled by rows (devplan/gui.md §4u).
 */

#include <cstring>
#include <bmds/font.h>
#include <bmds/formlayout.h>
#include <bmds/lineeditmodel.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        // @p text into @p out (@p size bytes with the NUL), cut on a character's boundary.
        void copyText(char *out, size_t size, StringView text) {
                size_t n = text.size() < size - 1 ? text.size() : size - 1;
                if (n < text.size())
                        while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xc0u) == 0x80u) --n;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

        uint16_t columnUp(uint32_t x) {
                return static_cast<uint16_t>((x + GuiStyle::Column - 1) & ~uint32_t(GuiStyle::Column - 1));
        }

} // namespace

// ---- TextBlock

TextBlock::TextBlock(Widget *parent, StringView text) : Widget(parent) {
        copyText(_text, sizeof(_text), text);
}

void TextBlock::setText(StringView text) {
        char next[MaxText + 1];
        copyText(next, sizeof(next), text);
        if (std::strcmp(next, _text) == 0) return;
        std::memcpy(_text, next, sizeof(_text));
        update();
        return;
}

void TextBlock::setColor(WindowPalette::Role role) {
        _role = role;
        update();
        return;
}

size_t TextBlock::wrapped(uint16_t width, StringView (&lines)[MaxLines]) const {
        const uint16_t columns = static_cast<uint16_t>(width / Fonts::unscii16.width());
        if (_text[0] == '\0' || columns == 0) return 0;
        return TextWrap::wrap(StringView(_text), columns, Span<StringView>(lines, MaxLines));
}

GuiSize TextBlock::sizeHint() const {
        StringView   lines[MaxLines];
        const size_t n = wrapped(UINT16_MAX - 7, lines); // only its own line breaks
        uint32_t     width = 0;
        for (size_t i = 0; i < n; ++i) {
                const uint32_t w = Fonts::unscii16.textWidth(lines[i]);
                if (w > width) width = w;
        }
        return GuiSize{columnUp(width), static_cast<uint16_t>(n * GuiStyle::TextHeight)};
}

uint16_t TextBlock::heightForWidth(uint16_t width) const {
        StringView lines[MaxLines];
        return static_cast<uint16_t>(wrapped(width, lines) * GuiStyle::TextHeight);
}

void TextBlock::paint(Painter &painter) {
        const Rect    &r = geometry();
        const uint32_t bg = painter.palette().color(Role::Face);
        const uint32_t fg = painter.palette().color(isEnabled() ? _role : Role::DisabledText);
        (void)painter.fill(r, bg);
        const uint16_t x = columnUp(r.x);
        if (x >= r.right()) return;
        StringView   lines[MaxLines];
        const size_t n = wrapped(static_cast<uint16_t>(r.right() - x), lines);
        for (size_t i = 0; i < n; ++i) {
                const uint32_t y = r.y + i * GuiStyle::TextHeight;
                if (y + GuiStyle::TextHeight > r.bottom()) break;
                (void)painter.text(x, int(y), lines[i], Fonts::unscii16, fg, bg, static_cast<uint16_t>(r.right() - x));
        }
        return;
}

// ---- FormSection

FormSection::FormSection(Widget *parent, StringView title) : Widget(parent) {
        copyText(_title, sizeof(_title), title);
}

void FormSection::setTitle(StringView title) {
        copyText(_title, sizeof(_title), title);
        update();
        return;
}

GuiSize FormSection::sizeHint() const {
        return GuiSize{columnUp(Fonts::unscii16.textWidth(StringView(_title)) + 16),
                       static_cast<uint16_t>(Above + GuiStyle::ButtonHeight)};
}

void FormSection::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &r = geometry();
        const uint32_t       bg = p.color(Role::Face);
        (void)painter.fill(r, bg);
        const uint16_t x = columnUp(r.x);
        const uint32_t top = r.y + Above + (GuiStyle::ButtonHeight - GuiStyle::TextHeight) / 2;
        if (x >= r.right() || top + GuiStyle::TextHeight > r.bottom()) return;
        const StringView title(_title);
        (void)painter.text(x, int(top), title, Fonts::unscii16,
                           p.color(isEnabled() ? Role::FaceText : Role::DisabledText), bg,
                           static_cast<uint16_t>(r.right() - x));
        // The etched line, from after the title to the right edge, at the text's middle.
        const uint32_t from = x + Fonts::unscii16.textWidth(title) + 8;
        if (from + 8 > r.right()) return;
        const auto y = static_cast<uint16_t>(top + GuiStyle::TextHeight / 2);
        const auto w = static_cast<uint16_t>(r.right() - from);
        (void)painter.fill(Rect{uint16_t(from), y, w, 1}, p.color(Role::FaceShadow));
        (void)painter.fill(Rect{uint16_t(from), uint16_t(y + 1), w, 1}, p.color(Role::FaceHighlight));
        return;
}

// ---- FormRow

FormRow::FormRow(Widget *parent, StringView label, StringView help) : Widget(parent) {
        _label = Widget::create<Label>(this, label);
        _field = Widget::create<BoxLayout>(this, BoxLayout::Direction::Horizontal);
        if (_field != nullptr) _field->setSpacing(8);
        if (!help.isEmpty()) _help = Widget::create<TextBlock>(this, help);
        if (_help != nullptr) _help->setColor(Role::DisabledText);
}

FormRow::~FormRow() {
        if (_form != nullptr) _form->forget(this);
}

Error FormRow::setHelp(StringView text) {
        if (_help == nullptr && text.isEmpty()) return Error::Ok;
        if (_help == nullptr) {
                _help = Widget::create<TextBlock>(this, text);
                if (_help == nullptr) return Error::NoMem;
                _help->setColor(Role::DisabledText);
        }
        _help->setText(text);
        _help->setVisible(!text.isEmpty());
        layout();
        return Error::Ok;
}

uint16_t FormRow::labelHint() const {
        return _label != nullptr && !_label->text().isEmpty() ? _label->sizeHint().width : 0;
}

void FormRow::setLabelWidth(uint16_t width) {
        width = columnUp(width);
        if (width == _labelWidth) return;
        _labelWidth = width;
        layout();
        return;
}

uint16_t FormRow::fieldX(uint16_t width) const {
        const uint32_t x = _labelWidth > 0 ? uint32_t(_labelWidth) + Gap : 0;
        return static_cast<uint16_t>(x < width ? columnUp(x) : width);
}

uint16_t FormRow::fieldHeight() const {
        const uint16_t h = _field != nullptr ? _field->sizeHint().height : 0;
        return h > GuiStyle::ButtonHeight ? h : GuiStyle::ButtonHeight;
}

GuiSize FormRow::sizeHint() const {
        const uint16_t fx = fieldX(UINT16_MAX);
        const GuiSize  f = _field != nullptr ? _field->sizeHint() : GuiSize();
        return GuiSize{static_cast<uint16_t>(fx + f.width), heightForWidth(static_cast<uint16_t>(fx + f.width))};
}

uint16_t FormRow::heightForWidth(uint16_t width) const {
        uint32_t h = fieldHeight();
        if (_help != nullptr && _help->isVisible()) {
                const uint16_t lines = _help->heightForWidth(static_cast<uint16_t>(width - fieldX(width)));
                if (lines > 0) h += HelpGap + lines;
        }
        return static_cast<uint16_t>(h > UINT16_MAX ? UINT16_MAX : h);
}

void FormRow::layout() {
        const Rect    &r = geometry();
        const uint16_t fx = fieldX(r.width);
        const uint16_t fh = fieldHeight();
        const uint16_t fw = static_cast<uint16_t>(r.width - fx);
        if (_label != nullptr)
                _label->setGeometry(Rect{r.x, r.y, _labelWidth < r.width ? _labelWidth : r.width,
                                         fh < GuiStyle::ButtonHeight ? fh : GuiStyle::ButtonHeight});
        if (_field != nullptr) _field->setGeometry(Rect{uint16_t(r.x + fx), r.y, fw, fh});
        if (_help != nullptr && _help->isVisible()) {
                const uint32_t y = uint32_t(r.y) + fh + HelpGap;
                const uint32_t h = y < r.bottom() ? r.bottom() - y : 0;
                _help->setGeometry(Rect{uint16_t(r.x + fx), uint16_t(y), fw, uint16_t(h)});
        }
        return;
}

void FormRow::resized() {
        layout();
        return;
}

void FormRow::paint(Painter &painter) {
        fillOutsideChildren(painter, geometry(), painter.palette().color(Role::Face));
        return;
}

// ---- FormLayout

FormLayout::FormLayout(Widget *parent) : ScrollView(parent) {}

FormLayout::~FormLayout() {
        for (size_t i = 0; i < _rowCount; ++i) _rows[i]->_form = nullptr; // destroyed with it, after this
}

FormSection *FormLayout::addSection(StringView title) {
        return Widget::create<FormSection>(this, title);
}

FormRow *FormLayout::addRow(StringView label, StringView help) {
        if (_rowCount == MaxRows) return nullptr;
        auto *row = Widget::create<FormRow>(this, label, help);
        if (row == nullptr) return nullptr;
        if (row->label() == nullptr || row->field() == nullptr || (!help.isEmpty() && row->help() == nullptr)) {
                row->destroy();
                return nullptr;
        }
        row->_form = this;
        _rows[_rowCount++] = row;
        return row;
}

void FormLayout::forget(FormRow *row) {
        for (size_t i = 0; i < _rowCount; ++i) {
                if (_rows[i] != row) continue;
                std::memmove(&_rows[i], &_rows[i + 1], (_rowCount - i - 1) * sizeof(_rows[0]));
                --_rowCount;
                return;
        }
        return;
}

void FormLayout::layingOut(uint16_t width) {
        // The label column: the widest label shown, at most two fifths of the width.
        uint16_t widest = 0;
        for (size_t i = 0; i < _rowCount; ++i) {
                if (!_rows[i]->isVisible()) continue;
                const uint16_t w = _rows[i]->labelHint();
                if (w > widest) widest = w;
        }
        const auto most = static_cast<uint16_t>(uint32_t(width) * 2 / 5);
        if (widest > most) widest = most;
        for (size_t i = 0; i < _rowCount; ++i) _rows[i]->setLabelWidth(widest);
        return;
}

BMDS_NAMESPACE_END
