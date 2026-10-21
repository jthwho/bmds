/**
 * @file      filedialog.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Asks for files to open, a name to save as, or a folder (devplan/mediaplayer.md §6d).
 */

#include <cstdio>
#include <cstring>
#include <bmds/directorywatch.h>
#include <bmds/filedialog.h>
#include <bmds/filenamelist.h>
#include <bmds/guiapplication.h>
#include <bmds/inputevent.h>
#include <bmds/namefilter.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t   Width = 560;
        constexpr uint16_t   Height = 320;
        constexpr uint16_t   LabelWidth = 128; // "Files of type:" and the rest, one column
        constexpr uint16_t   PlacesWidth = 120;
        constexpr size_t     FillBatch = 64; // entries read a turn (FB12)
        constexpr StringView Home("/home");

        const Vfs::Context Root{"/", nullptr};

        // A row of @p parent's: widgets side by side.
        BoxLayout *row(Widget *parent) {
                auto *r = Widget::create<BoxLayout>(parent, BoxLayout::Direction::Horizontal);
                if (r != nullptr) r->setSpacing(8);
                return r;
        }

        // @p folder and @p name as one path.
        Error join(StringView folder, StringView name, String &out) {
                Error err = out.assign(folder);
                if (err.isOk() && !folder.endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(name);
                return err;
        }

} // namespace

FileDialog::FileDialog(GuiApplication &app, Mode mode, StringView title)
    : Dialog(app, title, Width, Height), _mode(mode), _icons(app.icons()) {
        _icons.setAsked(iconsAsked, this);
        Dialog::setFinished(finished, this);
        setCancelResult(Rejected);
        (void)build();
}

FileDialog::~FileDialog() {
        stopReading();
        application().cancelPosts(this);
}

Error FileDialog::build() {
        BoxLayout *c = content();
        if (c == nullptr) return Error::NoMem;
        // Look in: the path, Back, Up.
        BoxLayout *top = row(c);
        if (top == nullptr) return Error::NoMem;
        auto *lookLabel = Widget::create<Label>(top, "Look &in:");
        _lookIn = Widget::create<LineEdit>(top);
        _back = Widget::create<Button>(top, "&Back");
        _up = Widget::create<Button>(top, "&Up");
        if (lookLabel == nullptr || _lookIn == nullptr || _back == nullptr || _up == nullptr) return Error::NoMem;
        lookLabel->setMinimumWidth(LabelWidth);
        lookLabel->setBuddy(_lookIn);
        _lookIn->setStretch(1);
        _lookIn->setAccepted(lookInAccepted, this);
        _lookIn->setCancelled(lookInCancelled, this);
        _back->setClicked(backClicked, this);
        _up->setClicked(upClicked, this);
        _back->setFocusable(false); // Alt+B, Alt+U; Tab goes between the fields and the list
        _up->setFocusable(false);

        // The places, then the folder.
        BoxLayout *middle = row(c);
        if (middle == nullptr) return Error::NoMem;
        middle->setStretch(1);
        auto *places = Widget::create<BoxLayout>(middle, BoxLayout::Direction::Vertical);
        _view = Widget::create<DetailsView>(middle);
        if (places == nullptr || _view == nullptr) return Error::NoMem;
        places->setMaximumWidth(PlacesWidth);
        places->setStretch(0);
        for (Place &p : _places) {
                p.button = Widget::create<Button>(places);
                if (p.button == nullptr) return Error::NoMem;
                p.button->setLook(Button::Look::Flat);
                p.button->setFocusable(false);
                p.button->setClicked(placeClicked, this);
                p.button->setVisible(false);
        }
        _view->setStretch(1);
        _view->setModel(&_items);
        _view->setFocusable(true);
        _view->setActivated(activated, this);
        _view->setChanged(selectionChanged, this);
        _view->setKeyTyped(viewKey, this);
        _view->setSortAsked(sortAsked, this);
        _view->setSortIndicator(0, false);
        _view->setSingleSelection(_mode != Mode::OpenMany);

        // File name; Files of type (not for a folder).
        BoxLayout *nameRow = row(c);
        if (nameRow == nullptr) return Error::NoMem;
        auto *nameLabel = Widget::create<Label>(nameRow, _mode == Mode::Folder ? "Folder &name:" : "File &name:");
        _name = Widget::create<LineEdit>(nameRow);
        if (nameLabel == nullptr || _name == nullptr) return Error::NoMem;
        nameLabel->setMinimumWidth(LabelWidth);
        nameLabel->setBuddy(_name);
        _name->setStretch(1);
        _name->setAccepted(nameAccepted, this);
        _name->setCancelled(nameCancelled, this);
        BoxLayout *typeRow = row(c);
        if (typeRow == nullptr) return Error::NoMem;
        auto *typeLabel = Widget::create<Label>(typeRow, "Files of &type:");
        _type = Widget::create<ComboBox>(typeRow);
        if (typeLabel == nullptr || _type == nullptr) return Error::NoMem;
        typeLabel->setMinimumWidth(LabelWidth);
        typeLabel->setBuddy(_type);
        _type->setStretch(1);
        _type->setChanged(typeChanged, this);
        typeRow->setVisible(_mode != Mode::Folder);

        const char *verb = _mode == Mode::Save ? "&Save" : _mode == Mode::Folder ? "&Choose" : "&Open";
        _accept = addButton(verb, Accepted);
        (void)addButton("Cancel", Rejected);
        if (_accept == nullptr) return Error::NoMem;
        _accept->setClicked(acceptClicked, this); // checked first, then finished
        setFocusWidget(_name);
        return Error::Ok;
}

