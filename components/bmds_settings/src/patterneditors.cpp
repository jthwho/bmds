/**
 * @file      patterneditors.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop pattern's editors: the grid of patterns, and the current one's colors and palettes
 * (devplan/wallpaper.md §4c, §4f).
 */

#include <cstdio>
#include <cstring>
#include <utility>
#include <bmds/colordialog.h>
#include <bmds/combobox.h>
#include <bmds/font.h>
#include <bmds/memory.h>
#include <bmds/patternfiles.h>
#include <bmds/pngdecoder.h>
#include <bmds/scrollview.h>
#include <bmds/sysconf.h>
#include <bmds/tiledpattern.h>

#include "patterneditors.h"
#include "settingstable.h"
#include "settingswindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t  SwatchTop = 4; // in its cell
        constexpr uint16_t  NameTop = PatternChooser::SwatchHeight + 10;
        constexpr size_t    MaxPalettes = 48;      // built-in and saved, offered
        constexpr uintptr_t Unsaved = UINTPTR_MAX; // the combo's item for colors no palette has

        // A setting's value, or empty.
        void read(const char *key, String &out) {
                out.clear();
                (void)SysConf::get(StringView(key), out);
                return;
        }

        uint32_t desktopColor() {
                const Result<uint32_t> c = SysConf::getColor(StringView("wm/palette/desktop"));
                return c.isOk() ? c.value() : 0;
        }

        bool focusShown(const Widget &w) {
                const GuiWindow *window = w.window();
                return w.hasFocus() && window != nullptr && window->isActive();
        }

        void ring(Painter &p, int x, int y, int w, int h, uint16_t width, uint32_t rgb) {
                const Rect sides[] = {
                        {uint16_t(x), uint16_t(y), uint16_t(w), width},
                        {uint16_t(x), uint16_t(y + h - width), uint16_t(w), width},
                        {uint16_t(x), uint16_t(y + width), width, uint16_t(h - 2 * width)},
                        {uint16_t(x + w - width), uint16_t(y + width), width, uint16_t(h - 2 * width)},
                };
                (void)p.fill(Span<const Rect>(sides, 4), rgb);
                return;
        }

        // A pattern's palette from @p file, as many entries as @p out holds.
        size_t paletteOf(ByteSpan file, uint32_t (&out)[TiledPattern::MaxColors]) {
                const Result<size_t> n = Png::palette(file, Span<uint32_t>(out, TiledPattern::MaxColors));
                return n.isOk() ? n.value() : 0;
        }

        // @p colors over @p base's first @p count entries into @p out (equal palettes compared so).
        void over(const uint32_t *base, size_t count, StringView colors, uint32_t *out) {
                for (size_t i = 0; i < count; ++i) out[i] = base[i];
                (void)TiledPattern::parseColors(colors, Span<uint32_t>(out, count));
                return;
        }

        // @p colors as `#rrggbb;...`.
        Error format(const uint32_t *colors, size_t count, String &out) {
                out.clear();
                for (size_t i = 0; i < count; ++i) {
                        char hex[9];
                        std::snprintf(hex, sizeof(hex), "%s#%06lx", i == 0 ? "" : ";",
                                      static_cast<unsigned long>(colors[i] & 0xffffffu));
                        if (out.append(StringView(hex)).isError()) return Error::NoMem;
                }
                return Error::Ok;
        }

} // namespace

// ---- PatternSettings

Error PatternSettings::storeCurrent(StringView pattern, StringView colors) {
        if (pattern.isEmpty()) return Error::Ok;
        String path(MemSpace::Psram), text(MemSpace::Psram), out(MemSpace::Psram);
        read(PatternFiles::PalettesKey, path);
        if (path.isEmpty()) return Error::NotExist;
        Error err = PatternFiles::readText(path.view(), text);
        if (err.isError()) return err;
        // Nothing kept if nothing changes: the same colors, or its own with none kept before.
        StringView now;
        const bool had = PatternPalettes::current(text.view(), pattern, now);
        if (had ? now == colors : colors.isEmpty()) return Error::Ok;
        err = PatternPalettes::set(text.view(), pattern, StringView(PatternPalettes::CurrentKey), colors, out);
        if (err.isOk()) err = PatternFiles::writeText(path.view(), out.view());
        return err;
}

