/**
 * @file      settingswindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Settings' window (devplan/controlpanel.md §4): the groups and the search, the page, the status;
 * saving, refusals, the Keep dialog, the folder, file and color dialogs, the registry followed.
 */

#include <cstdio>
#include <cstring>
#include <bmds/detailsview.h>
#include <bmds/formlayout.h>
#include <bmds/formmodels.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/icontheme.h>
#include <bmds/lineedit.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

#include "pagestatus.h"
#include "settingspage.h"
#include "settingswindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        namespace Conf = ::bmds::Config; // GuiWindow has a Config of its own

        constexpr uint16_t SidebarWidth = 224;
        constexpr int      ConfirmSeconds = 15;
        constexpr size_t   MaxHits = 100;

        size_t allPage() {
                return SettingsTable::pages().size() - 1;
        }

        StringView pageTitle(size_t page) {
                return StringView(SettingsTable::pages()[page].title);
        }

} // namespace

// ---- OwnedIcon

void SettingsWindow::OwnedIcon::take(const Icon *from) {
        if (from == nullptr || !from->isValid() || pixels != nullptr) return;
        const size_t n = size_t(from->width) * from->height;
        pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
        if (pixels == nullptr) return;
        std::memcpy(pixels, from->pixels, n * sizeof(uint32_t));
        icon = Icon{from->width, from->height, pixels};
        return;
}

SettingsWindow::OwnedIcon::~OwnedIcon() {
        Memory::free(pixels);
}

// ---- SettingsWindow

SettingsWindow::SettingsWindow(GuiApplication &app, const Config &config, const BuiltinProgram::Context &program)
    : GuiWindow(app, config), _folderDialog(app, FileDialog::Mode::Folder, "Choose a Folder"),
      _fileDialog(app, FileDialog::Mode::Open, "Choose a File"), _colorDialog(app, "Color"),
      _confirm(app, "Keep This Setting?", 320, 48), _zoneDialog(app), _passwordDialog(app), _nameDialog(app),
      _program(program) {}

SettingsWindow::~SettingsWindow() {
        if (_confirmTimer >= 0) application().stopTimer(_confirmTimer);
        if (_tickTimer >= 0) application().stopTimer(_tickTimer);
        application().cancelPosts(this);
        destroyPage();
}

Error SettingsWindow::build() {
        auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (column == nullptr) return Error::NoMem;
        setCentral(column);
        auto *split = Widget::create<Splitter>(column, Splitter::Orientation::Horizontal);
        _status = Widget::create<StatusBar>(column);
        if (split == nullptr || _status == nullptr) return Error::NoMem;
        split->setStretch(1);
        (void)_status->addField(0);
        auto *side = Widget::create<BoxLayout>(split, BoxLayout::Direction::Vertical);
        _host = Widget::create<BoxLayout>(split, BoxLayout::Direction::Vertical);
        if (side == nullptr || _host == nullptr) return Error::NoMem;
        side->setMargins(4, 4, 4, 4);
        side->setSpacing(4);
        _search = Widget::create<LineEdit>(side);
        _sidebar = Widget::create<DetailsView>(side);
        if (_search == nullptr || _sidebar == nullptr) return Error::NoMem;
        _search->setEdited(searchEdited, this);
        _search->setAccepted(searchDone, this);
        _search->setCancelled(searchDone, this);
        _sidebar->setStretch(1);
        _sidebar->setHeaderVisible(false);
        _sidebar->setSingleSelection(true);
        _sidebar->setChanged(sidebarChanged, this);
        _sidebar->setActivated(sidebarActivated, this);
        split->setLeast(160, 320);
        split->setPosition(SidebarWidth);
        split->setWidgets(side, _host);

        // The groups, each with its icon (copied: the theme's cache may drop its own).
        IconTheme                            &theme = application().icons();
        const Span<const SettingsTable::Page> pages = SettingsTable::pages();
        const StringView                      none[1] = {StringView()};
        const uint16_t                        widths[1] = {SidebarWidth - 32};
        const ItemModel::Align                aligns[1] = {ItemModel::Align::Left};
        for (ListItemModel *m : {&_groups, &_results})
                m->setColumns(Span<const StringView>(none), Span<const uint16_t>(widths),
                              Span<const ItemModel::Align>(aligns));
        for (size_t i = 0; i < pages.size() && i < sizeof(_icons) / sizeof(_icons[0]); ++i) {
                _icons[i].take(theme.find(StringView(pages[i].icon), 16));
                const Icon *icon = _icons[i].pixels != nullptr ? &_icons[i].icon : nullptr;
                if (Error e = _groups.add(StringView(pages[i].title), icon, icon); e.isError()) return e;
        }

        // Its own icon: General's (the theme's preferences-system), and a large one for Alt+Tab.
        if (_icons[0].pixels != nullptr) setIcon(&_icons[0].icon);
        _largeIcon.take(theme.find(StringView(pages[0].icon), 32));
        if (_largeIcon.pixels != nullptr) setLargeIcon(&_largeIcon.icon);

        // The Keep dialog.
        _confirmText = Widget::create<TextBlock>(_confirm.content());
        if (_confirmText == nullptr) return Error::NoMem;
        _confirmText->setStretch(1);
        (void)_confirm.addButton("&Keep", 1);
        (void)_confirm.addButton("&Revert", 0);
        _confirm.setCancelResult(0);
        _confirm.setFinished(confirmDone, this);
        _folderDialog.setFinished(pathChosen, this);
        _fileDialog.setFinished(pathChosen, this);
        _colorDialog.setChanged(colorChanged, this);
        _colorDialog.setFinished(colorDone, this);
        if (Error e = _zoneDialog.build(); e.isError()) return e;
        if (Error e = _passwordDialog.build(); e.isError()) return e;
        _zoneDialog.setFinished(zoneDone, this);
        _passwordDialog.setFinished(passwordDone, this);
        if (Error e = _nameDialog.build(); e.isError()) return e;
        _nameDialog.setFinished(nameDone, this);

        if (Error e = application().watchConfig(StringView(), configChanged, this); e.isError()) return e;
        showGroups(0);
        showPage(0);
        return Error::Ok;
}

