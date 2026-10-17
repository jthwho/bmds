/**
 * @file      folderwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A folder window, and the menus folder windows share (devplan/filebrowser.md §4h).
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/apps.h>
#include <bmds/fileops.h>
#include <bmds/guiapplication.h>
#include <bmds/memory.h>
#include <bmds/menulayout.h>
#include <bmds/vfs.h>

#include "folderwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t     FillBatch = 64;    // entries read a turn (FB12)
        constexpr size_t     IdentifyBatch = 4; // files identified by their contents a turn (512 bytes each)
        constexpr size_t     MaxName = 255;     // a name's bytes (FAT's long names)
        constexpr uint16_t   TreeWidth = 200;   // the folder tree's, unless remembered
        constexpr StringView HomePath("/home");
        constexpr StringView DesktopPath("/home/Desktop");

        constexpr uint8_t F4 = HidKey::F1 + 3, F5 = HidKey::F1 + 4;
        constexpr uint8_t KeyC = HidKey::A + ('c' - 'a'), KeyD = HidKey::A + ('d' - 'a');
        constexpr uint8_t KeyL = HidKey::A + ('l' - 'a'), KeyV = HidKey::A + ('v' - 'a');
        constexpr uint8_t KeyW = HidKey::A + ('w' - 'a'), KeyX = HidKey::A + ('x' - 'a');

        bool lessBytes(StringView a, StringView b) {
                const size_t n = a.size() < b.size() ? a.size() : b.size();
                const int    c = std::memcmp(a.data(), b.data(), n);
                return c < 0 || (c == 0 && a.size() < b.size());
        }

} // namespace

// ---- The window

FolderWindow::FolderWindow(GuiApplication &app, const Config &config, FolderWindowHost &host)
    : GuiWindow(app, config), _host(host), _items(host.fileIcons()) {
        _items.setCopied(&host.folderActions().copied());
}

FolderWindow::~FolderWindow() {
        stopReading();
        unwatchAll();
        _host.folderMenus().forget(*this);
        if (_nameEdit != nullptr) {
                _nameEdit->~PopupEdit();
                Memory::free(_nameEdit);
        }
        // The views go with the window, after its members: the model left first.
        if (_icons != nullptr) _icons->setModel(nullptr);
        if (_details != nullptr) _details->setModel(nullptr);
}

Error FolderWindow::build() {
        auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (column == nullptr) return Error::NoMem;
        setCentral(column);
        _bar = Widget::create<MenuBar>(column);
        _tools = Widget::create<ToolBar>(column);
        auto *row = Widget::create<BoxLayout>(column, BoxLayout::Direction::Horizontal);
        if (_bar == nullptr || _tools == nullptr || row == nullptr) return Error::NoMem;
        Error err = _host.folderMenus().attach(*_bar);
        if (err.isError()) return err;
        setMenuBar(_bar);

        // The toolbar: each button's choice its data (its icon set by setIcons()).
        struct Tool {
                        IconId      icon;
                        const char *text; // null: a separator
                        Choice      choice;
        };
        constexpr Tool tools[] = {
                {IconId::GoPrevious, "Back", ChooseBack},
                {IconId::GoNext, "Forward", ChooseForward},
                {IconId::GoUp, "Up", ChooseUp},
                {IconId::Folder, nullptr, ChooseOpen},
                {IconId::EditCut, "Cut", ChooseCut},
                {IconId::EditCopy, "Copy", ChooseCopy},
                {IconId::EditPaste, "Paste", ChoosePaste},
                {IconId::EditDelete, "Delete", ChooseDelete},
                {IconId::Folder, nullptr, ChooseOpen},
                {IconId::ViewListIcons, "Icons", ChooseIcons},
                {IconId::ViewListDetails, "Details", ChooseDetails},
        };
        size_t iconed = 0;
        for (const Tool &t : tools) {
                if (t.text == nullptr) {
                        (void)_tools->addSeparator();
                        continue;
                }
                Button *b = _tools->addButton(nullptr, t.text, toolClicked, this);
                if (b == nullptr) return Error::NoMem;
                b->setData(uintptr_t(t.choice));
                if (iconed < sizeof(_toolButtons) / sizeof(_toolButtons[0])) {
                        _toolButtons[iconed] = b;
                        _toolIcons[iconed++] = t.icon;
                }
                if (t.choice == ChooseBack) _back = b;
                if (t.choice == ChooseForward) _forward = b;
                if (t.choice == ChooseUp) _up = b;
        }

        // The address.
        row->setMargins(4, 2, 4, 2);
        row->setSpacing(4);
        auto *label = Widget::create<Label>(row, "Address");
        _address = Widget::create<LineEdit>(row);
        if (label == nullptr || _address == nullptr) return Error::NoMem;
        _address->setStretch(1);
        _address->setAccepted(addressAccepted, this);
        _address->setCancelled(addressCancelled, this);

        // The tree beside the views.
        _split = Widget::create<Splitter>(column, Splitter::Orientation::Horizontal);
        if (_split == nullptr) return Error::NoMem;
        _split->setStretch(1);
        _tree = Widget::create<TreeView>(_split);
        auto *right = Widget::create<BoxLayout>(_split, BoxLayout::Direction::Vertical);
        if (_tree == nullptr || right == nullptr) return Error::NoMem;
        _icons = Widget::create<IconView>(right);
        _details = Widget::create<DetailsView>(right);
        if (_icons == nullptr || _details == nullptr) return Error::NoMem;
        for (ItemView *v : {static_cast<ItemView *>(_icons), static_cast<ItemView *>(_details)}) {
                v->setStretch(1);
                v->setModel(&_items);
                v->setActivated(viewActivated, this);
                v->setChanged(viewChanged, this);
                v->setMenuAsked(viewMenu, this);
                v->setRenameAsked(viewRename, this);
                v->setKeyTyped(viewKey, this);
                v->setDragAsked(viewDragAsked, this);
                v->setDropHandlers(viewDropAsked, viewDropped, this);
        }
        _tree->setDropHandlers(treeDropAsked, treeDropped, this);
        _details->setSortAsked(sortAsked, this);
        _details->setVisible(false);
        _tree->setExpanding(treeExpanding, this);
        _tree->setChanged(treeChosen, this);
        _tree->setRowsChanged(treeRowsChanged, this);
        (void)_tree->addRoot("/");
        _split->setLeast(80, 160);
        _split->setPosition(TreeWidth);
        _split->setWidgets(_tree, right);

        _status = Widget::create<StatusBar>(column);
        if (_status == nullptr) return Error::NoMem;
        (void)_status->addField(0);
        (void)_status->addField(220);
        (void)_status->addField(140);
        setIcons();
        column->relayout();
        _tree->expand(0);
        _icons->setFocus();
        return Error::Ok;
}

// The window's and the tree's icons (again after a new theme: the old ones are gone).
void FolderWindow::setIcons() {
        FileIcons  &icons = _host.fileIcons();
        const Icon *folder = icons.named(IconId::Folder, 16);
        _tree->setIcons(icons.named(IconId::DriveHarddisk, 16), folder, icons.named(IconId::FolderOpen, 16));
        setIcon(folder);
        setLargeIcon(icons.named(IconId::Folder, 32));
        for (size_t i = 0; i < sizeof(_toolButtons) / sizeof(_toolButtons[0]); ++i)
                if (_toolButtons[i] != nullptr) _toolButtons[i]->setIcon(icons.named(_toolIcons[i], 16));
        return;
}

ItemView &FolderWindow::view() const {
        if (_view == FolderSettings::View::Details) return *_details;
        return *_icons;
}

// ---- Going to a folder

Error FolderWindow::go(StringView path, StringView select) {
        // Absolute, against the folder shown (the address may be relative).
        auto [abs, nerr] = Vfs::normalize(path.trimmed(), this->path().isEmpty() ? StringView("/") : this->path());
        const Vfs::Context vc{"/", nullptr};
        Error              err = nerr;
        FileType           type = FileType::File;
        if (err.isOk()) {
                auto [info, serr] = Vfs::stat(abs.view(), vc);
                type = info.type;
                err = serr;
        }
        if (err.isError()) {
                char message[200];
                std::snprintf(message, sizeof(message), "Can't find \"%.*s\".", int(path.size()), path.data());
                _host.folderActions().tell("Files", StringView(message));
                return err;
        }
        if (type != FileType::Directory) {
                // A file: its folder, the file selected.
                const StringView p = abs.view();
                const size_t     slash = p.lastIndexOf('/');
                return go(slash == 0 ? StringView("/") : p.left(slash), p.mid(slash + 1));
        }
        if (abs.view() == this->path()) {
                // Here already: only the name to select.
                if (!select.isEmpty() && _select.assign(select).isOk()) restoreSelection();
                return Error::Ok;
        }
        stopReading();
        if (_nameEdit != nullptr && _nameEdit->isShown()) _nameEdit->close();
        FolderSettings::Values values;
        if (!_host.folderSettings().find(abs.view(), values)) values.view = _host.defaultView();
        FolderModel &folder = _items.folder();
        String       was(MemSpace::Psram);
        (void)was.assign(this->path());
        folder.setShowHidden(_host.showHidden());
        folder.setSort(values.sort, values.descending);
        err = folder.open(abs.view(), vc);
        if (err.isError()) {
                // The model is empty now: the views told, then the folder shown read again.
                _icons->resetTo(Span<const size_t>(), -1);
                _details->resetTo(Span<const size_t>(), -1);
                if (!was.isEmpty() && folder.open(was.view(), vc).isOk()) startReading();
                char message[200];
                std::snprintf(message, sizeof(message), "Can't open \"%.*s\": %s.", int(abs.size()), abs.cStr(),
                              err.desc());
                _host.folderActions().tell("Files", StringView(message));
                return err;
        }
        (void)_history.visit(abs.view());
        watchFolder();
        _refreshDue = false;
        _kept.clear();
        _keptCurrent.clear();
        _select.clear();
        _renameSelected = false;
        if (!select.isEmpty()) (void)_select.assign(select);
        _details->setSortIndicator(int(FolderItemModel::columnOf(values.sort)), values.descending);
        showView(values.view);
        _icons->resetTo(Span<const size_t>(), -1);
        _details->resetTo(Span<const size_t>(), -1);
        // The title: the path, its end if it is long.
        const StringView p = this->path();
        char             title[WindowManager::MaxTitle + 1];
        if (p.size() <= WindowManager::MaxTitle) {
                std::snprintf(title, sizeof(title), "%.*s", int(p.size()), p.data());
        } else {
                const StringView tail = p.mid(p.size() - (WindowManager::MaxTitle - 3));
                std::snprintf(title, sizeof(title), "...%.*s", int(tail.size()), tail.data());
        }
        setTitle(StringView(title));
        _address->setText(p);
        auto [fs, ferr] = Vfs::statfs(p, vc);
        _hasFree = ferr.isOk();
        _freeBytes = ferr.isOk() ? fs.freeBytes : 0;
        startReading();
        syncTree();
        updateTools();
        updateStatus();
        return Error::Ok;
}

void FolderWindow::refresh() {
        if (path().isEmpty()) return;
        // The selection as it is now; while a read is under way, the names it still looks for.
        if (_items.folder().isComplete()) keepSelection();
        stopReading();
        const Vfs::Context vc{"/", nullptr};
        String             p(MemSpace::Psram);
        if (p.assign(path()).isError()) return;
        FolderModel &folder = _items.folder();
        folder.setShowHidden(_host.showHidden());
        if (folder.open(p.view(), vc).isError()) {
                // Gone (removed elsewhere, its filesystem unmounted): the model is empty now, the
                // views told; then the nearest folder above it that is there (the root at last).
                _icons->resetTo(Span<const size_t>(), -1);
                _details->resetTo(Span<const size_t>(), -1);
                do {
                        const StringView up = NavigationHistory::parentOf(p.view()); // a prefix of p
                        if (up.isEmpty()) break;
                        p.truncate(up.size());
                        auto [info, serr] = Vfs::stat(p.view(), vc);
                        if (serr.isOk() && info.isDirectory()) break;
                } while (p.view() != "/");
                (void)go(p.view());
                return;
        }
        auto [fs, ferr] = Vfs::statfs(p.view(), vc);
        _hasFree = ferr.isOk();
        _freeBytes = ferr.isOk() ? fs.freeBytes : 0;
        startReading();
        return;
}

// ---- Changes from elsewhere (devplan/filebrowser.md §4i)

void FolderWindow::redrawIcons() {
        view().update();
        return;
}

void FolderWindow::watchedChanged(StringView folder, uint8_t changes) {
        if (folder == path()) {
                // Not under a name being edited: its row would move.
                if (_nameEdit != nullptr && _nameEdit->isShown()) {
                        _refreshDue = true;
                } else {
                        refresh();
                }
        }
        if ((changes & (DirectoryWatch::Entries | DirectoryWatch::Mounts)) != 0) treeChanged(folder);
        return;
}

void FolderWindow::watchFolder() {
        if (_watched.view() == path()) return;
        FolderWatches &watches = _host.folderWatches();
        if (!_watched.isEmpty()) watches.remove(_watched.view());
        _watched.clear();
        if (watches.add(path()).isOk() && _watched.assign(path()).isError()) watches.remove(path());
        return;
}

void FolderWindow::unwatchAll() {
        FolderWatches &watches = _host.folderWatches();
        if (!_watched.isEmpty()) watches.remove(_watched.view());
        _watched.clear();
        for (StringView rest = _treeWatched.view(); !rest.isEmpty();) {
                const size_t end = rest.indexOf('\0');
                if (end == StringView::NPos) break;
                watches.remove(rest.left(end));
                rest = rest.mid(end + 1);
        }
        _treeWatched.clear();
        return;
}

namespace {

        // True if @p list (names, NUL after each) has @p item.
        bool listHas(StringView list, StringView item) {
                while (!list.isEmpty()) {
                        const size_t end = list.indexOf('\0');
                        if (end == StringView::NPos) return false;
                        if (list.left(end) == item) return true;
                        list = list.mid(end + 1);
                }
                return false;
        }

} // namespace

// The tree's rows changed: its expanded folders watched, those no longer expanded not.
void FolderWindow::treeRowsChanged(void *arg, TreeView &view) {
        auto           *self = static_cast<FolderWindow *>(arg);
        const TreeRows &rows = view.rows();
        String          now(MemSpace::Psram);
        String          p(MemSpace::Psram);
        for (size_t r = 0; r < rows.count(); ++r) {
                if ((rows.row(r).flags & TreeRows::Expanded) == 0) continue;
                if (rows.pathOf(r, p).isError() || now.append(p.view()).isError() || now.append('\0').isError())
                        return; // no memory: as they were
        }
        // Those now watched: the ones before still expanded, and the new ones that could be.
        FolderWatches &watches = self->_host.folderWatches();
        String         watched(MemSpace::Psram);
        if (watched.reserve(now.size()).isError()) return;
        for (StringView rest = now.view(); !rest.isEmpty();) {
                const size_t     end = rest.indexOf('\0');
                const StringView folder = rest.left(end);
                rest = rest.mid(end + 1);
                if (listHas(self->_treeWatched.view(), folder) || watches.add(folder).isOk()) {
                        (void)watched.append(folder); // reserved
                        (void)watched.append('\0');
                }
        }
        for (StringView rest = self->_treeWatched.view(); !rest.isEmpty();) {
                const size_t     end = rest.indexOf('\0');
                const StringView folder = rest.left(end);
                rest = rest.mid(end + 1);
                if (!listHas(watched.view(), folder)) watches.remove(folder);
        }
        self->_treeWatched = std::move(watched);
        return;
}

void FolderWindow::settingsChanged() {
        setIcons();
        if (_items.folder().showsHidden() != _host.showHidden()) refresh();
        view().update();
        return;
}

// The folder read in batches on the loop (FB12), shown as it fills, then the files whose names
// say nothing identified by their contents.
void FolderWindow::startReading() {
        stopReading();
        auto [timer, err] = application().startTimer(1, true, fillTick, this);
        _fillTimer = err.isOk() ? timer : -1;
        if (_fillTimer < 0) {
                // No timer: all of it now.
                while (!_items.folder().isComplete()) (void)_items.folder().fillSome(FillBatch);
                restoreSelection();
                return;
        }
        // The first batch now: the views never draw rows the model emptied by its open has lost.
        fillTick(this);
        return;
}

void FolderWindow::stopReading() {
        if (_fillTimer >= 0) application().stopTimer(_fillTimer);
        _fillTimer = -1;
        return;
}

void FolderWindow::fillTick(void *arg) {
        auto        *self = static_cast<FolderWindow *>(arg);
        FolderModel &folder = self->_items.folder();
        if (!folder.isComplete()) {
                (void)folder.fillSome(FillBatch);
                self->restoreSelection();
                if (folder.isComplete()) {
                        // What wasn't found is gone.
                        self->_select.clear();
                        self->_renameSelected = false;
                        self->keepSelection();
                }
                self->updateStatus();
                return;
        }
        const bool done = self->_items.identifySome(IdentifyBatch);
        self->view().update(); // icons for the types found
        if (done) self->stopReading();
        return;
}

// The selection's names, to find again after the rows change.
void FolderWindow::keepSelection() {
        _kept.clear();
        _keptCurrent.clear();
        const ItemView    &v = view();
        const FolderModel &folder = _items.folder();
        for (int r = v.selection().nextSelected(0); r >= 0; r = v.selection().nextSelected(size_t(r) + 1)) {
                if (size_t(r) >= folder.count()) break;
                if (_kept.append(folder.name(size_t(r))).isError() || _kept.append('\0').isError()) break;
        }
        if (v.current() >= 0 && size_t(v.current()) < folder.count())
                (void)_keptCurrent.assign(folder.name(size_t(v.current())));
        return;
}

// The view reset to the rows now, the names kept selected again (and a name asked for, alone).
void FolderWindow::restoreSelection() {
        const FolderModel &folder = _items.folder();
        List<StringView>   names(MemSpace::Psram);
        for (StringView rest = _kept.view(); !rest.isEmpty();) {
                const size_t end = rest.indexOf('\0');
                if (end == StringView::NPos) break;
                (void)names.append(rest.left(end));
                rest = rest.mid(end + 1);
        }
        std::sort(names.data(), names.data() + names.size(), lessBytes);
        List<size_t> rows(MemSpace::Psram);
        int          current = -1;
        for (size_t row = 0; row < folder.count(); ++row) {
                const StringView name = folder.name(row);
                if (!names.isEmpty() && std::binary_search(names.data(), names.data() + names.size(), name, lessBytes))
                        (void)rows.append(row);
                if (current < 0 && !_keptCurrent.isEmpty() && name == _keptCurrent.view()) current = int(row);
        }
        int asked = _select.isEmpty() ? -1 : folder.indexOf(_select.view());
        if (asked >= 0) {
                rows.clear();
                (void)rows.append(size_t(asked));
                current = asked;
        }
        ItemView &v = view();
        v.resetTo(rows.span(), current);
        if (asked < 0) return;
        // Found: shown, kept as the selection from now on, renamed if new.
        v.showRow(size_t(asked));
        const bool rename = _renameSelected;
        _select.clear();
        _renameSelected = false;
        keepSelection();
        if (rename) startRename(size_t(asked), folder.name(size_t(asked)));
        return;
}

void FolderWindow::showView(FolderSettings::View view) {
        const bool focused = this->view().hasFocus();
        if (view != _view) {
                keepSelection();
                _view = view;
                _icons->setVisible(view == FolderSettings::View::Icons);
                _details->setVisible(view == FolderSettings::View::Details);
                static_cast<BoxLayout *>(_icons->parent())->relayout();
                restoreSelection();
        }
        if (focused) this->view().setFocus();
        return;
}

void FolderWindow::setSort(FolderModel::Sort sort, bool descending) {
        keepSelection();
        _items.folder().setSort(sort, descending);
        _details->setSortIndicator(int(FolderItemModel::columnOf(sort)), descending);
        restoreSelection();
        remember();
        return;
}

// The folder's view and sort (and the tree's width) into folders.conf (FB16).
void FolderWindow::remember() {
        FolderSettings::Values v;
        v.view = _view;
        v.sort = _items.folder().sort();
        v.descending = _items.folder().isDescending();
        v.tree = _treeShown ? _split->position() : 0;
        if (_host.folderSettings().set(path(), v).isOk()) _host.folderSettingsChanged();
        return;
}

// ---- The tree: it follows the folder shown

void FolderWindow::syncTree() {
        _syncingTree = true;
        const StringView p = path();
        int              row = _tree->rows().find("/");
        for (size_t at = 1; row >= 0 && at <= p.size();) {
                size_t end = p.mid(at).indexOf('/');
                end = end == StringView::NPos ? p.size() : at + end;
                if (end == at) break;
                if ((_tree->rows().row(size_t(row)).flags & TreeRows::Expanded) == 0) _tree->expand(size_t(row));
                const int next = _tree->rows().find(p.left(end));
                if (next < 0) break;
                row = next;
                at = end + 1;
        }
        if (row >= 0 && row != _tree->current()) _tree->selectOnly(row);
        _syncingTree = false;
        return;
}

// A folder in the tree expanded: its folders (and links to folders), as the tree's children, sorted
// as the views sort names. Only the listing is read, the files passed over: a folder of thousands of
// files costs its listing, not a model of every file (a link is looked up, to know where it leads).
void FolderWindow::treeExpanding(void *arg, TreeView &view, size_t row) {
        auto  *self = static_cast<FolderWindow *>(arg);
        String path(MemSpace::Psram);
        if (view.rows().pathOf(row, path).isError()) return;
        const Vfs::Context vc{"/", nullptr};
        const bool         hidden = self->_host.showHidden();
        List<String>       folders(MemSpace::Psram);
        auto [dir, err] = Vfs::openDirectory(path.view(), vc);
        String child(MemSpace::Psram);
        while (err.isOk()) {
                auto [info, nerr] = dir.next();
                if (nerr.isError()) break;
                if (!hidden && (info.name.view().startsWith('.') || (info.attributes & FileInfo::Hidden) != 0))
                        continue;
                bool folder = info.isDirectory();
                if (info.type == FileType::Link && child.assign(path.view()).isOk() &&
                    (path.view().endsWith('/') || child.append('/').isOk()) && child.append(info.name.view()).isOk()) {
                        auto [target, serr] = Vfs::stat(child.view(), vc);
                        folder = serr.isOk() && target.isDirectory();
                }
                if (folder) err = folders.append(std::move(info.name));
        }
        List<StringView> names(MemSpace::Psram);
        if (names.reserve(folders.size()).isOk()) {
                for (const String &f : folders) (void)names.append(f.view()); // reserved
                std::sort(names.begin(), names.end(),
                          [](StringView a, StringView b) { return FolderModel::compareNames(a, b) < 0; });
        }
        (void)view.setChildren(row, names.span());
        return;
}

void FolderWindow::treeChanged(StringView folder) {
        const int row = _tree->rows().find(folder);
        if (row < 0 || (_tree->rows().row(size_t(row)).flags & TreeRows::Expanded) == 0) return;
        _syncingTree = true;
        treeExpanding(this, *_tree, size_t(row));
        _syncingTree = false;
        return;
}

void FolderWindow::treeChosen(void *arg, ItemView &view) {
        auto *self = static_cast<FolderWindow *>(arg);
        auto &tree = static_cast<TreeView &>(view);
        if (self->_syncingTree || tree.current() < 0) return;
        String path(MemSpace::Psram);
        if (tree.rows().pathOf(size_t(tree.current()), path).isOk() && path.view() != self->path())
                (void)self->go(path.view());
        return;
}

// ---- The views

void FolderWindow::itemPath(size_t row, String &out) const {
        const StringView folder = path();
        out.clear();
        (void)out.assign(folder);
        if (!folder.endsWith('/')) (void)out.append('/');
        (void)out.append(_items.folder().name(row));
        return;
}

void FolderWindow::selectedPaths(String &out) const {
        out.clear();
        const ItemView &v = view();
        String          one(MemSpace::Psram);
        for (int r = v.selection().nextSelected(0); r >= 0; r = v.selection().nextSelected(size_t(r) + 1)) {
                itemPath(size_t(r), one);
                if (!out.isEmpty()) (void)out.append('\n');
                (void)out.append(one.view());
        }
        return;
}

// ---- Drag and drop (devplan/clipboard.md §5e)

bool FolderWindow::viewDragAsked(void *arg, ItemView &, GuiDrag &drag) {
        auto  *self = static_cast<FolderWindow *>(arg);
        String paths(MemSpace::Psram);
        self->selectedPaths(paths);
        return self->_host.folderActions().dragFiles(paths.view(), drag);
}

// Where a drop on @p row of a view goes, into @p out: the row's item if it takes drops (a folder,
// an application) and isn't one being dragged, else the folder shown (@p row -1 then).
bool FolderWindow::dropTarget(ItemView &view, int &row, const GuiDropEvent &event, String &out) {
        FolderActions &actions = _host.folderActions();
        if (row >= 0 && !(event.isOwn() && event.source() == &view && view.selection().isSelected(size_t(row)))) {
                itemPath(size_t(row), out);
                if (actions.dropAction(out.view(), event) != DropAction::None) return true;
        }
        row = -1;
        return out.assign(path()).isOk();
}

DropAction FolderWindow::viewDropAsked(void *arg, ItemView &view, int &row, const GuiDropEvent &event) {
        auto  *self = static_cast<FolderWindow *>(arg);
        String target(MemSpace::Psram);
        if (!self->dropTarget(view, row, event, target)) return DropAction::None;
        return self->_host.folderActions().dropAction(target.view(), event);
}

bool FolderWindow::viewDropped(void *arg, ItemView &view, int row, GuiDropEvent &event) {
        auto  *self = static_cast<FolderWindow *>(arg);
        String target(MemSpace::Psram);
        if (!self->dropTarget(view, row, event, target)) return false;
        const Rect s = self->screenRect();
        return self->_host.folderActions().drop(target.view(), event, uint16_t(s.x + event.x()),
                                                uint16_t(s.y + event.y()));
}

// The tree: a folder row takes the drop; between rows, nothing.
DropAction FolderWindow::treeDropAsked(void *arg, ItemView &view, int &row, const GuiDropEvent &event) {
        auto  *self = static_cast<FolderWindow *>(arg);
        String target(MemSpace::Psram);
        if (row < 0 || static_cast<TreeView &>(view).rows().pathOf(size_t(row), target).isError())
                return DropAction::None;
        return self->_host.folderActions().dropAction(target.view(), event);
}

bool FolderWindow::treeDropped(void *arg, ItemView &view, int row, GuiDropEvent &event) {
        auto  *self = static_cast<FolderWindow *>(arg);
        String target(MemSpace::Psram);
        if (row < 0 || static_cast<TreeView &>(view).rows().pathOf(size_t(row), target).isError()) return false;
        const Rect s = self->screenRect();
        return self->_host.folderActions().drop(target.view(), event, uint16_t(s.x + event.x()),
                                                uint16_t(s.y + event.y()));
}

bool FolderWindow::folderSelected() const {
        const ItemView &v = view();
        const int       r = v.selection().nextSelected(0);
        return r >= 0 && v.selection().selectedCount() == 1 && _items.folder().entry(size_t(r)).isDirectory();
}

bool FolderWindow::applicationSelected() const {
        if (!folderSelected()) return false;
        const int r = view().selection().nextSelected(0);
        return Apps::isFolderName(_items.folder().name(size_t(r)));
}

bool FolderWindow::canPaste() {
        return _host.folderActions().canPaste();
}

void FolderWindow::openRow(size_t row, bool newWindow) {
        String p(MemSpace::Psram);
        itemPath(row, p);
        // An application's folder runs it (devplan/apps.md §7); its contents by Show Contents or
        // in a new window.
        const bool application = Apps::isFolderName(_items.folder().name(row));
        if (_items.folder().entry(row).isDirectory() && !(application && !newWindow)) {
                if (newWindow) {
                        _host.openFolder(p.view(), StringView());
                } else {
                        (void)go(p.view());
                }
                return;
        }
        _host.folderActions().open(p.view());
        return;
}

void FolderWindow::viewActivated(void *arg, ItemView &view, size_t row) {
        auto *self = static_cast<FolderWindow *>(arg);
        if (row < self->_items.folder().count())
                self->openRow(row, (view.activationModifiers() & KeyEvent::Shift) != 0);
        return;
}

void FolderWindow::viewChanged(void *arg, ItemView &view) {
        (void)view;
        static_cast<FolderWindow *>(arg)->updateStatus();
        return;
}

void FolderWindow::viewMenu(void *arg, ItemView &view, int row, uint16_t x, uint16_t y) {
        (void)view;
        auto *self = static_cast<FolderWindow *>(arg);
        self->_host.folderMenus().popup(*self, row >= 0, x, y);
        return;
}

void FolderWindow::viewRename(void *arg, ItemView &view, size_t row) {
        (void)view;
        auto *self = static_cast<FolderWindow *>(arg);
        if (row < self->_items.folder().count()) self->startRename(row, self->_items.folder().name(row));
        return;
}

// Keys a view offers first: Delete (Shift: for good), Ctrl+X, C and V, Backspace.
bool FolderWindow::viewKey(void *arg, ItemView &view, const KeyEvent &event) {
        (void)view;
        auto      *self = static_cast<FolderWindow *>(arg);
        const bool plain = !event.hasCtrl() && !event.hasAlt() && !event.hasShift();
        const bool ctrl = event.hasCtrl() && !event.hasAlt() && !event.hasShift();
        const bool shift = event.hasShift() && !event.hasCtrl() && !event.hasAlt();
        if (plain && event.usage == HidKey::Delete) {
                self->choose(ChooseDelete);
        } else if (shift && event.usage == HidKey::Delete) {
                self->choose(ChooseDeleteForGood);
        } else if (ctrl && event.usage == KeyX) {
                self->choose(ChooseCut);
        } else if (ctrl && event.usage == KeyC) {
                self->choose(ChooseCopy);
        } else if (ctrl && event.usage == KeyV) {
                self->choose(ChoosePaste);
        } else if (plain && event.usage == HidKey::Backspace) {
                self->choose(ChooseUp);
        } else {
                return false;
        }
        return true;
}

void FolderWindow::sortAsked(void *arg, DetailsView &view, size_t column) {
        (void)view;
        auto                   *self = static_cast<FolderWindow *>(arg);
        const FolderModel::Sort sort = FolderItemModel::sortOf(column);
        const FolderModel      &folder = self->_items.folder();
        self->setSort(sort, folder.sort() == sort && !folder.isDescending());
        return;
}

// ---- Renaming in place

void FolderWindow::startRename(size_t row, StringView text) {
        if (row >= _items.folder().count()) return;
        if (_nameEdit == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(PopupEdit));
                if (mem == nullptr) return;
                _nameEdit = ::new (mem) PopupEdit(application());
        }
        itemPath(row, _renamePath);
        ItemView             &v = view();
        const PopupEdit::Look look = showsDetails() ? PopupEdit::Look::Field : PopupEdit::Look::IconName;
        if (_nameEdit->open(v.nameRect(row), text, look, renamed, this).isError()) return;
        // A file's extension stays: only the name before it selected (devplan/filetypes.md T10).
        LineEdit *edit = _nameEdit->edit();
        edit->setMaxLength(MaxName);
        const size_t dot = text.lastIndexOf('.');
        if (!_items.folder().entry(row).isDirectory() && dot != StringView::NPos && dot > 0) edit->setSelection(0, dot);
        v.setEditing(int(row));
        return;
}

void FolderWindow::renamed(void *arg, bool accepted, StringView text) {
        auto *self = static_cast<FolderWindow *>(arg);
        self->view().setEditing(-1);
        if (self->_refreshDue) {
                // A change told while the name was edited.
                self->_refreshDue = false;
                self->refresh();
        }
        if (!accepted) return;
        String to(MemSpace::Psram);
        char   problem[200];
        if (self->_host.folderActions().rename(self->_renamePath.view(), text, StringView(), MaxName, to,
                                               Span<char>(problem, sizeof(problem)))) {
                // Read again (the rename told the folder's windows): the new name selected.
                (void)self->_select.assign(FileOps::baseName(to.view()));
                return;
        }
        if (problem[0] == '\0') return; // the same name
        (void)self->_renameTried.assign(text);
        if (MessageBox::show(self->application(), MessageBox::Kind::Error, "Rename", StringView(problem),
                             MessageBox::Buttons::Ok, renameRetry, self)
                    .isError())
                self->_host.folderActions().say("panel: ", problem, "\n");
        return;
}

// Told why the name can't be: back to editing what was typed.
void FolderWindow::renameRetry(void *arg, MessageBox::Answer answer) {
        (void)answer;
        auto     *self = static_cast<FolderWindow *>(arg);
        const int row = self->_items.folder().indexOf(FileOps::baseName(self->_renamePath.view()));
        if (row >= 0) self->startRename(size_t(row), self->_renameTried.view());
        return;
}

void FolderWindow::createNew(FileOps::NewKind kind) {
        auto [made, err] = _host.folderActions().makeNew(path(), kind);
        if (err.isError()) return;
        // Read again (the folder's windows were told): selected, then named in place.
        if (_select.assign(FileOps::baseName(made.view())).isOk()) _renameSelected = true;
        return;
}

// ---- The address

void FolderWindow::addressAccepted(void *arg, LineEdit &edit) {
        auto  *self = static_cast<FolderWindow *>(arg);
        String text(MemSpace::Psram);
        if (text.assign(edit.text()).isError() || text.view().trimmed().isEmpty()) return;
        if (self->go(text.view()).isOk()) self->view().setFocus();
        return;
}

void FolderWindow::addressCancelled(void *arg, LineEdit &edit) {
        auto *self = static_cast<FolderWindow *>(arg);
        edit.setText(self->path());
        self->view().setFocus();
        return;
}

void FolderWindow::toolClicked(void *arg, Button &button) {
        static_cast<FolderWindow *>(arg)->choose(int(button.data()));
        return;
}

// ---- What the menus, the toolbar and the keys ask

void FolderWindow::choose(int choice) {
        FolderActions &actions = _host.folderActions();
        ItemView      &v = view();
        String         paths(MemSpace::Psram);
        switch (choice) {
                case ChooseOpen:
                case ChooseOpenNew: {
                        // One: as a double click; several: each, folders in windows of their own.
                        const size_t n = v.selection().selectedCount();
                        for (int r = v.selection().nextSelected(0); r >= 0;
                             r = v.selection().nextSelected(size_t(r) + 1))
                                openRow(size_t(r), choice == ChooseOpenNew || n > 1);
                        break;
                }
                case ChooseShowContents: {
                        const int r = v.selection().nextSelected(0);
                        if (r < 0) break;
                        String p(MemSpace::Psram);
                        itemPath(size_t(r), p);
                        (void)go(p.view());
                        break;
                }
                case ChooseNewFolder: createNew(FileOps::NewKind::Folder); break;
                case ChooseNewText: createNew(FileOps::NewKind::TextDocument); break;
                case ChooseNewFile: createNew(FileOps::NewKind::EmptyFile); break;
                case ChooseDelete:
                        selectedPaths(paths);
                        if (!paths.isEmpty()) actions.trash(paths.view());
                        break;
                case ChooseRename:
                        if (v.current() >= 0 && v.selection().isSelected(size_t(v.current())))
                                startRename(size_t(v.current()), _items.folder().name(size_t(v.current())));
                        break;
                case ChooseClose: closeWindow(); break;
                case ChooseDeleteForGood:
                        selectedPaths(paths);
                        if (!paths.isEmpty()) actions.removeForGood(paths.view());
                        break;
                case ChooseCut:
                case ChooseCopy:
                        selectedPaths(paths);
                        if (paths.isEmpty()) break;
                        if (choice == ChooseCut) {
                                actions.cut(paths.view());
                        } else {
                                actions.copy(paths.view());
                        }
                        break;
                case ChoosePaste: actions.paste(path()); break;
                case ChooseSelectAll:
                case ChooseInvert: {
                        List<size_t> rows(MemSpace::Psram);
                        for (size_t r = 0; r < v.count(); ++r)
                                if (choice == ChooseSelectAll || !v.selection().isSelected(r)) (void)rows.append(r);
                        v.resetTo(rows.span(), v.current());
                        break;
                }
                case ChooseIcons:
                case ChooseDetails:
                        showView(choice == ChooseIcons ? FolderSettings::View::Icons : FolderSettings::View::Details);
                        remember();
                        break;
                case ChooseSortName: setSort(FolderModel::Sort::Name, false); break;
                case ChooseSortType: setSort(FolderModel::Sort::Type, false); break;
                case ChooseSortSize: setSort(FolderModel::Sort::Size, false); break;
                case ChooseSortModified: setSort(FolderModel::Sort::Modified, false); break;
                case ChooseFolders:
                        _treeShown = !_treeShown;
                        _tree->setVisible(_treeShown);
                        _split->setWidgets(_tree, _icons->parent());
                        break;
                case ChooseStatusBar:
                        _statusShown = !_statusShown;
                        _status->setVisible(_statusShown);
                        static_cast<BoxLayout *>(_status->parent())->relayout();
                        break;
                case ChooseRefresh: refresh(); break;
                case ChooseBack:
                case ChooseForward: {
                        // Moved along the history first: going there doesn't add to it.
                        const StringView to = choice == ChooseBack ? _history.back() : _history.forward();
                        String           target(MemSpace::Psram);
                        if (!to.isEmpty() && target.assign(to).isOk() && go(target.view()).isError())
                                (void)(choice == ChooseBack ? _history.forward() : _history.back());
                        updateTools();
                        break;
                }
                case ChooseUp:
                        if (canGoUp()) {
                                // The folder come from selected in its parent.
                                String from(MemSpace::Psram);
                                if (from.assign(path()).isOk())
                                        (void)go(NavigationHistory::parentOf(from.view()),
                                                 FileOps::baseName(from.view()));
                        }
                        break;
                case ChooseHome: (void)go(HomePath); break;
                case ChooseDesktop: (void)go(DesktopPath); break;
                case ChooseAbout:
                        actions.tell("About Files",
                                     "Files: the desktop's folder windows. Back, Forward and Up go about; "
                                     "Shift and a double click opens a folder in a window of its own.",
                                     false);
                        break;
                default: break;
        }
        return;
}

bool FolderWindow::keyUnused(const KeyEvent &event) {
        const bool alt = event.hasAlt() && !event.hasCtrl() && !event.hasShift();
        const bool ctrl = event.hasCtrl() && !event.hasAlt() && !event.hasShift();
        const bool plain = !event.hasAlt() && !event.hasCtrl() && !event.hasShift();
        if (alt && event.usage == HidKey::Left) {
                choose(ChooseBack);
        } else if (alt && event.usage == HidKey::Right) {
                choose(ChooseForward);
        } else if ((alt && event.usage == HidKey::Up) || (plain && event.usage == HidKey::Backspace)) {
                choose(ChooseUp);
        } else if (plain && event.usage == F5) {
                choose(ChooseRefresh);
        } else if ((ctrl && event.usage == KeyL) || (alt && event.usage == KeyD) || (plain && event.usage == F4)) {
                _address->setFocus();
                _address->selectAll();
        } else if (ctrl && event.usage == KeyW) {
                closeWindow();
        } else {
                return false;
        }
        return true;
}

void FolderWindow::closeWindow() {
        if (_treeShown) {
                // The tree's width, if moved, for the folder's next window.
                FolderSettings::Values v;
                if (_host.folderSettings().find(path(), v) && v.tree != _split->position()) remember();
        }
        close();
        _host.folderClosed(*this);
        return;
}

void FolderWindow::closedByManager() {
        GuiWindow::closedByManager();
        _host.folderClosed(*this);
        return;
}

// ---- The status bar and the toolbar

void FolderWindow::updateStatus() {
        const FolderModel &folder = _items.folder();
        char               text[96];
        const size_t       hidden = folder.hiddenCount();
        int n = std::snprintf(text, sizeof(text), "%zu item%s", folder.count(), folder.count() == 1 ? "" : "s");
        if (hidden > 0 && n > 0 && size_t(n) < sizeof(text))
                n += std::snprintf(text + n, sizeof(text) - size_t(n), " (%zu hidden)", hidden);
        if (!folder.isComplete() && n > 0 && size_t(n) < sizeof(text))
                std::snprintf(text + n, sizeof(text) - size_t(n), ", reading...");
        _status->setText(0, StringView(text));
        // The selection and its files' size.
        const ItemView &v = view();
        const size_t    selected = v.selection().selectedCount();
        uint64_t        bytes = 0;
        size_t          files = 0;
        for (int r = v.selection().nextSelected(0); r >= 0; r = v.selection().nextSelected(size_t(r) + 1)) {
                if (size_t(r) >= folder.count() || folder.entry(size_t(r)).type != FileType::File) continue;
                bytes += folder.entry(size_t(r)).size;
                ++files;
        }
        char size[32];
        formatBytes(bytes, size, sizeof(size));
        if (selected > 0 && files > 0) {
                std::snprintf(text, sizeof(text), "%zu selected, %s", selected, size);
        } else if (selected > 0) {
                std::snprintf(text, sizeof(text), "%zu selected", selected); // folders: no size of their own
        } else {
                text[0] = '\0';
        }
        _status->setText(1, StringView(text));
        if (_hasFree) {
                formatBytes(_freeBytes, size, sizeof(size));
                std::snprintf(text, sizeof(text), "%s free", size);
        } else {
                text[0] = '\0';
        }
        _status->setText(2, StringView(text));
        return;
}

void FolderWindow::updateTools() {
        if (_back != nullptr) _back->setEnabled(canGoBack());
        if (_forward != nullptr) _forward->setEnabled(canGoForward());
        if (_up != nullptr) _up->setEnabled(canGoUp());
        return;
}

// ---- The shared menus

FolderMenus::FolderMenus(GuiApplication &app)
    : _app(app), _file(app), _edit(app), _view(app), _go(app), _help(app), _new(app), _arrange(app), _views(app),
      _item(app), _background(app) {
        for (Menu *m : {&_file, &_edit, &_view, &_go, &_help, &_new, &_arrange, &_views, &_item, &_background})
                m->setTriggered(triggered, this);
}

Error FolderMenus::attach(MenuBar &bar) {
        const struct {
                        const char *title;
                        Menu       *menu;
        } titles[] = {{"&File", &_file}, {"&Edit", &_edit}, {"&View", &_view}, {"&Go", &_go}, {"&Help", &_help}};
        for (const auto &t : titles) {
                const Error err = bar.addMenu(t.title, *t.menu);
                if (err.isError()) return err;
        }
        bar.setAboutToShow(aboutToShow, this);
        return Error::Ok;
}

void FolderMenus::aboutToShow(void *arg, MenuBar &bar, size_t index, Menu &menu) {
        (void)index;
        auto *self = static_cast<FolderMenus *>(arg);
        auto *window = static_cast<FolderWindow *>(bar.window()); // a folder window's bar
        if (window == nullptr) return;
        self->_owner = window;
        self->fill(menu, *window);
        return;
}

void FolderMenus::popup(FolderWindow &window, bool item, uint16_t x, uint16_t y) {
        _owner = &window;
        Menu &menu = item ? _item : _background;
        fill(menu, window);
        (void)menu.popup(Rect{x, y, 0, 0});
        return;
}

void FolderMenus::forget(FolderWindow &window) {
        if (_owner != &window) return;
        for (Menu *m : {&_file, &_edit, &_view, &_go, &_help, &_item, &_background})
                if (m->isShown()) m->dismiss();
        _owner = nullptr;
        return;
}

void FolderMenus::triggered(void *arg, int choice) {
        auto *self = static_cast<FolderMenus *>(arg);
        if (self->_owner != nullptr) self->_owner->choose(choice);
        return;
}

// @p menu's items as they are for @p window now (and its submenus').
void FolderMenus::fill(Menu &menu, FolderWindow &w) {
        using W = FolderWindow;
        constexpr uint8_t Off = MenuLayout::Disabled, On = MenuLayout::Checked;
        const size_t      selected = w.selectedCount();
        const uint8_t     needOne = selected > 0 ? 0 : Off;
        const uint8_t     paste = w.canPaste() ? 0 : Off;
        const auto        checked = [](bool on) {
                return on ? On : uint8_t(0);
        };
        // Items' icons, the theme's (made again at each showing: a new theme drops the old ones').
        const auto icon = [this](IconId id) {
                return _app.icons().find(id, 16);
        };
        // The submenus first: they show as they are.
        _new.clear();
        (void)_new.addItem("&Folder", W::ChooseNewFolder, 0, StringView(), icon(IconId::Folder));
        (void)_new.addItem("&Text Document", W::ChooseNewText, 0, StringView(), icon(IconId::TextXGeneric));
        (void)_new.addItem("&Empty File", W::ChooseNewFile, 0, StringView(), icon(IconId::Unknown));
        _arrange.clear();
        const FolderModel::Sort sort = w.sort();
        (void)_arrange.addItem("by &Name", W::ChooseSortName, checked(sort == FolderModel::Sort::Name));
        (void)_arrange.addItem("by &Type", W::ChooseSortType, checked(sort == FolderModel::Sort::Type));
        (void)_arrange.addItem("by &Size", W::ChooseSortSize, checked(sort == FolderModel::Sort::Size));
        (void)_arrange.addItem("by &Modified", W::ChooseSortModified, checked(sort == FolderModel::Sort::Modified));
        _views.clear();
        (void)_views.addItem("Lar&ge Icons", W::ChooseIcons, checked(!w.showsDetails()));
        (void)_views.addItem("&Details", W::ChooseDetails, checked(w.showsDetails()));
        menu.clear();
        if (&menu == &_file) {
                (void)menu.addItem("&Open", W::ChooseOpen, needOne, "Enter", icon(IconId::DocumentOpen));
                if (w.applicationSelected())
                        (void)menu.addItem("Show Co&ntents", W::ChooseShowContents, 0, StringView(),
                                           icon(IconId::Folder));
                (void)menu.addItem("Open in New &Window", W::ChooseOpenNew, w.folderSelected() ? 0 : Off, "Shift+Enter",
                                   icon(IconId::WindowNew));
                (void)menu.addSeparator();
                (void)menu.addSubmenu("Ne&w", _new, icon(IconId::DocumentNew));
                (void)menu.addSeparator();
                (void)menu.addItem("&Delete", W::ChooseDelete, needOne, "Del", icon(IconId::EditDelete));
                (void)menu.addItem("Rena&me", W::ChooseRename, selected == 1 ? 0 : Off, "F2", icon(IconId::EditRename));
                (void)menu.addSeparator();
                (void)menu.addItem("&Close", W::ChooseClose, 0, "Ctrl+W", icon(IconId::WindowClose));
        } else if (&menu == &_edit) {
                (void)menu.addItem("Cu&t", W::ChooseCut, needOne, "Ctrl+X", icon(IconId::EditCut));
                (void)menu.addItem("&Copy", W::ChooseCopy, needOne, "Ctrl+C", icon(IconId::EditCopy));
                (void)menu.addItem("&Paste", W::ChoosePaste, paste, "Ctrl+V", icon(IconId::EditPaste));
                (void)menu.addSeparator();
                (void)menu.addItem("Select &All", W::ChooseSelectAll, 0, "Ctrl+A", icon(IconId::EditSelectAll));
                (void)menu.addItem("&Invert Selection", W::ChooseInvert);
        } else if (&menu == &_view) {
                (void)menu.addItem("Lar&ge Icons", W::ChooseIcons, checked(!w.showsDetails()));
                (void)menu.addItem("&Details", W::ChooseDetails, checked(w.showsDetails()));
                (void)menu.addSeparator();
                (void)menu.addSubmenu("Arrange &Icons", _arrange, icon(IconId::ViewSortAscending));
                (void)menu.addSeparator();
                (void)menu.addItem("&Folders", W::ChooseFolders, checked(w.showsTree()));
                (void)menu.addItem("&Status Bar", W::ChooseStatusBar, checked(w.showsStatusBar()));
                (void)menu.addSeparator();
                (void)menu.addItem("&Refresh", W::ChooseRefresh, 0, "F5", icon(IconId::ViewRefresh));
        } else if (&menu == &_go) {
                (void)menu.addItem("&Back", W::ChooseBack, w.canGoBack() ? 0 : Off, "Alt+Left",
                                   icon(IconId::GoPrevious));
                (void)menu.addItem("&Forward", W::ChooseForward, w.canGoForward() ? 0 : Off, "Alt+Right",
                                   icon(IconId::GoNext));
                (void)menu.addItem("&Up One Level", W::ChooseUp, w.canGoUp() ? 0 : Off, "Backspace",
                                   icon(IconId::GoUp));
                (void)menu.addSeparator();
                (void)menu.addItem("&Home", W::ChooseHome, 0, StringView(), icon(IconId::UserHome));
                (void)menu.addItem("&Desktop", W::ChooseDesktop, 0, StringView(), icon(IconId::UserDesktop));
        } else if (&menu == &_item) {
                (void)menu.addItem("&Open", W::ChooseOpen, 0, StringView(), icon(IconId::DocumentOpen));
                if (w.applicationSelected())
                        (void)menu.addItem("Show Co&ntents", W::ChooseShowContents, 0, StringView(),
                                           icon(IconId::Folder));
                (void)menu.addItem("Open in New &Window", W::ChooseOpenNew, w.folderSelected() ? 0 : Off, StringView(),
                                   icon(IconId::WindowNew));
                (void)menu.addSeparator();
                (void)menu.addItem("Cu&t", W::ChooseCut, 0, StringView(), icon(IconId::EditCut));
                (void)menu.addItem("&Copy", W::ChooseCopy, 0, StringView(), icon(IconId::EditCopy));
                (void)menu.addSeparator();
                (void)menu.addItem("&Delete", W::ChooseDelete, 0, StringView(), icon(IconId::EditDelete));
                (void)menu.addItem("Rena&me", W::ChooseRename, selected == 1 ? 0 : Off, StringView(),
                                   icon(IconId::EditRename));
        } else if (&menu == &_background) {
                (void)menu.addSubmenu("&View", _views, icon(IconId::ViewListIcons));
                (void)menu.addSubmenu("Arrange &Icons", _arrange, icon(IconId::ViewSortAscending));
                (void)menu.addSeparator();
                (void)menu.addItem("&Paste", W::ChoosePaste, paste, StringView(), icon(IconId::EditPaste));
                (void)menu.addSeparator();
                (void)menu.addSubmenu("Ne&w", _new, icon(IconId::DocumentNew));
        } else if (&menu == &_help) {
                (void)menu.addItem("&About Files", W::ChooseAbout, 0, StringView(), icon(IconId::HelpAbout));
        }
        return;
}

BMDS_NAMESPACE_END