void PatternSettings::choose(SettingsWindow &window, StringView name) {
        String oldName(MemSpace::Psram), oldColors(MemSpace::Psram), path(MemSpace::Psram), text(MemSpace::Psram);
        read(PatternFiles::PatternKey, oldName);
        read(PatternFiles::ColorsKey, oldColors);
        if (oldName.view() == name) return;
        const Error kept = storeCurrent(oldName.view(), oldColors.view());
        // The new one's colors: as it last had them, else its own.
        read(PatternFiles::PalettesKey, path);
        StringView colors;
        if (path.isEmpty() || PatternFiles::readText(path.view(), text).isError() ||
            !PatternPalettes::current(text.view(), name, colors))
                colors = StringView();
        const Result<SysConf::Id> colorsId = SysConf::find(StringView(PatternFiles::ColorsKey));
        const Result<SysConf::Id> patternId = SysConf::find(StringView(PatternFiles::PatternKey));
        if (colorsId.isError() || patternId.isError()) return;
        Error err = SysConf::set(colorsId.value(), colors);
        if (err.isOk()) err = SysConf::set(patternId.value(), name);
        if (err.isError()) {
                window.refused("Pattern", patternId.value(), err);
                return;
        }
        const SysConf::Id ids[] = {colorsId.value(), patternId.value()};
        err = SysConf::save(Span<const SysConf::Id>(ids, 2));
        char label[64];
        (void)SettingsTable::labelOf(name, Span<char>(label, sizeof(label)));
        char text2[160];
        std::snprintf(text2, sizeof(text2), "Pattern %s: %s%s%s", label, err.isOk() ? "saved" : err.desc(),
                      kept.isError() ? "; the last one's colors not kept: " : "", kept.isError() ? kept.desc() : "");
        window.say(StringView(text2));
        return;
}

void PatternSettings::openFolder(SettingsWindow &window) {
        String paths(MemSpace::Psram), command(MemSpace::Psram);
        read(PatternFiles::PathsKey, paths);
        const size_t     semi = paths.view().indexOf(';');
        const StringView first = (semi == StringView::NPos ? paths.view() : paths.view().left(semi)).trimmed();
        if (first.isEmpty()) {
                window.say("Patterns from: no folder");
                return;
        }
        const Error err = PatternFiles::makeFolders(first);
        if (err.isError() || first.indexOf(' ') != StringView::NPos || command.assign("files ").isError() ||
            command.append(first).isError()) {
                char text[160];
                std::snprintf(text, sizeof(text), "Can't open %.*s: %s", int(first.size()), first.data(),
                              err.isError() ? err.desc() : "its name has a space");
                window.say(StringView(text));
                return;
        }
        window.run(command.view());
        return;
}

// ---- PatternChooser

PatternChooser::PatternChooser(Widget *parent, ScrollView *form, SettingsWindow &window)
    : Widget(parent), _form(form), _window(window) {
        setFocusable(true);
        reload();
}

PatternChooser::~PatternChooser() {
        clearEntries();
}

void PatternChooser::clearEntries() {
        for (Entry &e : _entries) Memory::free(e.pixels);
        _entries.clear();
        return;
}

void PatternChooser::found(void *arg, StringView name, StringView) {
        auto *self = static_cast<PatternChooser *>(arg);
        if (name.size() >= sizeof(Entry::name) || self->_entries.reserve(self->_entries.size() + 1).isError()) return;
        Entry e;
        std::memcpy(e.name, name.data(), name.size());
        // In name order (a few dozen at most: inserted in place).
        size_t at = self->_entries.size();
        while (at > 0 && std::strcmp(self->_entries[at - 1].name, e.name) > 0) --at;
        (void)self->_entries.insert(at, std::move(e));
        return;
}

void PatternChooser::list() {
        clearEntries();
        (void)PatternFiles::list(_paths.view(), found, this);
        for (Entry &e : _entries) makeSwatch(e);
        return;
}

void PatternChooser::makeSwatch(Entry &e) const {
        Memory::free(e.pixels);
        e.pixels = nullptr;
        String path(MemSpace::Psram);
        if (PatternFiles::find(_paths.view(), StringView(e.name), path).isError()) return;
        PatternFiles::Bytes file;
        TiledPattern        tile;
        const bool          current = _current.view() == StringView(e.name);
        if (file.read(path.view()).isError() ||
            tile.load(file.bytes(), current ? _colors.view() : StringView(), 1, _desktop).isError())
                return;
        const size_t bytes = size_t(SwatchWidth) * SwatchHeight * 3;
        e.pixels = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes));
        if (e.pixels != nullptr) tile.make(0, 0, SwatchWidth, SwatchHeight, e.pixels, size_t(SwatchWidth) * 3);
        return;
}