Error FileDialog::addFilter(StringView name, StringView patterns) {
        if (_filterCount == MaxFilters || _type == nullptr) return Error::Full;
        Filter      &f = _filters[_filterCount];
        const size_t pn = patterns.size() < sizeof(f.patterns) - 1 ? patterns.size() : sizeof(f.patterns) - 1;
        std::memcpy(f.patterns, patterns.data(), pn);
        f.patterns[pn] = '\0';
        Error err = _type->addItem(name, _filterCount);
        if (err.isError()) return err;
        const StringView shown = _type->itemText(_filterCount);
        std::memcpy(f.name, shown.data(), shown.size());
        f.name[shown.size()] = '\0';
        ++_filterCount;
        return Error::Ok;
}

Error FileDialog::setFolder(StringView path) {
        return _start.assign(path);
}

Error FileDialog::setFileName(StringView name) {
        if (_name == nullptr) return Error::NoMem;
        _name->setText(name);
        return Error::Ok;
}

Error FileDialog::open() {
        if (_view == nullptr || _accept == nullptr) return Error::NoMem;
        _paths.clear();
        _asking = false;
        setPlaces();
        // Where it starts: the folder asked for, else the last one, else home (or /).
        String from(MemSpace::Psram);
        Error  err = from.assign(!_start.isEmpty()              ? _start.view()
                                 : !folder().isEmpty()          ? folder()
                                 : Vfs::stat(Home, Root).isOk() ? Home
                                                                : StringView("/"));
        if (err.isError()) return err;
        _start.clear();
        _history.clear();
        if (go(from.view(), StringView()).isError() && go("/", StringView()).isError()) return Error::NotExist;
        applyFilter();
        err = Dialog::open();
        if (err.isOk()) {
                setFocusWidget(_name);
                _name->selectAll();
        }
        return err;
}

// The home folder, each filesystem, and / (again at each opening: cards come and go).
void FileDialog::setPlaces() {
        Vfs::MountInfo mounts[Vfs::MaxMounts];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
        size_t         k = 0;
        const auto     add = [&](StringView label, StringView path, IconId icon) {
                if (k == MaxPlaces) return;
                Place       &p = _places[k++];
                const size_t len = path.size() < sizeof(p.path) - 1 ? path.size() : sizeof(p.path) - 1;
                std::memcpy(p.path, path.data(), len);
                p.path[len] = '\0';
                p.button->setText(label);
                p.button->setIcon(_icons.named(icon, 16));
                p.button->setVisible(true);
        };
        if (Vfs::stat(Home, Root).isOk()) add("Home", Home, IconId::UserHome);
        for (size_t i = 0; i < n; ++i)
                add(StringView(mounts[i].path), StringView(mounts[i].path),
                    mounts[i].network                     ? IconId::FolderRemote
                    : StringView(mounts[i].type) == "rom" ? IconId::MediaFlash
                                                          : IconId::DriveHarddisk);
        add("/", "/", IconId::Computer);
        for (; k < MaxPlaces; ++k) _places[k].button->setVisible(false);
        static_cast<BoxLayout *>(_places[0].button->parent())->relayout();
        return;
}