void SettingsWindow::showGroups(int select) {
        _syncing = true;
        _sidebar->setModel(&_groups);
        if (select >= 0) _sidebar->selectOnly(select);
        _syncing = false;
        return;
}

void SettingsWindow::destroyPage() {
        if (_page != nullptr) _page->destroy();
        if (_all != nullptr) _all->destroy();
        _page = nullptr;
        _all = nullptr;
        return;
}

void SettingsWindow::showPage(size_t page) {
        const Span<const SettingsTable::Page> pages = SettingsTable::pages();
        if (page >= pages.size()) return;
        if (page == _current && (_page != nullptr || _all != nullptr)) return;
        destroyPage();
        _current = page;
        Error err = Error::NoMem;
        if (page == allPage()) {
                _all = Widget::create<AllSettingsPage>(_host, *this);
                if (_all != nullptr) {
                        _all->setStretch(1);
                        err = _all->build();
                }
        } else {
                _page = Widget::create<SettingsPage>(_host, *this, page);
                if (_page != nullptr) {
                        _page->setStretch(1);
                        err = _page->build();
                }
        }
        _host->relayout();
        if (err.isError()) say("Not enough memory for the page");
        // A status that changes by itself is read each second while the page shows.
        const bool ticking = PageStatus::ticks(StringView(pages[page].id));
        if (ticking && _tickTimer < 0) {
                auto [timer, terr] = application().startTimer(1000, true, tick, this);
                _tickTimer = terr.isOk() ? timer : -1;
        } else if (!ticking && _tickTimer >= 0) {
                application().stopTimer(_tickTimer);
                _tickTimer = -1;
        }
        if (!_searching) {
                _syncing = true;
                if (_sidebar->current() != int(page)) _sidebar->selectOnly(int(page));
                _syncing = false;
        }
        return;
}

void SettingsWindow::raise(int page) {
        if (!isShown()) (void)show();
        activate();
        if (page >= 0) {
                if (_searching) {
                        _search->setText(StringView());
                        _searching = false;
                        showGroups(page);
                }
                showPage(size_t(page));
        }
        return;
}

void SettingsWindow::say(StringView text) {
        if (_status != nullptr) _status->setText(0, text);
        return;
}