void PatternChooser::reload() {
        String paths(MemSpace::Psram), current(MemSpace::Psram), colors(MemSpace::Psram);
        read(PatternFiles::PathsKey, paths);
        read(PatternFiles::PatternKey, current);
        read(PatternFiles::ColorsKey, colors);
        const uint32_t desktop = desktopColor();
        const size_t   before = _entries.size();
        const int      oldIndex = indexOf(_current.view());
        const bool     relist = paths.view() != _paths.view() || desktop != _desktop || _entries.isEmpty();
        const bool     recolor = current.view() != _current.view() || colors.view() != _colors.view();
        (void)_paths.assign(paths.view());
        (void)_current.assign(current.view());
        (void)_colors.assign(colors.view());
        _desktop = desktop;
        if (relist) {
                list();
        } else if (recolor) {
                // The last current one back in its own colors, the current one in its colors now.
                if (oldIndex >= 0) makeSwatch(_entries[size_t(oldIndex)]);
                const int now = indexOf(_current.view());
                if (now >= 0 && now != oldIndex) makeSwatch(_entries[size_t(now)]);
        }
        if (!hasFocus()) {
                const int now = indexOf(_current.view());
                _focus = now >= 0 ? now : 0;
        }
        if (_entries.size() != before && _form != nullptr) _form->relayout();
        update();
        return;
}

int PatternChooser::indexOf(StringView name) const {
        for (size_t i = 0; i < _entries.size(); ++i)
                if (StringView(_entries[i].name) == name) return int(i);
        return -1;
}

uint16_t PatternChooser::columns(uint16_t width) const {
        const uint16_t n = uint16_t(width / CellWidth);
        return n > 0 ? n : 1;
}

GuiSize PatternChooser::sizeHint() const {
        return GuiSize{uint16_t(CellWidth * 4), heightForWidth(uint16_t(CellWidth * 4))};
}

uint16_t PatternChooser::heightForWidth(uint16_t width) const {
        const size_t cols = columns(width);
        const size_t rows = _entries.isEmpty() ? 1 : (_entries.size() + cols - 1) / cols;
        return uint16_t(rows * CellHeight);
}

void PatternChooser::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        (void)painter.fill(g, p.color(Role::Face));
        if (_entries.isEmpty()) {
                (void)painter.text((g.x + 3) & ~3, g.y + 4, "No patterns in the folders below", Fonts::unscii16,
                                   p.color(Role::DisabledText), p.color(Role::Face), g.width);
                return;
        }
        const uint16_t cols = columns(g.width);
        const bool     focus = focusShown(*this);
        for (size_t i = 0; i < _entries.size(); ++i) {
                const Entry &e = _entries[i];
                const int    cx = g.x + int(i % cols) * CellWidth, cy = g.y + int(i / cols) * CellHeight;
                const int    sx = cx + (CellWidth - SwatchWidth) / 2, sy = cy + SwatchTop;
                const bool   current = _current.view() == StringView(e.name);
                if (current)
                        ring(painter, sx - 3, sy - 3, SwatchWidth + 6, SwatchHeight + 6, 3, p.color(Role::Highlight));
                else
                        ring(painter, sx - 1, sy - 1, SwatchWidth + 2, SwatchHeight + 2, 1, p.color(Role::FaceShadow));
                if (focus && int(i) == _focus)
                        ring(painter, sx - 5, sy - 5, SwatchWidth + 10, SwatchHeight + 10, 1, p.color(Role::FaceText));
                if (e.pixels != nullptr)
                        (void)painter.pixels(sx, sy, e.pixels, size_t(SwatchWidth) * 3, SwatchWidth, SwatchHeight);
                else
                        (void)painter.fill(Rect{uint16_t(sx), uint16_t(sy), SwatchWidth, SwatchHeight},
                                           p.color(Role::FaceShadow));
                char             label[64];
                const size_t     n = SettingsTable::labelOf(StringView(e.name), Span<char>(label, sizeof(label)));
                const StringView text(label, n);
                const uint32_t   tw = Fonts::unscii16.textWidth(text);
                const int        tx = tw < CellWidth ? cx + int(CellWidth - tw) / 2 : cx;
                (void)painter.text((tx + 3) & ~3, cy + NameTop, text, Fonts::unscii16, p.color(Role::FaceText),
                                   p.color(Role::Face), uint16_t(CellWidth - 4));
        }
        return;
}