// ---- Going to a folder

Error FileDialog::go(StringView path, StringView select) {
        const StringView here = folder().isEmpty() ? StringView("/") : folder();
        auto [abs, nerr] = Vfs::normalize(path.trimmed(), here);
        if (nerr.isError()) return nerr;
        auto [info, serr] = Vfs::stat(abs.view(), Root);
        if (serr.isError()) return serr;
        if (info.type != FileType::Directory) {
                // A file: its folder, the file selected.
                const StringView p = abs.view();
                const size_t     slash = p.lastIndexOf('/');
                return go(slash == 0 ? StringView("/") : p.left(slash), p.mid(slash + 1));
        }
        stopReading();
        FolderModel &f = _items.folder();
        const Error  err = f.open(abs.view(), Root);
        // The view told at once: its rows (and current row) were the last folder's.
        _view->resetTo(Span<const size_t>(), -1);
        if (err.isError()) return err;
        (void)_history.visit(f.path());
        if (_watched.view() != f.path()) {
                if (!_watched.isEmpty()) _watches.remove(_watched.view());
                _watched.clear();
                if (_watches.add(f.path()).isOk()) (void)_watched.assign(f.path());
        }
        (void)_select.assign(select);
        _lookIn->setText(f.path());
        // Names the last folder's selection put there (or any, choosing a folder) name nothing here.
        // (A name edited since is the user's, kept.)
        if (_mode == Mode::Folder || (!_selectionNames.isEmpty() && _name->text() == _selectionNames.view()))
                _name->setText(StringView());
        _selectionNames.clear();
        updateButtons();
        startReading();
        return Error::Ok;
}

void FileDialog::reload() {
        if (folder().isEmpty()) return;
        // The current row's name, found again once read.
        const int row = _view->current();
        if (row >= 0 && size_t(row) < _items.folder().count()) (void)_select.assign(_items.folder().name(size_t(row)));
        String path(MemSpace::Psram);
        if (path.assign(folder()).isError()) return;
        stopReading();
        const Error err = _items.folder().open(path.view(), Root);
        _view->resetTo(Span<const size_t>(), -1); // emptied: told at once
        if (err.isOk()) startReading();
        return;
}

void FileDialog::updateButtons() {
        _back->setEnabled(_history.canGoBack());
        _up->setEnabled(folder() != "/");
        return;
}

void FileDialog::startReading() {
        auto [timer, err] = application().startTimer(1, true, fillTick, this);
        _fillTimer = err.isOk() ? timer : -1;
        if (_fillTimer < 0) {
                // No timer: all of it now.
                while (!_items.folder().isComplete()) (void)_items.folder().fillSome(FillBatch);
                rowsChanged();
                return;
        }
        fillTick(this); // the first batch now: the view never draws rows the open emptied
        return;
}

void FileDialog::stopReading() {
        if (_fillTimer >= 0) application().stopTimer(_fillTimer);
        _fillTimer = -1;
        return;
}

void FileDialog::fillTick(void *arg) {
        auto        *self = static_cast<FileDialog *>(arg);
        FolderModel &f = self->_items.folder();
        (void)f.fillSome(FillBatch);
        if (f.isComplete()) self->stopReading();
        self->rowsChanged();
        return;
}

// The view reset to the model's rows, keeping the current row (by name), or selecting the one asked
// for once it is read.
void FileDialog::rowsChanged() {
        const FolderModel &f = _items.folder();
        String             keep(MemSpace::Psram);
        if (!_select.isEmpty()) {
                (void)keep.assign(_select.view());
        } else if (_view->current() >= 0 && size_t(_view->current()) < f.count()) {
                (void)keep.assign(f.name(size_t(_view->current())));
        }
        const int row = keep.isEmpty() ? -1 : f.indexOf(keep.view());
        if (row >= 0) {
                const size_t rows[] = {size_t(row)};
                _view->resetTo(Span<const size_t>(rows), row);
                _view->showRow(size_t(row));
                _select.clear();
        } else {
                _view->modelReset();
        }
        if (f.isComplete()) _select.clear(); // what wasn't found is gone
        return;
}