void SettingsWindow::refused(StringView label, SysConf::Id id, Error err) {
        SysConf::Info info;
        const bool    known = SysConf::info(id, info).isOk() && info.param != nullptr;
        char          text[160];
        if (err == Error::OutOfRange && known && info.param->type == Conf::Type::Int) {
                std::snprintf(text, sizeof(text), "%.*s: not taken: from %ld to %ld", int(label.size()), label.data(),
                              long(info.param->min), long(info.param->max));
        } else if (err == Error::OutOfRange && known && info.param->max > 0) {
                std::snprintf(text, sizeof(text), "%.*s: not taken: %ld bytes at most", int(label.size()), label.data(),
                              long(info.param->max));
        } else if (err == Error::Invalid) {
                std::snprintf(text, sizeof(text), "%.*s: not taken: not a valid %s", int(label.size()), label.data(),
                              known ? Conf::typeName(info.param->type) : "value");
        } else {
                std::snprintf(text, sizeof(text), "%.*s: not taken: %s", int(label.size()), label.data(), err.desc());
        }
        say(StringView(text));
        return;
}

void SettingsWindow::save(SysConf::Id id, StringView label) {
        const Error err = SysConf::save(id);
        char        text[160];
        if (err.isError()) {
                std::snprintf(text, sizeof(text), "%.*s: applied, not saved: %s", int(label.size()), label.data(),
                              err.desc());
        } else {
                std::snprintf(text, sizeof(text), "%.*s: saved", int(label.size()), label.data());
        }
        say(StringView(text));
        return;
}

// ---- The Keep dialog (§6.3)

void SettingsWindow::confirm(SysConf::Id id, StringView before, StringView label) {
        if (_confirmId != SysConf::NoId) {
                // One asked at a time: the one before is kept as it is, unsaved.
                _confirm.finish(-1);
        }
        if (_confirmBefore.assign(before).isError()) {
                save(id, label); // no room to put it back: saved as any other
                return;
        }
        _confirmId = id;
        std::snprintf(_confirmLabel, sizeof(_confirmLabel), "%.*s", int(label.size()), label.data());
        _confirmLeft = ConfirmSeconds;
        confirmShown();
        auto [timer, err] = application().startTimer(1000, true, confirmTick, this);
        _confirmTimer = err.isOk() ? timer : -1;
        if (_confirm.open().isError()) _confirm.finish(0);
        return;
}

void SettingsWindow::confirmShown() {
        char text[200];
        std::snprintf(text, sizeof(text), "%s changed. Keep it? Otherwise it is put back in %d s.", _confirmLabel,
                      _confirmLeft);
        _confirmText->setText(StringView(text));
        return;
}

void SettingsWindow::confirmTick(void *arg) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (--self->_confirmLeft <= 0) {
                self->_confirm.finish(0);
                return;
        }
        self->confirmShown();
        return;
}

void SettingsWindow::confirmDone(void *arg, Dialog &, int result) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (self->_confirmTimer >= 0) self->application().stopTimer(self->_confirmTimer);
        self->_confirmTimer = -1;
        const SysConf::Id id = self->_confirmId;
        self->_confirmId = SysConf::NoId;
        if (id == SysConf::NoId || result < 0) return;
        if (result == 1) {
                self->save(id, StringView(self->_confirmLabel));
                return;
        }
        const Error err = SysConf::set(id, self->_confirmBefore.view());
        char        text[120];
        std::snprintf(text, sizeof(text), "%s: put back%s", self->_confirmLabel, err.isError() ? " (failed)" : "");
        self->say(StringView(text));
        return;
}

// ---- The dialogs

void SettingsWindow::choosePath(SysConf::Id id, bool folder, StringView label) {
        _pathId = id;
        _pathAdds = false;
        std::snprintf(_pathLabel, sizeof(_pathLabel), "%.*s", int(label.size()), label.data());
        FileDialog &d = folder ? _folderDialog : _fileDialog;
        String      value(MemSpace::Psram);
        if (SysConf::get(id, value).isOk() && value.view().startsWith('/')) {
                // Opens where it is: the folder itself, or a file's folder.
                const StringView v = value.view();
                const size_t     slash = v.lastIndexOf('/');
                (void)d.setFolder(folder ? v : (slash > 0 ? v.left(slash) : StringView("/")));
        }
        if (d.open().isError()) say("The dialog can't open");
        return;
}

void SettingsWindow::addFolder(SysConf::Id id, StringView label) {
        _pathId = id;
        _pathAdds = true;
        std::snprintf(_pathLabel, sizeof(_pathLabel), "%.*s", int(label.size()), label.data());
        if (_folderDialog.open().isError()) say("The dialog can't open");
        return;
}