void PatternChooser::choose(size_t index) {
        if (index >= _entries.size()) return;
        _focus = int(index);
        update();
        char name[sizeof(Entry::name)];
        std::memcpy(name, _entries[index].name, sizeof(name)); // the list may be made again
        PatternSettings::choose(_window, StringView(name));
        return;
}

void PatternChooser::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        setFocus();
        const Rect &g = geometry();
        if (event.x < g.x || event.y < g.y) return;
        const size_t col = size_t(event.x - g.x) / CellWidth, row = size_t(event.y - g.y) / CellHeight;
        if (col >= columns(g.width)) return;
        choose(row * columns(g.width) + col);
        return;
}

bool PatternChooser::keyPressed(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release || event.hasCtrl() || event.hasAlt() || _entries.isEmpty())
                return false;
        const int cols = columns(geometry().width), last = int(_entries.size()) - 1;
        int       to = _focus;
        switch (event.usage) {
                case HidKey::Left: to = _focus - 1; break;
                case HidKey::Right: to = _focus + 1; break;
                case HidKey::Up: to = _focus - cols; break;
                case HidKey::Down: to = _focus + cols; break;
                case HidKey::Home: to = 0; break;
                case HidKey::End: to = last; break;
                case HidKey::Space:
                case HidKey::Enter:
                case HidKey::KeypadEnter: choose(size_t(_focus)); return true;
                default: return false;
        }
        _focus = to < 0 ? 0 : to > last ? last : to;
        update();
        return true;
}

// ---- PatternColors

PatternColors::PatternColors(Widget *parent, SettingsWindow &window)
    : BoxLayout(parent, Direction::Vertical), _window(window) {}

PatternColors::~PatternColors() {
        _window.forget(this); // a dialog's answer no longer comes here
}

Error PatternColors::build() {
        setSpacing(4);
        _swatchRow = Widget::create<BoxLayout>(this, Direction::Horizontal);
        auto *row = Widget::create<BoxLayout>(this, Direction::Horizontal);
        if (_swatchRow == nullptr || row == nullptr) return Error::NoMem;
        _swatchRow->setSpacing(2);
        row->setSpacing(6);
        for (size_t i = 0; i < MaxShown; ++i) {
                _swatches[i] = Widget::create<ColorSwatch>(_swatchRow);
                if (_swatches[i] == nullptr) return Error::NoMem;
                _swatches[i]->setHint(22, 22);
                _swatches[i]->setData(i);
                _swatches[i]->setClicked(swatchClicked, this);
                _swatches[i]->setVisible(false);
        }
        _fixed = Widget::create<Label>(_swatchRow, "This pattern's colors are its own");
        _choice = Widget::create<ComboBox>(row);
        _saveAs = Widget::create<Button>(row, "Save As...");
        _delete = Widget::create<Button>(row, "Delete");
        if (_fixed == nullptr || _choice == nullptr || _saveAs == nullptr || _delete == nullptr) return Error::NoMem;
        _fixed->setVisible(false);
        _choice->setStretch(1);
        _choice->setChanged(paletteChosen, this);
        _saveAs->setClicked(buttonClicked, this);
        _delete->setClicked(buttonClicked, this);
        reload();
        return Error::Ok;
}