void FileDialog::applyFilter() {
        const int        i = _type->current();
        const StringView patterns = _mode == Mode::Folder                ? NameFilter::NoFiles
                                    : i >= 0 && size_t(i) < _filterCount ? StringView(_filters[i].patterns)
                                                                         : StringView();
        if (_items.folder().setNameFilter(patterns).isOk()) rowsChanged();
        return;
}

// ---- Choosing

void FileDialog::accept() {
        if (_asking) return;
        if (acceptNames()) finishWith();
        return;
}

// The name field's names, else the list's folder or selection: chosen into _paths (true), or gone
// somewhere, warned of or asked about (false).
bool FileDialog::acceptNames() {
        _paths.clear();
        List<StringView> names(MemSpace::Psram);
        if (FileNameList::split(_name->text(), names).isError()) return false;
        const FolderModel &f = _items.folder();
        if (names.isEmpty()) {
                // No name: the list's folder entered, or (choosing a folder) the one shown.
                const int row = _view->current();
                if (row >= 0 && size_t(row) < f.count() && f.entry(size_t(row)).isDirectory() &&
                    _view->selection().isSelected(size_t(row))) {
                        String path(MemSpace::Psram);
                        if (join(f.path(), f.name(size_t(row)), path).isOk()) (void)go(path.view(), StringView());
                        return false;
                }
                if (_mode != Mode::Folder) return false;
                choose(f.path());
                return true;
        }
        if (names.size() == 1 && NameFilter::hasWildcard(names[0])) {
                // A pattern: the folder filtered with it, until the type changes.
                if (_items.folder().setNameFilter(names[0]).isOk()) rowsChanged();
                _name->setText(StringView());
                return false;
        }
        if (names.size() > 1 && _mode != Mode::OpenMany) {
                warn("Choose one name, not \"%.*s\" and more.", names[0]);
                return false;
        }
        for (const StringView name : names) {
                auto [abs, nerr] = Vfs::normalize(name, f.path());
                if (nerr.isError()) {
                        warn("\"%.*s\" isn't a name a file can have.", name);
                        return false;
                }
                auto [info, serr] = Vfs::stat(abs.view(), Root);
                const bool exists = serr.isOk();
                if (exists && info.type == FileType::Directory) {
                        if (_mode == Mode::Folder) {
                                choose(abs.view());
                                continue;
                        }
                        if (names.size() > 1) {
                                warn("\"%.*s\" is a folder.", name);
                                return false;
                        }
                        // A folder typed: gone into.
                        if (go(abs.view(), StringView()).isOk()) _name->setText(StringView());
                        return false;
                }
                if (_mode == Mode::Save) {
                        String path(MemSpace::Psram);
                        if (path.assign(abs.view()).isError()) return false;
                        // The type's extension, unless it has one.
                        const StringView base = abs.view().mid(abs.view().lastIndexOf('/') + 1);
                        const int        t = _type->current();
                        const StringView ext = t >= 0 && size_t(t) < _filterCount
                                                       ? NameFilter::firstExtension(StringView(_filters[t].patterns))
                                                       : StringView();
                        if (!ext.isEmpty() && FolderModel::extensionOf(base).isEmpty() &&
                            (path.append('.').isError() || path.append(ext).isError()))
                                return false;
                        auto [sinfo, sserr] = Vfs::stat(path.view(), Root);
                        if (sserr.isOk() && sinfo.type == FileType::Directory) {
                                warn("\"%.*s\" is a folder.", name);
                                return false;
                        }
                        const StringView p = path.view();
                        const size_t     slash = p.lastIndexOf('/');
                        auto [dinfo, derr] = Vfs::stat(slash == 0 ? StringView("/") : p.left(slash), Root);
                        if (derr.isError() || dinfo.type != FileType::Directory) {
                                warn("There's no folder for \"%.*s\".", name);
                                return false;
                        }
                        if (sserr.isOk()) {
                                // There already: asked first.
                                char             text[MessageBox::MaxText + 1];
                                const StringView b = p.mid(slash + 1);
                                std::snprintf(text, sizeof(text), "\"%.*s\" already exists.\nReplace it?",
                                              int(b.size()), b.data());
                                if (_pending.assign(p).isError()) return false;
                                _asking = MessageBox::show(application(), MessageBox::Kind::Question, title(),
                                                           StringView(text), MessageBox::Buttons::YesNo,
                                                           overwriteAnswered, this)
                                                  .isOk();
                                return false;
                        }
                        choose(p);
                        continue;
                }
                if (!exists) {
                        warn(_mode == Mode::Folder ? "Can't find the folder \"%.*s\"." : "Can't find \"%.*s\".", name);
                        return false;
                }
                choose(abs.view());
        }
        return !_paths.isEmpty();
}