void SettingsWindow::pathChosen(void *arg, FileDialog &dialog, int result) {
        auto             *self = static_cast<SettingsWindow *>(arg);
        const SysConf::Id id = self->_pathId;
        self->_pathId = SysConf::NoId;
        if (result != FileDialog::Accepted || id == SysConf::NoId || dialog.count() == 0) return;
        const StringView label(self->_pathLabel);
        String           next(MemSpace::Psram);
        if (self->_pathAdds) {
                String now(MemSpace::Psram);
                if (SysConf::get(id, now).isError() ||
                    ListText::insert(now.view(), ';', ListText::count(now.view(), ';'), dialog.path(0), next).isError())
                        return;
        } else if (next.assign(dialog.path(0)).isError()) {
                return;
        }
        const Error err = SysConf::set(id, next.view());
        if (err.isError()) {
                self->refused(label, id, err);
                return;
        }
        self->save(id, label);
        return;
}

void SettingsWindow::chooseColor(SysConf::Id id, StringView label) {
        String value(MemSpace::Psram);
        if (SysConf::get(id, value).isError()) return;
        _colorFn = nullptr;
        _colorId = id;
        std::snprintf(_colorLabel, sizeof(_colorLabel), "%.*s", int(label.size()), label.data());
        if (_colorDialog.open(Conf::toColor(value.view())).isError()) {
                _colorId = SysConf::NoId;
                say("The dialog can't open");
        }
        return;
}

void SettingsWindow::chooseColorFor(uint32_t rgb, StringView label, ColorTaken fn, void *arg) {
        _colorId = SysConf::NoId;
        _colorFn = fn;
        _colorArg = arg;
        std::snprintf(_colorLabel, sizeof(_colorLabel), "%.*s", int(label.size()), label.data());
        if (_colorDialog.open(rgb).isError()) {
                _colorFn = nullptr;
                say("The dialog can't open");
        }
        return;
}

void SettingsWindow::askName(StringView title, StringView prompt, StringView initial, NameTaken fn, void *arg) {
        _nameFn = fn;
        _nameArg = arg;
        if (_nameDialog.open(title, prompt, initial).isError()) {
                _nameFn = nullptr;
                say("The dialog can't open");
        }
        return;
}

void SettingsWindow::forget(void *arg) {
        if (_colorArg == arg) _colorFn = nullptr;
        if (_nameArg == arg) _nameFn = nullptr;
        return;
}

void SettingsWindow::nameDone(void *arg, Dialog &, int result) {
        auto     *self = static_cast<SettingsWindow *>(arg);
        NameTaken fn = self->_nameFn;
        self->_nameFn = nullptr;
        if (fn != nullptr && result == NameDialog::Accepted) fn(self->_nameArg, self->_nameDialog.name());
        return;
}

void SettingsWindow::colorChanged(void *arg, ColorDialog &, uint32_t rgb) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (self->_colorFn != nullptr) {
                self->_colorFn(self->_colorArg, rgb, false, false);
                return;
        }
        if (self->_colorId == SysConf::NoId) return;
        char hex[8];
        ColorModel::formatHex(rgb, hex);
        (void)SysConf::set(self->_colorId, StringView(hex + 1)); // live, saved at OK
        return;
}

void SettingsWindow::colorDone(void *arg, Dialog &, int result) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (self->_colorFn != nullptr) {
                ColorTaken fn = self->_colorFn;
                self->_colorFn = nullptr;
                const bool ok = result == ColorDialog::Accepted;
                fn(self->_colorArg, ok ? self->_colorDialog.color() : self->_colorDialog.originalColor(), true, ok);
                return;
        }
        const SysConf::Id id = self->_colorId;
        self->_colorId = SysConf::NoId;
        if (id == SysConf::NoId) return;
        if (result == ColorDialog::Accepted) {
                self->save(id, StringView(self->_colorLabel));
                return;
        }
        char hex[8];
        ColorModel::formatHex(self->_colorDialog.originalColor(), hex);
        (void)SysConf::set(id, StringView(hex + 1));
        return;
}

void SettingsWindow::chooseZone(SysConf::Id id, bool allowEmpty, StringView label) {
        String value(MemSpace::Psram);
        if (SysConf::get(id, value).isError()) return;
        _zoneId = id;
        std::snprintf(_zoneLabel, sizeof(_zoneLabel), "%.*s", int(label.size()), label.data());
        if (_zoneDialog.open(value.view(), allowEmpty).isError()) {
                _zoneId = SysConf::NoId;
                say("The dialog can't open");
        }
        return;
}