void PatternColors::reload() {
        String colors(MemSpace::Psram), path(MemSpace::Psram), text(MemSpace::Psram);
        read(PatternFiles::PatternKey, _pattern);
        read(PatternFiles::ColorsKey, colors);
        PatternFiles::Bytes file;
        uint32_t            base[TiledPattern::MaxColors] = {};
        _entries = PatternFiles::load(_pattern.view(), file).isOk() ? paletteOf(file.bytes(), base) : 0;
        const size_t shown = _entries < MaxShown ? _entries : MaxShown;
        over(base, shown, colors.view(), _colors);
        for (size_t i = 0; i < MaxShown && _swatches[i] != nullptr; ++i) {
                _swatches[i]->setVisible(i < shown);
                if (i < shown) _swatches[i]->setColor(_colors[i]);
        }
        if (_fixed != nullptr) _fixed->setVisible(shown == 0);

        // The palettes: Original, the built-in ones, the saved ones.
        _palettes.clear();
        if (shown != 0 && _palettes.reserve(MaxPalettes).isOk()) {
                Palette original;
                std::snprintf(original.name, sizeof(original.name), "%s", PatternPalettes::OriginalName);
                (void)_palettes.append(original);
                PatternPalettes::Palette found[MaxPalettes];
                size_t                   builtIn =
                        PatternPalettes::builtIn(file.bytes(), Span<PatternPalettes::Palette>(found, MaxPalettes));
                if (builtIn > MaxPalettes) builtIn = MaxPalettes;
                read(PatternFiles::PalettesKey, path);
                size_t saved = 0;
                if (!path.isEmpty() && PatternFiles::readText(path.view(), text).isOk())
                        saved = PatternPalettes::saved(
                                text.view(), _pattern.view(),
                                Span<PatternPalettes::Palette>(found + builtIn, MaxPalettes - builtIn));
                const size_t total = builtIn + saved < MaxPalettes ? builtIn + saved : MaxPalettes;
                for (size_t i = 0; i < total && _palettes.size() < MaxPalettes; ++i) {
                        Palette p;
                        std::snprintf(p.name, sizeof(p.name), "%.*s", int(found[i].name.size()), found[i].name.data());
                        std::snprintf(p.colors, sizeof(p.colors), "%.*s", int(found[i].colors.size()),
                                      found[i].colors.data());
                        p.saved = i >= builtIn;
                        // A saved one replaces a built-in one of its name.
                        bool replaced = false;
                        for (Palette &q : _palettes)
                                if (p.saved && !q.saved && std::strcmp(q.name, p.name) == 0) {
                                        q = p;
                                        replaced = true;
                                }
                        if (!replaced) (void)_palettes.append(p);
                }
        }
        // The combo: the palette these colors are (the one last chosen or saved, if it still is),
        // else "Unsaved colors" first.
        int match = -1;
        for (size_t i = 0; i < _palettes.size(); ++i) {
                uint32_t mine[MaxShown];
                over(base, shown, i == 0 ? StringView() : StringView(_palettes[i].colors), mine);
                if (std::memcmp(mine, _colors, shown * sizeof(uint32_t)) != 0) continue;
                if (match < 0 || StringView(_palettes[i].name) == _chosen.view()) match = int(i);
        }
        _choice->setChanged(nullptr, nullptr); // set by us: not a choice
        _choice->clear();
        if (match < 0 && shown != 0) (void)_choice->addItem("Unsaved colors", Unsaved);
        for (size_t i = 0; i < _palettes.size(); ++i) {
                char label[PatternPalettes::MaxName + 16];
                std::snprintf(label, sizeof(label), "%s%s", _palettes[i].name, _palettes[i].saved ? " (saved)" : "");
                (void)_choice->addItem(StringView(label), i);
        }
        if (match >= 0) _choice->setCurrent(_choice->indexOfData(uintptr_t(match)));
        _choice->setChanged(paletteChosen, this);
        _choice->setEnabled(shown != 0);
        _saveAs->setEnabled(shown != 0);
        _delete->setEnabled(match >= 0 && _palettes[size_t(match)].saved);
        _swatchRow->relayout();
        return;
}

void PatternColors::colorsWith(size_t entry, uint32_t rgb, String &out) const {
        uint32_t     c[MaxShown];
        const size_t shown = _entries < MaxShown ? _entries : MaxShown;
        for (size_t i = 0; i < shown; ++i) c[i] = i == entry ? rgb : _colors[i];
        (void)format(c, shown, out);
        return;
}

void PatternColors::apply(StringView colors, bool save) {
        const Result<SysConf::Id> id = SysConf::find(StringView(PatternFiles::ColorsKey));
        if (id.isError()) return;
        const Error err = SysConf::set(id.value(), colors);
        if (err.isError()) {
                _window.refused("Pattern colors", id.value(), err);
                return;
        }
        if (!save) return;
        _window.save(id.value(), "Pattern colors");
        const Error kept = PatternSettings::storeCurrent(_pattern.view(), colors);
        if (kept.isError()) {
                char text[96];
                std::snprintf(text, sizeof(text), "The pattern's colors not kept in its palettes: %s", kept.desc());
                _window.say(StringView(text));
        }
        return;
}

