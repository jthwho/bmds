/**
 * @file      widgetsapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/widgets: a window with the toolkit's furniture and item views, to try them
 * (devplan/filebrowser.md §4e, §4g). A menu bar, a tool bar, a splitter between a folder tree and
 * the folder's files as icons or details (View), a status bar, and two dialogs: a question, and a
 * progress bar with Cancel.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/detailsview.h>
#include <bmds/dialog.h>
#include <bmds/fdtable.h>
#include <bmds/foldermodel.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/iconview.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/menubar.h>
#include <bmds/treeview.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t ProgressMs = 50;
        constexpr size_t   FillBatch = 64; // entries read a turn (FB12)

        enum Choice : int {
                Question = 1,
                Copy,
                Quit,
                ShowStatus,
                LargeIcons,
                Details,
                About,
        };

        // An icon of the theme, copied (its cache may drop its own).
        struct OwnedIcon {
                        Icon      icon;
                        uint32_t *pixels = nullptr;

                        void take(const Icon *from) {
                                if (from == nullptr || !from->isValid()) return;
                                const size_t n = size_t(from->width) * from->height;
                                pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                                if (pixels == nullptr) return;
                                std::memcpy(pixels, from->pixels, n * sizeof(uint32_t));
                                icon = Icon{from->width, from->height, pixels};
                        }

                        const Icon *get() const { return pixels != nullptr ? &icon : nullptr; }
                        ~OwnedIcon() { Memory::free(pixels); }
        };

        // The folder's entries as the views' model: the folder model's, with sizes and types written out.
        class FilesModel final : public ItemModel {
                public:
                        FolderModel folder;
                        const Icon *folderSmall = nullptr, *folderLarge = nullptr;
                        const Icon *fileSmall = nullptr, *fileLarge = nullptr;

                        size_t      count() const override { return folder.count(); }
                        StringView  text(size_t row) const override { return folder.name(row); }
                        const Icon *icon(size_t row, uint16_t size) const override {
                                const bool dir = folder.entry(row).isDirectory();
                                return size > 16 ? (dir ? folderLarge : fileLarge) : (dir ? folderSmall : fileSmall);
                        }
                        size_t     columns() const override { return 3; }
                        StringView columnTitle(size_t column) const override {
                                return column == 0 ? StringView("Name") : column == 1 ? StringView("Size") : "Type";
                        }
                        uint16_t columnWidth(size_t column) const override {
                                return column == 0 ? 200 : column == 1 ? 80 : 136;
                        }
                        Align    columnAlign(size_t column) const override {
                                return column == 1 ? Align::Right : Align::Left;
                        }
                        StringView columnText(size_t row, size_t column, Span<char> buffer) const override {
                                const FolderModel::Entry &e = folder.entry(row);
                                if (column == 0) return folder.name(row);
                                if (column == 1) {
                                        if (e.isDirectory() || e.builtin) return StringView();
                                        const uint64_t kb = (e.size + 1023) / 1024;
                                        const int      n = std::snprintf(buffer.data(), buffer.size(), "%llu KB",
                                                                         static_cast<unsigned long long>(kb));
                                        return StringView(buffer.data(), n > 0 ? size_t(n) : 0);
                                }
                                if (e.isDirectory()) return "Folder";
                                if (e.builtin) return "Built-in program";
                                return FolderModel::extensionOf(folder.name(row));
                        }
        };

        class WidgetsWindow final : public GuiWindow {
                public:
                        WidgetsWindow(GuiApplication &app, const Config &config)
                            : GuiWindow(app, config), _file(app), _view(app), _help(app),
                              _question(app, "Question", 280, 40), _copying(app, "Copying", 280, 48) {}

                        ~WidgetsWindow() override { stopFill(); }

                        Error build();

                protected:
                        void closedByManager() override {
                                application().quit(0);
                                return;
                        }

                private:
                        static void chosen(void *arg, int item);
                        static void tool(void *arg, Button &button);
                        static void answered(void *arg, Dialog &dialog, int result);
                        static void copied(void *arg, Dialog &dialog, int result);
                        static void tick(void *arg);
                        static void expanding(void *arg, TreeView &view, size_t row);
                        static void folderChosen(void *arg, ItemView &view);
                        static void opened(void *arg, ItemView &view, size_t row);
                        static void selectionChanged(void *arg, ItemView &view);
                        static void sortAsked(void *arg, DetailsView &view, size_t column);
                        static void fill(void *arg);
                        void        startCopy();
                        void        status(const char *text);
                        void        showFolder(StringView path);
                        void        stopFill();
                        void        filesChanged(); // the views reset to the model, the status told
                        void        setDetails(bool details);

                        Menu         _file;
                        Menu         _view;
                        Menu         _help;
                        Dialog       _question;
                        Dialog       _copying;
                        StatusBar   *_status = nullptr;
                        ProgressBar *_progress = nullptr;
                        TreeView    *_tree = nullptr;
                        IconView    *_icons = nullptr;
                        DetailsView *_details = nullptr;
                        Splitter    *_split = nullptr;
                        FilesModel   _files;
                        OwnedIcon    _owned[6];
                        int          _timer = -1;
                        int          _fillTimer = -1;
                        uint32_t     _done = 0;
                        bool         _statusShown = true;
                        bool         _showDetails = false;
        };

        Error WidgetsWindow::build() {
                auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
                if (column == nullptr) return Error::NoMem;
                setCentral(column);
                auto *bar = Widget::create<MenuBar>(column);
                auto *tools = Widget::create<ToolBar>(column);
                _split = Widget::create<Splitter>(column, Splitter::Orientation::Horizontal);
                _status = Widget::create<StatusBar>(column);
                if (bar == nullptr || tools == nullptr || _split == nullptr || _status == nullptr) return Error::NoMem;

                (void)_file.addItem("&Question...", Question);
                (void)_file.addItem("&Copy...", Copy);
                (void)_file.addSeparator();
                (void)_file.addItem("E&xit", Quit, 0, "Alt+F4");
                for (Menu *m : {&_file, &_view, &_help}) m->setTriggered(chosen, this);
                (void)_help.addItem("&About", About);
                (void)bar->addMenu("&File", _file);
                (void)bar->addMenu("&View", _view);
                (void)bar->addMenu("&Help", _help);
                setMenuBar(bar);

                (void)tools->addButton(nullptr, "Icons", tool, this);
                (void)tools->addButton(nullptr, "Details", tool, this);
                (void)tools->addSeparator();
                (void)tools->addButton(nullptr, "Question", tool, this);
                (void)tools->addButton(nullptr, "Copy", tool, this);

                // The icons, copied once.
                IconTheme &theme = application().icons();
                _owned[0].take(theme.find("folder", 16));
                _owned[1].take(theme.find("folder", 32));
                _owned[2].take(theme.find("text-x-generic", 16));
                _owned[3].take(theme.find("text-x-generic", 32));
                _owned[4].take(theme.find("drive-harddisk", 16));
                _owned[5].take(theme.find("folder-open", 16));
                _files.folderSmall = _owned[0].get();
                _files.folderLarge = _owned[1].get();
                _files.fileSmall = _owned[2].get();
                _files.fileLarge = _owned[3].get();

                _tree = Widget::create<TreeView>(_split);
                // The files' side: both views, one shown.
                auto *right = Widget::create<BoxLayout>(_split, BoxLayout::Direction::Vertical);
                if (_tree == nullptr || right == nullptr) return Error::NoMem;
                _icons = Widget::create<IconView>(right);
                _details = Widget::create<DetailsView>(right);
                if (_icons == nullptr || _details == nullptr) return Error::NoMem;
                for (ItemView *v : {static_cast<ItemView *>(_icons), static_cast<ItemView *>(_details)}) {
                        v->setStretch(1);
                        v->setModel(&_files);
                        v->setActivated(opened, this);
                        v->setChanged(selectionChanged, this);
                }
                _details->setSortAsked(sortAsked, this);
                _details->setSortIndicator(0, false);
                _tree->setIcons(_owned[4].get(), _owned[0].get(), _owned[5].get());
                _tree->setExpanding(expanding, this);
                _tree->setChanged(folderChosen, this);
                // One root, /: the filesystems and the rest under it.
                (void)_tree->addRoot("/");
                _split->setLeast(120, 160);
                _split->setPosition(200);
                _split->setWidgets(_tree, right);
                _split->setStretch(1);

                (void)_status->addField(0);
                (void)_status->addField(160);
                status("Ready");

                (void)Widget::create<Label>(_question.content(), "Is the toolkit furnished now?");
                _question.addButton("&Yes", 1);
                _question.addButton("&No", 0);
                _question.setFinished(answered, this);

                (void)Widget::create<Label>(_copying.content(), "Copying 100 files...");
                _progress = Widget::create<ProgressBar>(_copying.content());
                _copying.addButton("&Cancel", 0);
                _copying.setFinished(copied, this);
                setDetails(false);
                column->relayout();
                _tree->setFocus();
                _tree->expand(0);
                _tree->selectOnly(0); // / shown
                return Error::Ok;
        }

        void WidgetsWindow::status(const char *text) {
                _status->setText(0, StringView(text));
                return;
        }

        void WidgetsWindow::setDetails(bool details) {
                _showDetails = details;
                _icons->setVisible(!details);
                _details->setVisible(details);
                static_cast<BoxLayout *>(_icons->parent())->relayout();
                _view.clear();
                (void)_view.addItem("&Large Icons", LargeIcons, details ? 0 : MenuLayout::Checked);
                (void)_view.addItem("&Details", Details, details ? MenuLayout::Checked : 0);
                (void)_view.addSeparator();
                (void)_view.addItem("&Status Bar", ShowStatus, _statusShown ? MenuLayout::Checked : 0);
                return;
        }

        void WidgetsWindow::chosen(void *arg, int item) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                switch (item) {
                        case Question: (void)self->_question.open(); break;
                        case Copy: self->startCopy(); break;
                        case Quit: self->application().quit(0); break;
                        case LargeIcons: self->setDetails(false); break;
                        case Details: self->setDetails(true); break;
                        case ShowStatus:
                                self->_statusShown = !self->_statusShown;
                                self->_status->setVisible(self->_statusShown);
                                static_cast<BoxLayout *>(self->_status->parent())->relayout();
                                self->setDetails(self->_showDetails);
                                break;
                        case About: self->status("BMDS widgets: the toolkit's furniture and views"); break;
                        default: break;
                }
                return;
        }

        void WidgetsWindow::tool(void *arg, Button &button) {
                auto      *self = static_cast<WidgetsWindow *>(arg);
                const auto text = button.text();
                if (text == "Question") {
                        (void)self->_question.open();
                } else if (text == "Copy") {
                        self->startCopy();
                } else {
                        self->setDetails(text == "Details");
                }
                return;
        }

        // A folder in the tree expanded: its folders, as the tree's children.
        void WidgetsWindow::expanding(void *arg, TreeView &view, size_t row) {
                (void)arg;
                String path(MemSpace::Psram);
                if (view.rows().pathOf(row, path).isError()) return;
                FolderModel sub;
                if (sub.open(path.view(), Vfs::Context{"/", nullptr}).isError()) {
                        (void)view.setChildren(row, Span<const StringView>());
                        return;
                }
                while (!sub.isComplete()) (void)sub.fillSome(FillBatch);
                List<StringView> names(MemSpace::Psram);
                for (size_t i = 0; i < sub.count(); ++i)
                        if (sub.entry(i).isDirectory()) (void)names.append(sub.name(i));
                (void)view.setChildren(row, names.span());
                return;
        }

        void WidgetsWindow::folderChosen(void *arg, ItemView &view) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                auto &tree = static_cast<TreeView &>(view);
                if (tree.current() < 0) return;
                String path(MemSpace::Psram);
                if (tree.rows().pathOf(size_t(tree.current()), path).isOk() &&
                    path.view() != self->_files.folder.path())
                        self->showFolder(path.view());
                return;
        }

        void WidgetsWindow::showFolder(StringView path) {
                stopFill();
                if (_files.folder.open(path, Vfs::Context{"/", nullptr}).isError()) {
                        status("Can't open that folder");
                        filesChanged();
                        return;
                }
                filesChanged();
                // Filled a batch at a time on the loop (FB12): shown as it fills.
                auto [timer, err] = application().startTimer(1, true, fill, this);
                _fillTimer = err.isOk() ? timer : -1;
                return;
        }

        void WidgetsWindow::stopFill() {
                if (_fillTimer >= 0) application().stopTimer(_fillTimer);
                _fillTimer = -1;
                return;
        }

        void WidgetsWindow::fill(void *arg) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                (void)self->_files.folder.fillSome(FillBatch);
                if (self->_files.folder.isComplete()) self->stopFill();
                self->filesChanged();
                return;
        }

        void WidgetsWindow::filesChanged() {
                _icons->modelReset();
                _details->modelReset();
                char text[64];
                std::snprintf(text, sizeof(text), "%zu items, %llu KB%s", _files.folder.count(),
                              static_cast<unsigned long long>((_files.folder.shownBytes() + 1023) / 1024),
                              _files.folder.isComplete() ? "" : " (reading)");
                _status->setText(1, StringView(text));
                return;
        }

        void WidgetsWindow::opened(void *arg, ItemView &view, size_t row) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                (void)view;
                if (!self->_files.folder.entry(row).isDirectory()) {
                        self->status("A file: opened by the folder windows (step 4)");
                        return;
                }
                // Into the folder: the tree follows if it shows it.
                String path(MemSpace::Psram);
                (void)path.assign(self->_files.folder.path());
                if (!path.view().endsWith('/')) (void)path.append('/');
                (void)path.append(self->_files.folder.name(row));
                if (self->_tree->current() >= 0) self->_tree->expand(size_t(self->_tree->current()));
                if (self->_tree->selectPath(path.view()) < 0) self->showFolder(path.view());
                return;
        }

        void WidgetsWindow::selectionChanged(void *arg, ItemView &view) {
                auto        *self = static_cast<WidgetsWindow *>(arg);
                char         text[64];
                const size_t n = view.selection().selectedCount();
                if (n == 1 && view.current() >= 0) {
                        const StringView name = self->_files.folder.name(size_t(view.current()));
                        std::snprintf(text, sizeof(text), "%.*s", int(name.size()), name.data());
                } else {
                        std::snprintf(text, sizeof(text), "%zu selected", n);
                }
                self->status(text);
                return;
        }

        void WidgetsWindow::sortAsked(void *arg, DetailsView &view, size_t column) {
                auto                   *self = static_cast<WidgetsWindow *>(arg);
                const FolderModel::Sort sorts[] = {FolderModel::Sort::Name, FolderModel::Sort::Size,
                                                   FolderModel::Sort::Type};
                const FolderModel::Sort sort = sorts[column < 3 ? column : 0];
                const bool down = self->_files.folder.sort() == sort && !self->_files.folder.isDescending();
                self->_files.folder.setSort(sort, down);
                view.setSortIndicator(int(column), down);
                self->filesChanged();
                return;
        }

        void WidgetsWindow::answered(void *arg, Dialog &, int result) {
                static_cast<WidgetsWindow *>(arg)->status(result == 1 ? "Answered yes" : "Answered no");
                return;
        }

        void WidgetsWindow::startCopy() {
                if (_copying.isShown()) return;
                _done = 0;
                if (_progress != nullptr) _progress->setValue(0);
                if (_copying.open().isError()) return;
                auto [id, err] = application().startTimer(ProgressMs, true, tick, this);
                _timer = err.isOk() ? id : -1;
                return;
        }

        void WidgetsWindow::tick(void *arg) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                if (++self->_done > 100) {
                        self->_copying.finish(1);
                        return;
                }
                if (self->_progress != nullptr) self->_progress->setValue(self->_done);
                return;
        }

        void WidgetsWindow::copied(void *arg, Dialog &, int result) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                self->application().stopTimer(self->_timer);
                self->_timer = -1;
                self->status(result == 1 ? "Copied" : "Copy cancelled");
                return;
        }

        int widgetsMain(const BuiltinProgram::Context &context) {
                FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                const auto  say = [err](const char *text) {
                        if (err != nullptr)
                                (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                  WaitForever);
                };
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                int   status = 1;
                if (app->connect(0).isError()) {
                        say("widgets: the window system isn't running ('win start')\n");
                } else {
                        GuiWindow::Config config;
                        config.rect = Rect{120, 80, 720, 480};
                        config.title = "Widgets";
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(WidgetsWindow));
                        if (wmem != nullptr) {
                                auto *window = ::new (wmem) WidgetsWindow(*app, config);
                                Error e;
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        e = window->build();
                                        if (e.isOk()) e = window->show();
                                }
                                if (e.isOk()) {
                                        status = app->exec();
                                } else {
                                        say("widgets: can't open its window\n");
                                }
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        window->~WidgetsWindow();
                                }
                                Memory::free(wmem);
                        }
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM(widgets, widgetsMain, 12 * 1024)

BMDS_NAMESPACE_END