void FileDialog::choose(StringView path) {
        String s(MemSpace::Psram);
        if (s.assign(path).isOk()) (void)_paths.append(std::move(s));
        return;
}

void FileDialog::finishWith() {
        finish(Accepted);
        return;
}

void FileDialog::overwriteAnswered(void *arg, MessageBox::Answer answer) {
        auto *self = static_cast<FileDialog *>(arg);
        self->_asking = false;
        if (answer != MessageBox::Answer::Yes || !self->isShown()) return;
        self->_paths.clear();
        self->choose(self->_pending.view());
        if (!self->_paths.isEmpty()) self->finishWith();
        return;
}

void FileDialog::warn(StringView format, StringView name) {
        char        text[MessageBox::MaxText + 1];
        const char *f = format.data(); // a literal: NUL-ended
        std::snprintf(text, sizeof(text), f, int(name.size()), name.data());
        (void)MessageBox::show(application(), MessageBox::Kind::Warning, title(), StringView(text),
                               MessageBox::Buttons::Ok, nullptr, nullptr);
        return;
}

// Closed (chosen, cancelled, the close box): the folder no longer read or watched.
void FileDialog::finished(void *arg, Dialog &, int result) {
        auto *self = static_cast<FileDialog *>(arg);
        self->stopReading();
        if (!self->_watched.isEmpty()) self->_watches.remove(self->_watched.view());
        self->_watched.clear();
        if (result != Accepted) self->_paths.clear();
        if (self->_told != nullptr) self->_told(self->_toldArg, *self, result);
        return;
}

// ---- Events

void FileDialog::iconsAsked(void *arg) {
        auto *self = static_cast<FileDialog *>(arg);
        if (self->_iconsPosted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->application().post(iconsDue, self).isError())
                self->_iconsPosted.store(false, std::memory_order_release);
        return;
}

void FileDialog::iconsDue(void *arg) {
        auto *self = static_cast<FileDialog *>(arg);
        self->_iconsPosted.store(false, std::memory_order_release);
        if (self->_icons.loadAsked()) self->_view->update();
        return;
}

// A change in the folder (any task, the storage lock held): taken on the loop a little later.
void FileDialog::watchNotify(void *arg) {
        auto *self = static_cast<FileDialog *>(arg);
        if (self->_watchPosted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->application().post(watchesDue, self).isError())
                self->_watchPosted.store(false, std::memory_order_release);
        return;
}

void FileDialog::watchesDue(void *arg) {
        auto *self = static_cast<FileDialog *>(arg);
        self->_watchPosted.store(false, std::memory_order_release);
        self->_watches.take(watchChanged, self);
        return;
}

void FileDialog::watchChanged(void *arg, StringView path, uint8_t changes) {
        auto *self = static_cast<FileDialog *>(arg);
        (void)changes;
        if (self->isShown() && path == self->folder()) self->reload();
        return;
}

void FileDialog::backClicked(void *arg, Button &) {
        auto *self = static_cast<FileDialog *>(arg);
        if (!self->_history.canGoBack()) return;
        // The folder before, as it was; its history entry kept (go() visits it again: no change).
        String path(MemSpace::Psram);
        if (path.assign(self->_history.back()).isOk()) (void)self->go(path.view(), StringView());
        return;
}

void FileDialog::upClicked(void *arg, Button &) {
        auto      *self = static_cast<FileDialog *>(arg);
        String     path(MemSpace::Psram);
        const auto here = self->folder();
        if (here == "/" || path.assign(NavigationHistory::parentOf(here)).isError()) return;
        // The folder left, selected in its parent.
        const StringView name = here.mid(here.lastIndexOf('/') + 1);
        String           select(MemSpace::Psram);
        (void)select.assign(name);
        (void)self->go(path.view(), select.view());
        return;
}