void PatternColors::swatchClicked(void *arg, ColorSwatch &swatch) {
        auto *self = static_cast<PatternColors *>(arg);
        self->_asking = swatch.data();
        read(PatternFiles::ColorsKey, self->_before);
        char label[32];
        std::snprintf(label, sizeof(label), "Pattern color %u", unsigned(self->_asking + 1));
        self->_window.chooseColorFor(swatch.color(), StringView(label), colorTaken, self);
        return;
}

void PatternColors::colorTaken(void *arg, uint32_t rgb, bool finished, bool accepted) {
        auto *self = static_cast<PatternColors *>(arg);
        if (finished && !accepted) {
                self->apply(self->_before.view(), false); // put back
                return;
        }
        String colors(MemSpace::Psram);
        self->colorsWith(self->_asking, rgb, colors);
        self->apply(colors.view(), finished); // shown as it changes, saved at OK
        return;
}

void PatternColors::paletteChosen(void *arg, ComboBox &box, int index) {
        auto *self = static_cast<PatternColors *>(arg);
        if (index < 0) return;
        const uintptr_t at = box.itemData(size_t(index));
        if (at == Unsaved || at >= self->_palettes.size()) return;
        // Original: the file's own (none set).
        const Palette &p = self->_palettes[at];
        (void)self->_chosen.assign(StringView(p.name));
        self->apply(at == 0 ? StringView() : StringView(p.colors), true);
        self->reload(); // its buttons now (the same colors change no setting, so nothing tells it)
        return;
}

void PatternColors::buttonClicked(void *arg, Button &button) {
        auto *self = static_cast<PatternColors *>(arg);
        if (&button == self->_saveAs) {
                self->_window.askName("Save Palette As", "Name:", StringView(), nameTaken, self);
                return;
        }
        if (&button != self->_delete || self->_choice->current() < 0) return;
        const uintptr_t at = self->_choice->itemData(size_t(self->_choice->current()));
        if (at == Unsaved || at >= self->_palettes.size() || !self->_palettes[at].saved) return;
        String path(MemSpace::Psram), text(MemSpace::Psram), out(MemSpace::Psram);
        read(PatternFiles::PalettesKey, path);
        Error err = PatternFiles::readText(path.view(), text);
        if (err.isOk())
                err = PatternPalettes::remove(text.view(), self->_pattern.view(), StringView(self->_palettes[at].name),
                                              out);
        if (err.isOk()) err = PatternFiles::writeText(path.view(), out.view());
        char line[96];
        std::snprintf(line, sizeof(line), "Palette %s: %s", self->_palettes[at].name,
                      err.isOk() ? "deleted" : err.desc());
        self->_window.say(StringView(line));
        self->reload();
        return;
}

void PatternColors::nameTaken(void *arg, StringView name) {
        auto *self = static_cast<PatternColors *>(arg);
        char  line[128];
        if (!PatternPalettes::isValidName(name)) {
                std::snprintf(line, sizeof(line),
                              "Not saved: a palette's name is 1 to %u characters, without = [ ] ; or #",
                              unsigned(PatternPalettes::MaxName));
                self->_window.say(StringView(line));
                return;
        }
        String path(MemSpace::Psram), text(MemSpace::Psram), out(MemSpace::Psram), colors(MemSpace::Psram);
        read(PatternFiles::PalettesKey, path);
        const size_t shown = self->_entries < MaxShown ? self->_entries : MaxShown;
        Error        err = path.isEmpty() ? Error(Error::NotExist) : PatternFiles::readText(path.view(), text);
        if (err.isOk()) err = format(self->_colors, shown, colors);
        bool replaced = false;
        if (err.isOk()) {
                PatternPalettes::Palette existing[MaxPalettes];
                const size_t             n = PatternPalettes::saved(text.view(), self->_pattern.view(),
                                                                    Span<PatternPalettes::Palette>(existing, MaxPalettes));
                for (size_t i = 0; i < n && i < MaxPalettes; ++i) replaced = replaced || existing[i].name == name;
                err = PatternPalettes::set(text.view(), self->_pattern.view(), name, colors.view(), out);
        }
        if (err.isOk()) err = PatternFiles::writeText(path.view(), out.view());
        std::snprintf(line, sizeof(line), "Palette %.*s: %s", int(name.size()), name.data(),
                      err.isError() ? err.desc()
                      : replaced    ? "replaced"
                                    : "saved");
        if (err.isOk()) (void)self->_chosen.assign(name);
        self->_window.say(StringView(line));
        self->reload();
        return;
}

BMDS_NAMESPACE_END
