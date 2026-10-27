/**
 * @file      fontdialog.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The font dialog and the font button (devplan/fonts.md §4i): families from the system's font
 * library, each drawn in itself; styles from the family's faces; sizes; a preview.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/detailsview.h>
#include <bmds/fontdialog.h>
#include <bmds/guiwindow.h>
#include <bmds/lineedit.h>
#include <bmds/numberfield.h>
#include <bmds/systemfonts.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t ContentWidth = 560;
        constexpr uint16_t ContentHeight = 360;
        constexpr char     DefaultSample[] = "The quick brown fox jumps over the lazy dog 0123";

        // The sizes a TrueType face is offered at.
        constexpr uint16_t Sizes[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 36, 40, 48, 56, 64, 72};

        // A one-column list as wide as it is, without a header (the GUI service's ListView's look).
        class ColumnList final : public DetailsView {
                public:
                        explicit ColumnList(Widget *parent) : DetailsView(parent) {
                                setHeaderVisible(false);
                                setSingleSelection(true);
                        }

                protected:
                        void resized() override {
                                DetailsView::resized();
                                const uint16_t w = uint16_t(viewport().width & ~3u);
                                if (w != 0 && columnWidth(0) != w) setColumnWidth(0, w);
                                return;
                        }
        };

        void copyText(char *out, size_t size, StringView text) {
                size_t n = text.size() < size - 1 ? text.size() : size - 1;
                while (n > 0 && n < text.size() && (uint8_t(text[n]) & 0xc0u) == 0x80u) --n;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

        // @p hay holds @p needle, ASCII letters in either case.
        bool contains(StringView hay, StringView needle) {
                const auto lower = [](char c) {
                        return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                };
                if (needle.isEmpty()) return true;
                for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                        size_t k = 0;
                        while (k < needle.size() && lower(hay[i + k]) == lower(needle[k])) ++k;
                        if (k == needle.size()) return true;
                }
                return false;
        }

} // namespace

FontDialog::FontDialog(GuiApplication &app, StringView title) : Dialog(app, title, ContentWidth, ContentHeight) {
        (void)build();
        (void)addButton("OK", Accepted);
        (void)addButton("Cancel", Rejected);
        setCancelResult(Rejected);
}

FontDialog::~FontDialog() = default;

Error FontDialog::build() {
        BoxLayout *content = this->content();
        if (content == nullptr) return Error::NoMem;
        content->setSpacing(8);
        auto *lists = Widget::create<BoxLayout>(content, BoxLayout::Direction::Horizontal);
        if (lists == nullptr) return Error::NoMem;
        lists->setSpacing(8);
        lists->setStretch(1);
        const auto column = [&](StringView title, uint8_t stretch) -> BoxLayout * {
                auto *c = Widget::create<BoxLayout>(lists, BoxLayout::Direction::Vertical);
                if (c == nullptr) return nullptr;
                c->setSpacing(4);
                c->setStretch(stretch);
                if (Widget::create<Label>(c, title) == nullptr) return nullptr;
                return c;
        };
        BoxLayout *families = column("Family", 3), *styles = column("Style", 2), *sizes = column("Size", 1);
        if (families == nullptr || styles == nullptr || sizes == nullptr) return Error::NoMem;
        _search = Widget::create<LineEdit>(families);
        _familyList = Widget::create<ColumnList>(families);
        _styleList = Widget::create<ColumnList>(styles);
        _size = Widget::create<NumberField>(sizes);
        _sizeList = Widget::create<ColumnList>(sizes);
        _preview = Widget::create<Label>(content, DefaultSample);
        if (_search == nullptr || _familyList == nullptr || _styleList == nullptr || _size == nullptr ||
            _sizeList == nullptr || _preview == nullptr)
                return Error::NoMem;
        _familyList->setStretch(1);
        _styleList->setStretch(1);
        _sizeList->setStretch(1);
        _familyList->setModel(&_families);
        _styleList->setModel(&_styles);
        _sizeList->setModel(&_sizes);
        _search->setEdited(searchEdited, this);
        _familyList->setChanged(familyChanged, this);
        _styleList->setChanged(styleRowChanged, this);
        _sizeList->setChanged(sizeListChanged, this);
        _size->setRange(FontName::MinSize, FontName::MaxSize);
        _size->setChanged(sizeFieldChanged, this);
        _preview->setFrame(Bevel::Style::Field);
        _preview->setAlign(Label::Align::Center);
        _preview->setMinimumWidth(ContentWidth);
        return Error::Ok;
}

Error FontDialog::open(StringView name) {
        // What the name comes to: a role as the font it is now, a family the nearest one there.
        FontLibrary &library = SystemFonts::library();
        auto [parsed, perr] = FontName::parse(name);
        if (perr.isError()) parsed = FontName::make("unscii", 16, 0);
        auto [font, ferr] = library.open(parsed);
        if (ferr.isOk()) {
                const FontLibrary::FaceInfo face = library.face(font.faceIndex());
                copyText(_family, sizeof(_family), face.family);
                _pixels = static_cast<uint16_t>(font.pixelSize());
                _style = static_cast<uint8_t>((face.weight >= 600 || font.isMadeUpBold() ? FontName::Bold : 0) |
                                              (face.italic ? FontName::Italic : 0));
        }
        _filling = true;
        _search->setText(StringView());
        listFamilies();
        listStyles();
        listSizes();
        _filling = false;
        chosen(false);
        return Dialog::open();
}

void FontDialog::setSample(StringView text) {
        if (_preview != nullptr) _preview->setText(text.isEmpty() ? StringView(DefaultSample) : text);
        return;
}

void FontDialog::listFamilies() {
        FontLibrary     &library = SystemFonts::library();
        const StringView search = _search != nullptr ? _search->text() : StringView();
        const Font       ui = GuiStyle::font();
        _families.clear();
        for (Font &f : _families.fonts) f = Font();
        int current = -1;
        for (size_t i = 0; i < library.faceCount() && _families.count() < MaxFamilies; ++i) {
                const FontLibrary::FaceInfo face = library.face(i);
                if (!face.present) continue;
                if (_filter == Filter::Monospace && !face.fixedPitch) continue;
                if (_filter == Filter::Scalable && face.bitmapSize != 0) continue;
                if (!contains(face.family, search)) continue;
                bool seen = false;
                for (size_t r = 0; r < _families.count() && !seen; ++r) seen = _families.text(r) == face.family;
                if (seen) continue;
                const size_t row = _families.count();
                if (_families.add(face.family, nullptr, nullptr).isError()) break;
                // Each in itself, at the list's own size (a bitmap face at its own).
                const uint16_t size = face.bitmapSize != 0 ? face.bitmapSize : uint16_t(ui.pixelSize());
                auto [font, err] = library.open(FontName::make(face.family, size, 0));
                if (err.isOk()) _families.fonts[row] = font;
                if (face.family == StringView(_family)) current = int(row);
        }
        _familyList->modelReset();
        if (current >= 0) {
                _familyList->selectOnly(current);
        } else if (_families.count() > 0 && search.isEmpty()) {
                // The family isn't offered (filtered out): the first.
                copyText(_family, sizeof(_family), _families.text(0));
                _familyList->selectOnly(0);
        }
        return;
}

bool FontDialog::isBitmapFamily() const {
        FontLibrary &library = SystemFonts::library();
        for (size_t i = 0; i < library.faceCount(); ++i) {
                const FontLibrary::FaceInfo face = library.face(i);
                if (face.family == StringView(_family)) return face.bitmapSize != 0;
        }
        return false;
}

void FontDialog::listStyles() {
        FontLibrary &library = SystemFonts::library();
        // The family's faces by style (regular, bold, italic, bold italic: FontName's bits), the
        // names their own.
        StringView names[4];
        bool       has[4] = {};
        for (size_t i = 0; i < library.faceCount(); ++i) {
                const FontLibrary::FaceInfo face = library.face(i);
                if (face.family != StringView(_family) || !face.present) continue;
                const uint8_t flags =
                        uint8_t((face.weight >= 600 ? FontName::Bold : 0) | (face.italic ? FontName::Italic : 0));
                if (has[flags]) continue;
                has[flags] = true;
                names[flags] = face.style.isEmpty() ? StringView("Regular") : face.style;
        }
        // A bold the library makes up where the family has none.
        if (!has[FontName::Bold] && has[0]) {
                has[FontName::Bold] = true;
                names[FontName::Bold] = "Bold";
        }
        _styles.clear();
        size_t count = 0;
        int    current = -1;
        for (uint8_t flags = 0; flags < 4; ++flags) {
                if (!has[flags] || _styles.add(names[flags], nullptr, nullptr).isError()) continue;
                _styleOf[count].flags = flags;
                if (flags == _style) current = int(count);
                ++count;
        }
        _styleList->modelReset();
        if (current < 0 && count > 0) {
                current = 0;
                _style = _styleOf[0].flags;
        }
        if (current >= 0) _styleList->selectOnly(current);
        return;
}

void FontDialog::listSizes() {
        _sizes.clear();
        size_t     count = 0;
        int        current = -1;
        char       text[8];
        const auto add = [&](uint16_t px) {
                if (count >= sizeof(_sizeOf) / sizeof(_sizeOf[0])) return;
                const int n = std::snprintf(text, sizeof(text), "%u", unsigned(px));
                if (_sizes.add(StringView(text, size_t(n)), nullptr, nullptr).isError()) return;
                if (px == _pixels) current = int(count);
                _sizeOf[count++] = px;
                return;
        };
        if (isBitmapFamily()) {
                // Its multiples: what it is drawn at.
                FontLibrary &library = SystemFonts::library();
                uint16_t     base = 16;
                for (size_t i = 0; i < library.faceCount(); ++i)
                        if (library.face(i).family == StringView(_family)) base = library.face(i).bitmapSize;
                for (uint16_t k = 1; k <= 8 && uint32_t(base) * k <= FontName::MaxSize; ++k) add(uint16_t(base * k));
                if (current < 0 && base != 0) {
                        _pixels = uint16_t(base * ((_pixels + base / 2) / base > 0 ? (_pixels + base / 2) / base : 1));
                        for (size_t i = 0; i < count; ++i)
                                if (_sizeOf[i] == _pixels) current = int(i);
                }
        } else {
                for (const uint16_t px : Sizes) add(px);
        }
        _sizeList->modelReset();
        if (current >= 0) _sizeList->selectOnly(current);
        _size->setValue(_pixels);
        return;
}

void FontDialog::chosen(bool tell) {
        const FontName name = FontName::make(StringView(_family), _pixels, _style);
        (void)name.format(_name, sizeof(_name));
        if (_preview != nullptr) (void)_preview->setFont(StringView(_name));
        if (tell && _changed != nullptr) _changed(_changedArg, *this, StringView(_name));
        return;
}

void FontDialog::searchEdited(void *arg, LineEdit &) {
        auto *self = static_cast<FontDialog *>(arg);
        self->_filling = true;
        self->listFamilies();
        self->_filling = false;
        return;
}

void FontDialog::familyChanged(void *arg, ItemView &view) {
        auto     *self = static_cast<FontDialog *>(arg);
        const int row = view.current();
        if (self->_filling || row < 0 || size_t(row) >= self->_families.count()) return;
        if (self->_families.text(size_t(row)) == StringView(self->_family)) return;
        copyText(self->_family, sizeof(self->_family), self->_families.text(size_t(row)));
        self->_filling = true;
        self->listStyles();
        self->listSizes();
        self->_filling = false;
        self->chosen(true);
        return;
}

void FontDialog::styleRowChanged(void *arg, ItemView &view) {
        auto     *self = static_cast<FontDialog *>(arg);
        const int row = view.current();
        if (self->_filling || row < 0 || size_t(row) >= self->_styles.count()) return;
        self->_style = self->_styleOf[row].flags;
        self->chosen(true);
        return;
}

void FontDialog::sizeListChanged(void *arg, ItemView &view) {
        auto     *self = static_cast<FontDialog *>(arg);
        const int row = view.current();
        if (self->_filling || row < 0 || size_t(row) >= self->_sizes.count()) return;
        self->_pixels = self->_sizeOf[row];
        self->_size->setValue(self->_pixels);
        self->chosen(true);
        return;
}

void FontDialog::sizeFieldChanged(void *arg, NumberField &, int32_t value) {
        auto *self = static_cast<FontDialog *>(arg);
        if (self->_filling) return;
        self->_pixels = uint16_t(value);
        int current = -1;
        for (size_t i = 0; i < self->_sizes.count(); ++i)
                if (self->_sizeOf[i] == self->_pixels) current = int(i);
        self->_filling = true;
        self->_sizeList->selectOnly(current);
        self->_filling = false;
        self->chosen(true);
        return;
}

// ---- FontButton

FontButton::FontButton(Widget *parent, StringView name) : Button(parent) {
        Button::setClicked(clicked, this);
        if (!name.isEmpty()) (void)setFontName(name);
}

FontButton::~FontButton() {
        dropDialog();
}

Error FontButton::setFontName(StringView name) {
        if (!name.isEmpty() && FontName::parse(name).error().isError()) return Error::Invalid;
        setText(name); // before the font: setFont() lays the window out again, for both
        return setFont(name);
}

void FontButton::dropDialog() {
        if (_dialog == nullptr) return;
        _dialog->~FontDialog();
        Memory::free(_dialog);
        _dialog = nullptr;
        return;
}

void FontButton::clicked(void *arg, Button &) {
        auto      *self = static_cast<FontButton *>(arg);
        GuiWindow *w = self->window();
        if (w == nullptr) return;
        if (self->_dialog != nullptr && self->_dialog->isShown()) {
                self->_dialog->activate();
                return;
        }
        self->dropDialog();
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(FontDialog));
        if (memory == nullptr) return;
        self->_dialog = ::new (memory) FontDialog(w->application(), "Font");
        self->_dialog->setFilter(self->_filter);
        self->_dialog->setFinished(finished, self);
        const StringView now = self->fontName();
        if (self->_dialog->open(now.isEmpty() ? StringView("ui") : now).isError()) self->dropDialog();
        return;
}

void FontButton::finished(void *arg, Dialog &dialog, int result) {
        auto *self = static_cast<FontButton *>(arg);
        if (result != FontDialog::Accepted) return;
        char name[FontName::MaxText + 1];
        copyText(name, sizeof(name), static_cast<FontDialog &>(dialog).fontName());
        if (self->setFontName(StringView(name)).isError()) return;
        if (self->_changedFn != nullptr) self->_changedFn(self->_changedArg, *self, StringView(name));
        return;
}

BMDS_NAMESPACE_END