void SettingsWindow::zoneDone(void *arg, Dialog &, int result) {
        auto             *self = static_cast<SettingsWindow *>(arg);
        const SysConf::Id id = self->_zoneId;
        self->_zoneId = SysConf::NoId;
        if (result != ZoneDialog::Accepted || id == SysConf::NoId || !self->_zoneDialog.hasChoice()) return;
        const StringView label(self->_zoneLabel);
        const Error      err = SysConf::set(id, self->_zoneDialog.zone());
        if (err.isError()) {
                self->refused(label, id, err);
                return;
        }
        self->save(id, label);
        return;
}

void SettingsWindow::setPassword() {
        if (_passwordDialog.open().isError()) say("The dialog can't open");
        return;
}

void SettingsWindow::passwordDone(void *arg, Dialog &, int result) {
        auto       *self = static_cast<SettingsWindow *>(arg);
        const char *problem = nullptr;
        if (result != 1) {
                (void)self->_passwordDialog.take(problem); // emptied, not taken
                return;
        }
        char text[96];
        if (self->_passwordDialog.take(problem).isError()) {
                std::snprintf(text, sizeof(text), "Password: %s", problem != nullptr ? problem : "unchanged");
        } else {
                std::snprintf(text, sizeof(text), "Password: set and saved");
        }
        self->say(StringView(text));
        return;
}

void SettingsWindow::run(StringView command) {
        // The program and its arguments; found on PATH, started as Settings' own child, let go.
        StringView args[8];
        size_t     n = 0;
        StringView rest = command;
        while (!rest.isEmpty() && n < sizeof(args) / sizeof(args[0])) {
                const size_t space = rest.indexOf(' ');
                if (space != 0) args[n++] = rest.left(space);
                if (space == StringView::NPos) break;
                rest = rest.mid(space + 1);
        }
        if (n == 0 || _program.launcher == nullptr) return;
        StringView searchPath;
        for (const StringView &v : _program.environment)
                if (v.startsWith(StringView("PATH="))) searchPath = v.mid(5);
        String path(MemSpace::Psram);
        Error  err = Vfs::findProgram(args[0], searchPath, Vfs::Context{_program.cwd, nullptr}, path);
        if (err.isOk()) {
                ProgramLauncher::Request request;
                request.path = path.view();
                request.args = Span<const StringView>(args, n);
                request.environment = _program.environment;
                request.fds = _program.fds;
                request.cwd = _program.cwd;
                request.console = _program.console;
                request.session = true; // its own: Settings' terminal's signals aren't its
                auto [pid, serr] = _program.launcher->start(request);
                err = serr;
                if (err.isOk() && _program.process != nullptr) _program.process->disownChild(pid);
        }
        if (err.isError()) {
                char text[96];
                std::snprintf(text, sizeof(text), "%.*s: can't start it: %s", int(args[0].size()), args[0].data(),
                              err.desc());
                say(StringView(text));
        }
        return;
}

void SettingsWindow::tick(void *arg) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (self->_page != nullptr && self->isShown()) self->_page->tick();
        return;
}

// ---- The sidebar and the search

void SettingsWindow::sidebarChanged(void *arg, ItemView &view) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (self->_syncing) return;
        const int cur = view.current();
        if (cur < 0) return;
        if (!self->_searching) {
                self->showPage(size_t(cur));
                return;
        }
        if (size_t(cur) >= self->_hits.size()) return;
        const Hit hit = self->_hits[size_t(cur)];
        self->showPage(hit.page);
        if (hit.key[0] == '\0') return;
        // Shown and lit; the keys stay with the matches (Enter gives them to the setting).
        if (self->_page != nullptr) (void)self->_page->reveal(StringView(hit.key), false);
        if (self->_all != nullptr) (void)self->_all->reveal(StringView(hit.key), false);
        return;
}

void SettingsWindow::sidebarActivated(void *arg, ItemView &view, size_t row) {
        auto *self = static_cast<SettingsWindow *>(arg);
        (void)view;
        if (!self->_searching || row >= self->_hits.size() || self->_hits[row].key[0] == '\0') return;
        if (self->_page != nullptr) (void)self->_page->reveal(StringView(self->_hits[row].key), true);
        if (self->_all != nullptr) (void)self->_all->reveal(StringView(self->_hits[row].key), true);
        return;
}