void FileDialog::placeClicked(void *arg, Button &button) {
        auto *self = static_cast<FileDialog *>(arg);
        for (const Place &p : self->_places)
                if (p.button == &button) (void)self->go(StringView(p.path), StringView());
        return;
}

void FileDialog::acceptClicked(void *arg, Button &) {
        static_cast<FileDialog *>(arg)->accept();
        return;
}

void FileDialog::lookInAccepted(void *arg, LineEdit &edit) {
        auto  *self = static_cast<FileDialog *>(arg);
        String typed(MemSpace::Psram);
        if (typed.assign(edit.text()).isError()) return;
        if (self->go(typed.view(), StringView()).isError()) {
                self->warn("Can't find \"%.*s\".", typed.view());
                edit.setText(self->folder());
                return;
        }
        self->_view->setFocus();
        return;
}

void FileDialog::lookInCancelled(void *arg, LineEdit &edit) {
        edit.setText(static_cast<FileDialog *>(arg)->folder());
        return;
}

void FileDialog::nameAccepted(void *arg, LineEdit &) {
        static_cast<FileDialog *>(arg)->accept();
        return;
}

void FileDialog::nameCancelled(void *arg, LineEdit &) {
        static_cast<FileDialog *>(arg)->finish(Rejected);
        return;
}

void FileDialog::typeChanged(void *arg, ComboBox &, int) {
        static_cast<FileDialog *>(arg)->applyFilter();
        return;
}

// Return or a double click: a folder entered, a file chosen.
void FileDialog::activated(void *arg, ItemView &, size_t row) {
        auto              *self = static_cast<FileDialog *>(arg);
        const FolderModel &f = self->_items.folder();
        if (row >= f.count()) return;
        String path(MemSpace::Psram);
        if (join(f.path(), f.name(row), path).isError()) return;
        if (f.entry(row).isDirectory()) {
                (void)self->go(path.view(), StringView());
                return;
        }
        if (self->_mode == Mode::Folder) return;
        if (self->_mode != Mode::OpenMany || self->_view->selection().selectedCount() <= 1)
                self->_name->setText(f.name(row));
        self->accept();
        return;
}

// The user's selection, into the name field: files (a folder's name when choosing one).
void FileDialog::selectionChanged(void *arg, ItemView &view) {
        auto              *self = static_cast<FileDialog *>(arg);
        const FolderModel &f = self->_items.folder();
        if (!view.hasFocus()) return; // the program's, or a refresh: the field kept
        String     text(MemSpace::Psram);
        StringView first;
        size_t     files = 0;
        for (int r = view.selection().nextSelected(0); r >= 0 && size_t(r) < f.count();
             r = view.selection().nextSelected(size_t(r) + 1)) {
                if (f.entry(size_t(r)).isDirectory() != (self->_mode == Mode::Folder)) continue;
                if (files++ == 0) first = f.name(size_t(r));
                if (self->_mode != Mode::OpenMany) break;
                if (FileNameList::appendQuoted(f.name(size_t(r)), text).isError()) return;
        }
        if (files == 0) return;
        self->_name->setText(files == 1 ? first : text.view()); // one as it is, several in quotes
        (void)self->_selectionNames.assign(self->_name->text());
        return;
}

bool FileDialog::viewKey(void *arg, ItemView &, const KeyEvent &event) {
        auto *self = static_cast<FileDialog *>(arg);
        if (event.action == KeyEvent::Action::Release || event.usage != HidKey::Backspace || event.modifiers != 0)
                return false;
        upClicked(self, *self->_up);
        return true;
}

void FileDialog::sortAsked(void *arg, DetailsView &view, size_t column) {
        auto                   *self = static_cast<FileDialog *>(arg);
        FolderModel            &f = self->_items.folder();
        const FolderModel::Sort sort = self->_items.sortOf(column);
        const bool              down = f.sort() == sort && !f.isDescending();
        f.setSort(sort, down);
        view.setSortIndicator(int(self->_items.columnOf(sort)), down);
        self->rowsChanged();
        return;
}

BMDS_NAMESPACE_END
