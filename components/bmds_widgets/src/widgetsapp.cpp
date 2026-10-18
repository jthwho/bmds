/**
 * @file      widgetsapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/widgets: a window with the toolkit's furniture and item views, to try them
 * (devplan/filebrowser.md §4e, §4g; devplan/mediaplayer.md §6d). A menu bar, a tool bar, tabs: a
 * splitter between a folder tree and the folder's files as icons or details (View), controls (a
 * combo box, a seek bar told on release), and a form (devplan/gui.md §4u: sections, labelled rows
 * with help, number fields, color swatches and the color dialog, list editors, scrolled by rows); a
 * status bar; dialogs: a question, a progress bar with Cancel, and the file dialog's four kinds.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <atomic>
#include <bmds/builtinprograms.h>
#include <bmds/colordialog.h>
#include <bmds/combobox.h>
#include <bmds/detailsview.h>
#include <bmds/dialog.h>
#include <bmds/fdtable.h>
#include <bmds/filedialog.h>
#include <bmds/fileicons.h>
#include <bmds/folderitemmodel.h>
#include <bmds/foldermodel.h>
#include <bmds/formlayout.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/iconview.h>
#include <bmds/lineedit.h>
#include <bmds/listeditor.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/menubar.h>
#include <bmds/numberfield.h>
#include <bmds/slider.h>
#include <bmds/tabwidget.h>
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
                OpenFile,
                OpenFiles,
                SaveFile,
                ChooseFolder,
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

        class WidgetsWindow final : public GuiWindow {
                public:
                        WidgetsWindow(GuiApplication &app, const Config &config)
                            : GuiWindow(app, config), _file(app), _view(app), _help(app),
                              _question(app, "Question", 280, 40), _copying(app, "Copying", 280, 48),
                              _openOne(app, FileDialog::Mode::Open, "Open"),
                              _openMany(app, FileDialog::Mode::OpenMany, "Open Several"),
                              _save(app, FileDialog::Mode::Save, "Save As"),
                              _folder(app, FileDialog::Mode::Folder, "Choose a Folder"), _colors(app, "Color"),
                              _fileIcons(app.icons()) {
                                _fileIcons.setAsked(iconsAsked, this);
                        }

                        ~WidgetsWindow() override {
                                stopFill();
                                application().cancelPosts(this);
                        }

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
                        static void iconsAsked(void *arg);
                        static void iconsDue(void *arg);
                        static void fileChosen(void *arg, FileDialog &dialog, int result);
                        static void comboChanged(void *arg, ComboBox &box, int index);
                        static void seekMoved(void *arg, Slider &slider, int32_t value);
                        static void seekReleased(void *arg, Slider &slider, int32_t value);
                        static void tabChanged(void *arg, TabWidget &tabs, size_t index);
                        static void numberChanged(void *arg, NumberField &field, int32_t value);
                        static void volumeMoved(void *arg, Slider &slider, int32_t value);
                        static void swatchClicked(void *arg, ColorSwatch &swatch);
                        static void colorChanged(void *arg, ColorDialog &dialog, uint32_t rgb);
                        static void colorFinished(void *arg, Dialog &dialog, int result);
                        static void listChanged(void *arg, ListEditor &editor);
                        static void folderAsked(void *arg, ListEditor &editor);
                        Error       buildControls(BoxLayout *page);
                        Error       buildForm(FormLayout *form);
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
                        FileDialog        _openOne;
                        FileDialog        _openMany;
                        FileDialog        _save;
                        FileDialog        _folder;
                        ColorDialog       _colors;
                        FileIcons         _fileIcons;
                        FolderItemModel   _files{_fileIcons};
                        TabWidget        *_tabs = nullptr;
                        Label            *_seekLabel = nullptr;
                        Label            *_volumeLabel = nullptr;
                        ColorSwatch      *_swatch = nullptr; // the one the color dialog changes
                        ListEditor       *_folders = nullptr;
                        bool              _folderForList = false;
                        std::atomic<bool> _iconsPosted{false};
                        StatusBar   *_status = nullptr;
                        ProgressBar *_progress = nullptr;
                        TreeView    *_tree = nullptr;
                        IconView    *_icons = nullptr;
                        DetailsView *_details = nullptr;
                        Splitter    *_split = nullptr;
                        OwnedIcon         _owned[3];
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
                _tabs = Widget::create<TabWidget>(column);
                _status = Widget::create<StatusBar>(column);
                if (bar == nullptr || tools == nullptr || _tabs == nullptr || _status == nullptr) return Error::NoMem;
                _tabs->setStretch(1);
                _split = Widget::create<Splitter>(_tabs, Splitter::Orientation::Horizontal);
                auto *controls = Widget::create<BoxLayout>(_tabs, BoxLayout::Direction::Vertical);
                auto *form = Widget::create<FormLayout>(_tabs);
                if (_split == nullptr || controls == nullptr || form == nullptr) return Error::NoMem;
                (void)_tabs->addTab(_split, "Files");
                (void)_tabs->addTab(controls, "Controls");
                (void)_tabs->addTab(form, "Form");
                _tabs->setChanged(tabChanged, this);

                IconTheme &theme = application().icons();
                (void)_file.addItem("&Open...", OpenFile, 0, StringView(), theme.find(IconId::DocumentOpen, 16));
                (void)_file.addItem("Open &Several...", OpenFiles, 0, StringView(),
                                    theme.find(IconId::DocumentOpen, 16));
                (void)_file.addItem("Save &As...", SaveFile, 0, StringView(), theme.find(IconId::DocumentSaveAs, 16));
                (void)_file.addItem("Choose &Folder...", ChooseFolder, 0, StringView(), theme.find(IconId::Folder, 16));
                (void)_file.addSeparator();
                (void)_file.addItem("&Question...", Question, 0, StringView(), theme.find(IconId::DialogQuestion, 16));
                (void)_file.addItem("&Copy...", Copy, 0, StringView(), theme.find(IconId::EditCopy, 16));
                (void)_file.addSeparator();
                (void)_file.addItem("E&xit", Quit, 0, "Alt+F4", theme.find(IconId::SystemLogOut, 16));
                for (Menu *m : {&_file, &_view, &_help}) m->setTriggered(chosen, this);
                (void)_help.addItem("&About", About, 0, StringView(), theme.find(IconId::HelpAbout, 16));
                (void)bar->addMenu("&File", _file);
                (void)bar->addMenu("&View", _view);
                (void)bar->addMenu("&Help", _help);
                setMenuBar(bar);

                (void)tools->addButton(nullptr, "Icons", tool, this);
                (void)tools->addButton(nullptr, "Details", tool, this);
                (void)tools->addSeparator();
                (void)tools->addButton(nullptr, "Question", tool, this);
                (void)tools->addButton(nullptr, "Copy", tool, this);

                // The tree's icons, copied once; the files' from the toolkit's FileIcons.
                _owned[0].take(theme.find(IconId::Folder, 16));
                _owned[1].take(theme.find(IconId::DriveHarddisk, 16));
                _owned[2].take(theme.find(IconId::FolderOpen, 16));

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
                _tree->setIcons(_owned[1].get(), _owned[0].get(), _owned[2].get());
                _tree->setExpanding(expanding, this);
                _tree->setChanged(folderChosen, this);
                // One root, /: the filesystems and the rest under it.
                (void)_tree->addRoot("/");
                _split->setLeast(120, 160);
                _split->setPosition(200);
                _split->setWidgets(_tree, right);
                if (Error e = buildControls(controls); e.isError()) return e;
                if (Error e = buildForm(form); e.isError()) return e;

                // The file dialogs: media files as the player asks for them, or all.
                for (FileDialog *d : {&_openOne, &_openMany, &_save}) {
                        if (d != &_save)
                                (void)d->addFilter("Media files", "*.mp3;*.ogg;*.flac;*.wav;*.mid;*.midi;*.m3u;*.m3u8");
                        (void)d->addFilter("Playlists (*.m3u8)", "*.m3u8;*.m3u");
                        (void)d->addFilter("All files", "");
                        d->setFinished(fileChosen, this);
                }
                (void)_save.setFileName("playlist");
                _folder.setFinished(fileChosen, this);
                _colors.setChanged(colorChanged, this);
                _colors.setFinished(colorFinished, this);

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
                        case OpenFile: (void)self->_openOne.open(); break;
                        case OpenFiles: (void)self->_openMany.open(); break;
                        case SaveFile: (void)self->_save.open(); break;
                        case ChooseFolder: (void)self->_folder.open(); break;
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
                    path.view() != self->_files.folder().path())
                        self->showFolder(path.view());
                return;
        }

        void WidgetsWindow::showFolder(StringView path) {
                stopFill();
                if (_files.folder().open(path, Vfs::Context{"/", nullptr}).isError()) {
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
                (void)self->_files.folder().fillSome(FillBatch);
                if (self->_files.folder().isComplete()) self->stopFill();
                self->filesChanged();
                return;
        }

        void WidgetsWindow::filesChanged() {
                _icons->modelReset();
                _details->modelReset();
                char text[64];
                std::snprintf(text, sizeof(text), "%zu items, %llu KB%s", _files.folder().count(),
                              static_cast<unsigned long long>((_files.folder().shownBytes() + 1023) / 1024),
                              _files.folder().isComplete() ? "" : " (reading)");
                _status->setText(1, StringView(text));
                return;
        }

        void WidgetsWindow::opened(void *arg, ItemView &view, size_t row) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                (void)view;
                if (!self->_files.folder().entry(row).isDirectory()) {
                        self->status("A file: opened by the folder windows (step 4)");
                        return;
                }
                // Into the folder: the tree follows if it shows it.
                String path(MemSpace::Psram);
                (void)path.assign(self->_files.folder().path());
                if (!path.view().endsWith('/')) (void)path.append('/');
                (void)path.append(self->_files.folder().name(row));
                if (self->_tree->current() >= 0) self->_tree->expand(size_t(self->_tree->current()));
                if (self->_tree->selectPath(path.view()) < 0) self->showFolder(path.view());
                return;
        }

        void WidgetsWindow::selectionChanged(void *arg, ItemView &view) {
                auto        *self = static_cast<WidgetsWindow *>(arg);
                char         text[64];
                const size_t n = view.selection().selectedCount();
                if (n == 1 && view.current() >= 0) {
                        const StringView name = self->_files.folder().name(size_t(view.current()));
                        std::snprintf(text, sizeof(text), "%.*s", int(name.size()), name.data());
                } else {
                        std::snprintf(text, sizeof(text), "%zu selected", n);
                }
                self->status(text);
                return;
        }

        void WidgetsWindow::sortAsked(void *arg, DetailsView &view, size_t column) {
                auto                   *self = static_cast<WidgetsWindow *>(arg);
                const FolderModel::Sort sort = self->_files.sortOf(column);
                const bool down = self->_files.folder().sort() == sort && !self->_files.folder().isDescending();
                self->_files.folder().setSort(sort, down);
                view.setSortIndicator(int(self->_files.columnOf(sort)), down);
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

        // Controls: a combo box (told its choice), a seek bar telling where it is dragged, seeking on release.
        Error WidgetsWindow::buildControls(BoxLayout *page) {
                page->setMargins(8, 8, 8, 8);
                page->setSpacing(8);
                auto *comboRow = Widget::create<BoxLayout>(page, BoxLayout::Direction::Horizontal);
                auto *seekRow = Widget::create<BoxLayout>(page, BoxLayout::Direction::Horizontal);
                if (comboRow == nullptr || seekRow == nullptr) return Error::NoMem;
                comboRow->setSpacing(8);
                seekRow->setSpacing(8);
                auto *comboLabel = Widget::create<Label>(comboRow, "&Repeat:");
                auto *combo = Widget::create<ComboBox>(comboRow);
                auto *seekLabel = Widget::create<Label>(seekRow, "S&eek:");
                auto *seek = Widget::create<Slider>(seekRow, Slider::Orientation::Horizontal);
                _seekLabel = Widget::create<Label>(seekRow, "0:00");
                if (comboLabel == nullptr || combo == nullptr || seekLabel == nullptr || seek == nullptr ||
                    _seekLabel == nullptr)
                        return Error::NoMem;
                for (Label *l : {comboLabel, seekLabel}) l->setMinimumWidth(80);
                comboLabel->setBuddy(combo);
                seekLabel->setBuddy(seek);
                (void)combo->addItem("Off");
                (void)combo->addItem("All");
                (void)combo->addItem("One");
                (void)combo->addItem("Rock & roll"); // an & shown as itself
                combo->setChanged(comboChanged, this);
                seek->setRange(0, 240);
                seek->setSteps(5, 30);
                seek->setStretch(1);
                seek->setChanged(seekMoved, this);
                seek->setReleased(seekReleased, this);
                _seekLabel->setMinimumWidth(120);
                return Error::Ok;
        }

        void WidgetsWindow::comboChanged(void *arg, ComboBox &box, int index) {
                auto            *self = static_cast<WidgetsWindow *>(arg);
                char             text[64];
                const StringView t = box.itemText(size_t(index));
                std::snprintf(text, sizeof(text), "Repeat: %.*s", int(t.size()), t.data());
                self->status(text);
                return;
        }

        void WidgetsWindow::seekMoved(void *arg, Slider &slider, int32_t value) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                char  text[32];
                std::snprintf(text, sizeof(text), "%ld:%02ld%s", long(value / 60), long(value % 60),
                              slider.isDragging() ? " (dragging)" : "");
                self->_seekLabel->setText(StringView(text));
                if (!slider.isDragging()) {
                        std::snprintf(text, sizeof(text), "Seek to %ld:%02ld", long(value / 60), long(value % 60));
                        self->status(text);
                }
                return;
        }

        void WidgetsWindow::seekReleased(void *arg, Slider &slider, int32_t value) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                char  text[32];
                (void)slider;
                std::snprintf(text, sizeof(text), "%ld:%02ld", long(value / 60), long(value % 60));
                self->_seekLabel->setText(StringView(text));
                std::snprintf(text, sizeof(text), "Released: seek to %ld:%02ld", long(value / 60), long(value % 60));
                self->status(text);
                return;
        }

        // A form: sections, labelled rows with help, every editor, more rows than fit (scrolled by rows).
        Error WidgetsWindow::buildForm(FormLayout *form) {
                (void)form->addSection("General");
                FormRow *name =
                        form->addRow("&Name:", "The name the board goes by on the network: letters, digits "
                                               "and '-', up to 63 (empty: bmds- and the serial number's last six)");
                FormRow *hidden = form->addRow(StringView());
                FormRow *edge = form->addRow("&Edge:");
                if (name == nullptr || hidden == nullptr || edge == nullptr) return Error::NoMem;
                auto *nameEdit = Widget::create<LineEdit>(name->field());
                auto *hiddenBox = Widget::create<CheckBox>(hidden->field(), "Show &hidden files");
                auto *edgeBox = Widget::create<ComboBox>(edge->field());
                if (nameEdit == nullptr || hiddenBox == nullptr || edgeBox == nullptr) return Error::NoMem;
                nameEdit->setText("bmds0");
                nameEdit->setStretch(1);
                name->label()->setBuddy(nameEdit);
                edge->label()->setBuddy(edgeBox);
                for (const char *e : {"Top", "Bottom", "Left", "Right"}) (void)edgeBox->addItem(e);
                edgeBox->setCurrent(0);

                (void)form->addSection("Numbers");
                FormRow *port = form->addRow("&Port:", "The SSH server's port");
                FormRow *volume = form->addRow("&Volume:");
                FormRow *interval = form->addRow("&Interval:", "How often it samples: Up and Down step 100, Page Up "
                                                               "and Page Down 1000; the wheel steps with the focus");
                if (port == nullptr || volume == nullptr || interval == nullptr) return Error::NoMem;
                auto *portField = Widget::create<NumberField>(port->field());
                auto *volumeSlider = Widget::create<Slider>(volume->field(), Slider::Orientation::Horizontal);
                _volumeLabel = Widget::create<Label>(volume->field(), "-9 dB");
                auto *intervalField = Widget::create<NumberField>(interval->field());
                auto *ms = Widget::create<Label>(interval->field(), "ms");
                if (portField == nullptr || volumeSlider == nullptr || _volumeLabel == nullptr ||
                    intervalField == nullptr || ms == nullptr)
                        return Error::NoMem;
                portField->setRange(1, 65535);
                portField->setSteps(1, 100);
                portField->setValue(22);
                portField->setChanged(numberChanged, this);
                port->label()->setBuddy(portField->edit());
                volumeSlider->setRange(-60, 0);
                volumeSlider->setSteps(1, 6);
                volumeSlider->setValue(-9);
                volumeSlider->setStretch(1);
                volumeSlider->setChanged(volumeMoved, this);
                volume->label()->setBuddy(volumeSlider);
                _volumeLabel->setMinimumWidth(72);
                intervalField->setRange(100, 10000);
                intervalField->setSteps(100, 1000);
                intervalField->setValue(1000);
                intervalField->setChanged(numberChanged, this);
                interval->label()->setBuddy(intervalField->edit());

                (void)form->addSection("Colors");
                const struct {
                                const char *label;
                                uint32_t    rgb;
                } colors[] = {{"&Desktop:", 0x3a6ea5}, {"&Frame:", 0x000080}, {"&Text:", 0x000000}};
                for (const auto &c : colors) {
                        FormRow *row = form->addRow(c.label);
                        if (row == nullptr) return Error::NoMem;
                        auto *swatch = Widget::create<ColorSwatch>(row->field(), c.rgb);
                        if (swatch == nullptr) return Error::NoMem;
                        swatch->setClicked(swatchClicked, this);
                        row->label()->setBuddy(swatch);
                }

                (void)form->addSection("Lists");
                FormRow *folders =
                        form->addRow("F&olders:", "Where applications are looked for, the first first: Add "
                                                  "chooses a folder; Up and Down (Alt+Up, Alt+Down) order them");
                FormRow *hosts = form->addRow("H&osts:", "Names known without DNS (name=address): Add and Edit (F2) "
                                                         "edit in place; an item edited to nothing goes");
                if (folders == nullptr || hosts == nullptr) return Error::NoMem;
                _folders = Widget::create<ListEditor>(folders->field(), ';');
                auto *hostList = Widget::create<ListEditor>(hosts->field(), ';');
                if (_folders == nullptr || hostList == nullptr) return Error::NoMem;
                folders->label()->setBuddy(_folders->view());
                hosts->label()->setBuddy(hostList->view());
                (void)_folders->setText("/sd0/p0/apps;/rom/apps");
                _folders->setTitle("Folder");
                _folders->setStretch(1);
                _folders->setAddAsked(folderAsked, this);
                _folders->setChanged(listChanged, this);
                (void)hostList->setText("sartre=192.168.1.10;printer=192.168.1.20");
                hostList->setTitle("Name=address");
                hostList->setStretch(1);
                hostList->setChanged(listChanged, this);

                (void)form->addSection("More");
                FormRow *longHelp = form->addRow(
                        StringView(), "Show a full-screen picture in a mode of the monitor's it fills exactly "
                                      "(Doom's 320x200 in 640x400 at 70 Hz), the monitor resyncing as it "
                                      "starts and ends; off: scaled on the desktop's mode. Experimental: this "
                                      "board's HDMI bridge glitches in those modes");
                if (longHelp == nullptr) return Error::NoMem;
                if (Widget::create<CheckBox>(longHelp->field(), "&Full-screen modes") == nullptr) return Error::NoMem;
                for (int i = 1; i <= 8; ++i) {
                        char text[24];
                        std::snprintf(text, sizeof(text), "Option %d", i);
                        FormRow *row = form->addRow(StringView());
                        if (row == nullptr || Widget::create<CheckBox>(row->field(), StringView(text)) == nullptr)
                                return Error::NoMem;
                }
                form->relayout();
                return Error::Ok;
        }

        void WidgetsWindow::numberChanged(void *arg, NumberField &field, int32_t value) {
                (void)field;
                char text[48];
                std::snprintf(text, sizeof(text), "Number: %ld", long(value));
                static_cast<WidgetsWindow *>(arg)->status(text);
                return;
        }

        void WidgetsWindow::volumeMoved(void *arg, Slider &slider, int32_t value) {
                (void)slider;
                char text[16];
                std::snprintf(text, sizeof(text), "%ld dB", long(value));
                static_cast<WidgetsWindow *>(arg)->_volumeLabel->setText(StringView(text));
                return;
        }

        void WidgetsWindow::swatchClicked(void *arg, ColorSwatch &swatch) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                self->_swatch = &swatch;
                if (self->_colors.open(swatch.color()).isError()) self->_swatch = nullptr;
                return;
        }

        // Live, as a setting would be.
        void WidgetsWindow::colorChanged(void *arg, ColorDialog &dialog, uint32_t rgb) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                (void)dialog;
                if (self->_swatch != nullptr) self->_swatch->setColor(rgb);
                char text[8], line[24];
                ColorModel::formatHex(rgb, text);
                std::snprintf(line, sizeof(line), "Color: %s", text);
                self->status(line);
                return;
        }

        void WidgetsWindow::colorFinished(void *arg, Dialog &dialog, int result) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                (void)dialog;
                if (self->_swatch != nullptr && result != ColorDialog::Accepted) {
                        self->_swatch->setColor(self->_colors.originalColor()); // put back
                        self->status("Color: put back");
                }
                if (self->_swatch != nullptr) self->_swatch->setFocus();
                self->_swatch = nullptr;
                return;
        }

        void WidgetsWindow::listChanged(void *arg, ListEditor &editor) {
                char             text[160];
                const StringView t = editor.text();
                std::snprintf(text, sizeof(text), "List: %.*s", int(t.size()), t.data());
                static_cast<WidgetsWindow *>(arg)->status(text);
                return;
        }

        void WidgetsWindow::folderAsked(void *arg, ListEditor &editor) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                (void)editor;
                self->_folderForList = true;
                if (self->_folder.open().isError()) self->_folderForList = false;
                return;
        }

        void WidgetsWindow::tabChanged(void *arg, TabWidget &tabs, size_t index) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                (void)tabs;
                self->status(index == 0 ? "Files" : index == 1 ? "Controls" : "Form");
                return;
        }

        void WidgetsWindow::fileChosen(void *arg, FileDialog &dialog, int result) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                char  text[160];
                const bool forList = self->_folderForList;
                self->_folderForList = false;
                if (result != FileDialog::Accepted) {
                        self->status("Cancelled");
                        return;
                }
                if (forList && self->_folders != nullptr) {
                        (void)self->_folders->addItem(dialog.path(0));
                        return;
                }
                const StringView first = dialog.path(0);
                std::snprintf(text, sizeof(text), "Chose %.*s%s (%zu)", int(first.size()), first.data(),
                              dialog.count() > 1 ? " and more" : "", dialog.count());
                self->status(text);
                return;
        }

        // File icons asked for while drawing (the window manager's task): loaded on the loop.
        void WidgetsWindow::iconsAsked(void *arg) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                if (self->_iconsPosted.exchange(true, std::memory_order_acq_rel)) return;
                if (self->application().post(iconsDue, self).isError())
                        self->_iconsPosted.store(false, std::memory_order_release);
                return;
        }

        void WidgetsWindow::iconsDue(void *arg) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                self->_iconsPosted.store(false, std::memory_order_release);
                if (!self->_fileIcons.loadAsked()) return;
                self->_icons->update();
                self->_details->update();
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