void SettingsWindow::searchEdited(void *arg, LineEdit &edit) {
        static_cast<SettingsWindow *>(arg)->search(edit.text());
        return;
}

void SettingsWindow::searchDone(void *arg, LineEdit &edit) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (!self->_searching) return;
        if (edit.text().isEmpty() || self->_hits.isEmpty()) {
                edit.setText(StringView());
                self->search(StringView());
                return;
        }
        // Enter: the first match, and the keys to the list to go on (Enter again: to the setting).
        self->_sidebar->selectOnly(0);
        self->_sidebar->setFocus();
        return;
}

void SettingsWindow::search(StringView text) {
        if (text.isEmpty()) {
                _searching = false;
                showGroups(int(_current));
                return;
        }
        _searching = true;
        _syncing = true;
        _results.clear();
        _hits.clear();
        const Span<const SettingsTable::Page> pages = SettingsTable::pages();
        // The pages whose titles match, then each setting matching by label, section, key or help.
        for (size_t i = 0; i < pages.size(); ++i) {
                if (!SettingsTable::containsFolded(StringView(pages[i].title), text)) continue;
                Hit hit;
                hit.page = uint8_t(i);
                const Icon *icon = _icons[i].pixels != nullptr ? &_icons[i].icon : nullptr;
                if (_hits.append(hit).isOk()) (void)_results.add(pageTitle(i), icon, icon);
        }
        SysConf::Id after = SysConf::NoId;
        while (_hits.size() < MaxHits) {
                const Result<SysConf::Id> next = SysConf::next(StringView(), after);
                if (next.isError()) break;
                after = next.value();
                const StringView           key = SysConf::keyOf(after);
                const SettingsTable::Place at = SettingsTable::place(key);
                SysConf::Info              info;
                if (SysConf::info(after, info).isError() || info.param == nullptr) continue;
                char         label[64];
                const size_t n =
                        at.row != nullptr ? SettingsTable::label(*at.row, key, Span<char>(label, sizeof(label))) : 0;
                const StringView l(label, n);
                const StringView help(at.row != nullptr && at.row->help != nullptr ? at.row->help : info.param->help);
                const bool       matched =
                        SettingsTable::containsFolded(l, text) || SettingsTable::containsFolded(key, text) ||
                        SettingsTable::containsFolded(help, text) ||
                        (at.section != nullptr && SettingsTable::containsFolded(StringView(at.section->label), text));
                if (!matched) continue;
                Hit hit;
                hit.page = uint8_t(at.page >= 0 ? size_t(at.page) : allPage());
                std::snprintf(hit.key, sizeof(hit.key), "%.*s", int(key.size()), key.data());
                // Its label, with its page's icon (the key for one no page places).
                const Icon *icon = _icons[hit.page].pixels != nullptr ? &_icons[hit.page].icon : nullptr;
                if (_hits.append(hit).isError()) break;
                (void)_results.add(n > 0 ? l : key, icon, icon);
        }
        _sidebar->setModel(&_results);
        _syncing = false;
        char text2[48];
        std::snprintf(text2, sizeof(text2), "%zu found", _hits.size());
        say(StringView(text2));
        return;
}

// ---- The registry followed (§6.7)

void SettingsWindow::configChanged(void *arg, SysConf::Id id) {
        auto *self = static_cast<SettingsWindow *>(arg);
        if (self->_all != nullptr) self->_all->changed(id);
        if (self->_page != nullptr && !self->_page->changed(id) && !self->_rebuildPosted) {
                // Made again after this call: not under its own widgets' feet.
                self->_rebuildPosted = self->application().post(rebuildLater, self).isOk();
        }
        return;
}

void SettingsWindow::rebuildLater(void *arg) {
        auto *self = static_cast<SettingsWindow *>(arg);
        self->_rebuildPosted = false;
        const size_t page = self->_current;
        self->destroyPage();
        self->showPage(page);
        return;
}

void SettingsWindow::closedByManager() {
        application().quit(0);
        return;
}

bool SettingsWindow::keyUnused(const KeyEvent &event) {
        if (event.hasCtrl() && !event.hasAlt() && event.usage == HidKey::A + ('f' - 'a')) {
                _search->setFocus();
                _search->selectAll();
                return true;
        }
        return false;
}

BMDS_NAMESPACE_END
