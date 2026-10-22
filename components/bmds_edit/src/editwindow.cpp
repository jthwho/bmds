/**
 * @file      editwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * edit's window (devplan/editor.md §8).
 */

#include "editwindow.h"

#include <cstdio>
#include <bmds/command.h>
#include <bmds/icontheme.h>
#include <bmds/iconid.h>
#include <bmds/messagebox.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // devplan/editor.md §8: the keys are an interface once shipped.
        BMDS_SYSCONF(editorTabWidth, "editor/tab-width", Config::Type::Int, "8",
                     "Columns between tab stops in the text editor (1 to 16)", 1, 16, nullptr, nullptr)
        BMDS_SYSCONF(editorInsertSpaces, "editor/insert-spaces", Config::Type::Bool, "false",
                     "The text editor's Tab key inserts spaces up to the next stop, not a tab", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(editorAutoIndent, "editor/auto-indent", Config::Type::Bool, "true",
                     "A new line in the text editor starts as indented as the line before", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(editorScheme, "editor/scheme", Config::Type::String, "classic",
                     "The text editor's colours: classic, qbasic or dark", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(editorLineNumbers, "editor/line-numbers", Config::Type::Bool, "true",
                     "The text editor shows line numbers", 0, 0, nullptr, nullptr)

        constexpr size_t MaxFileBytes = 16u * 1024u * 1024u; // larger files are refused, not half read

        constexpr uint8_t letter(char ch) {
                return static_cast<uint8_t>(HidKey::A + (ch - 'a'));
        }

        constexpr uint8_t  F3 = HidKey::F1 + 2;
        constexpr uint8_t  F5 = HidKey::F1 + 4;
        constexpr uint8_t  F8 = HidKey::F1 + 7;
        constexpr uint32_t CheckAfterMs = 500; // a check when the typing pauses this long

        StringView fileName(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        StringView folderOf(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                if (slash == StringView::NPos) return StringView();
                return slash == 0 ? StringView("/") : path.left(slash);
        }

} // namespace

EditWindow::EditWindow(GuiApplication &app, const Config &config, const Startup &startup)
    : GuiWindow(app, config), _startup(startup), _fileMenu(app), _editMenu(app), _viewMenu(app), _helpMenu(app),
      _endingsMenu(app), _languageMenu(app), _schemeMenu(app), _runMenu(app), _newMenu(app), _completionMenu(app),
      _examplesDialog(app, FileDialog::Mode::Open, "Examples"), _openDialog(app, FileDialog::Mode::Open, "Open"),
      _saveDialog(app, FileDialog::Mode::Save, "Save As") {
        (void)_cwd.assign(startup.cwd);
        _startup.cwd = _cwd.view();
}

EditWindow::~EditWindow() {
        _settings.stop();
        if (_checkTimer >= 0) application().stopTimer(_checkTimer);
        if (_angel != nullptr) {
                _angel->~AngelSupport();
                Memory::free(_angel);
        }
        application().cancelPosts(this);
}

// ---- Building

Error EditWindow::build() {
        _column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (_column == nullptr) return Error::NoMem;
        setCentral(_column);
        auto *bar = Widget::create<MenuBar>(_column);
        if (bar == nullptr) return Error::NoMem;
        Error err = buildMenus();
        if (err.isOk()) err = bar->addMenu("&File", _fileMenu);
        if (err.isOk()) err = bar->addMenu("&Edit", _editMenu);
        if (err.isOk()) err = bar->addMenu("&View", _viewMenu);
        if (err.isOk()) err = bar->addMenu("&Run", _runMenu);
        if (err.isOk()) err = bar->addMenu("&Help", _helpMenu);
        if (err.isError()) return err;
        bar->setAboutToShow(aboutToShow, this);
        setMenuBar(bar);
        // The documents above, the pane (Output, Problems, Help) below, hidden until wanted.
        _split = Widget::create<Splitter>(_column, Splitter::Orientation::Vertical);
        if (_split == nullptr) return Error::NoMem;
        _tabs = Widget::create<TabWidget>(_split);
        err = buildPane(_split);
        _findBar = Widget::create<FindBar>(_column);
        _status = Widget::create<StatusBar>(_column);
        if (err.isError() || _tabs == nullptr || _findBar == nullptr || _status == nullptr) return Error::NoMem;
        _split->setWidgets(_tabs, _pane);
        _split->setLeast(96, 64);
        _split->setStretch(1);
        _tabs->setChanged(tabChanged, this);
        _findBar->setToggled(findToggled, this);
        (void)_status->addField(0);   // messages
        (void)_status->addField(112); // the language
        (void)_status->addField(160); // line and column
        (void)_status->addField(48);  // INS or OVR
        (void)_status->addField(56);  // LF or CRLF

        for (FileDialog *d : {&_openDialog, &_saveDialog}) {
                (void)d->addFilter("Text files",
                                   "*.txt;*.md;*.as;*.c;*.h;*.cpp;*.hpp;*.sh;*.conf;*.ini;*.json;*.desktop");
                (void)d->addFilter("All files", "");
                (void)d->setFolder(_cwd.isEmpty() ? StringView("/home") : _cwd.view());
        }
        _openDialog.setFinished(opened, this);
        _saveDialog.setFinished(savedAs, this);

        IconTheme &theme = application().icons();
        setIcon(theme.find(IconId::AccessoriesTextEditor, 16));
        setLargeIcon(theme.find(IconId::AccessoriesTextEditor, 32));
        readSettings();
        (void)_settings.start("editor", settingsChanged, this);
        _column->relayout();
        return Error::Ok;
}

Error EditWindow::buildMenus() {
        IconTheme &theme = application().icons();
        Error      err = _fileMenu.addItem("&New", ChooseNew, 0, "Ctrl+N", theme.find(IconId::DocumentNew, 16));
        if (err.isOk())
                err = _fileMenu.addItem("&Open...", ChooseOpen, 0, "Ctrl+O", theme.find(IconId::DocumentOpen, 16));
        if (err.isOk()) err = _fileMenu.addItem("&Save", ChooseSave, 0, "Ctrl+S", theme.find(IconId::DocumentSave, 16));
        if (err.isOk())
                err = _fileMenu.addItem("Save &As...", ChooseSaveAs, 0, "Ctrl+Shift+S",
                                        theme.find(IconId::DocumentSaveAs, 16));
        if (err.isOk()) err = _fileMenu.addItem("Save A&ll", ChooseSaveAll);
        if (err.isOk()) err = _fileMenu.addItem("&Revert", ChooseRevert);
        if (err.isOk()) err = _fileMenu.addSeparator();
        if (err.isOk())
                err = _fileMenu.addItem("&Close", ChooseClose, 0, "Ctrl+W", theme.find(IconId::WindowClose, 16));
        if (err.isOk()) err = _fileMenu.addItem("&Quit", ChooseQuit, 0, "Ctrl+Q", theme.find(IconId::SystemLogOut, 16));
        // The Edit menu is made each time it opens (what can be done then); the View menu too.
        if (err.isOk()) err = _fileMenu.addSubmenu("New from &Template", _newMenu);
        if (err.isOk()) err = _newMenu.addItem("&Text Document", ChooseTemplate + 0);
        if (err.isOk()) err = _newMenu.addItem("&AngelScript Program", ChooseTemplate + 1);
        if (err.isOk()) err = _newMenu.addItem("AngelScript &Game", ChooseTemplate + 2);
        if (err.isOk()) err = _runMenu.addItem("&Run", ChooseRun, 0, "F5", theme.find(IconId::MediaPlaybackStart, 16));
        if (err.isOk())
                err = _runMenu.addItem("&Stop", ChooseStop, 0, "Shift+F5", theme.find(IconId::MediaPlaybackStop, 16));
        if (err.isOk()) err = _runMenu.addItem("&Check", ChooseCheck, 0, "Ctrl+F5");
        if (err.isOk()) err = _runMenu.addSeparator();
        if (err.isOk()) err = _runMenu.addItem("&Next Problem", ChooseNextProblem, 0, "F8");
        if (err.isOk()) err = _runMenu.addItem("&Previous Problem", ChoosePreviousProblem, 0, "Shift+F8");
        if (err.isOk()) err = _runMenu.addItem("Co&mplete Word", ChooseComplete, 0, "Ctrl+Space");
        if (err.isOk()) err = _runMenu.addSeparator();
        if (err.isOk()) err = _runMenu.addItem("Show or Hide the &Pane", ChoosePane, 0, "F12");
        if (err.isOk()) err = _runMenu.addItem("C&lear the Output", ChooseClearOutput);
        if (err.isOk())
                err = _helpMenu.addItem("&Help on the Word", ChooseHelpWord, 0, "F1",
                                        theme.find(IconId::HelpContents, 16));
        if (err.isOk()) err = _helpMenu.addItem("&Examples...", ChooseExamples);
        if (err.isOk()) err = _helpMenu.addSeparator();
        if (err.isOk())
                err = _helpMenu.addItem("&About", ChooseAbout, 0, StringView(), theme.find(IconId::HelpAbout, 16));
        for (Menu *m : {&_fileMenu, &_editMenu, &_viewMenu, &_helpMenu, &_endingsMenu, &_languageMenu, &_schemeMenu,
                        &_runMenu, &_newMenu})
                m->setTriggered(chosen, this);
        _completionMenu.setTriggered(completionChosen, this);
        return err;
}

void EditWindow::aboutToShow(void *arg, MenuBar &, size_t index, Menu &menu) {
        auto      *self = static_cast<EditWindow *>(arg);
        Document  *doc = self->current();
        CodeEdit  *e = doc != nullptr ? doc->edit : nullptr;
        IconTheme &theme = self->application().icons();
        const auto on = [](bool enabled) {
                return enabled ? uint8_t(0) : uint8_t(MenuLayout::Disabled);
        };
        if (index == 1) {
                const bool sel = e != nullptr && e->selection().hasSelection();
                const bool writable = e != nullptr && !e->isReadOnly();
                menu.clear();
                (void)menu.addItem("&Undo", ChooseUndo, on(e != nullptr && e->document().canUndo()), "Ctrl+Z",
                                   theme.find(IconId::EditUndo, 16));
                (void)menu.addItem("&Redo", ChooseRedo, on(e != nullptr && e->document().canRedo()), "Ctrl+Y",
                                   theme.find(IconId::EditRedo, 16));
                (void)menu.addSeparator();
                (void)menu.addItem("Cu&t", ChooseCut, on(sel && writable), "Ctrl+X", theme.find(IconId::EditCut, 16));
                (void)menu.addItem("&Copy", ChooseCopy, on(sel), "Ctrl+C", theme.find(IconId::EditCopy, 16));
                (void)menu.addItem("&Paste", ChoosePaste, on(writable), "Ctrl+V", theme.find(IconId::EditPaste, 16));
                (void)menu.addItem("&Delete", ChooseDelete, on(sel && writable), "Del");
                (void)menu.addItem("Select &All", ChooseSelectAll, on(e != nullptr), "Ctrl+A");
                (void)menu.addSeparator();
                (void)menu.addItem("&Find...", ChooseFind, on(e != nullptr), "Ctrl+F",
                                   theme.find(IconId::EditFind, 16));
                (void)menu.addItem("Find &Next", ChooseFindNext, on(e != nullptr), "F3");
                (void)menu.addItem("Find Pre&vious", ChooseFindPrevious, on(e != nullptr), "Shift+F3");
                (void)menu.addItem("R&eplace...", ChooseReplace, on(writable), "Ctrl+H");
                (void)menu.addItem("&Go to Line...", ChooseGoToLine, on(e != nullptr), "Ctrl+G",
                                   theme.find(IconId::GoJump, 16));
                (void)menu.addSeparator();
                (void)menu.addItem("&Indent", ChooseIndent, on(writable), "Tab");
                (void)menu.addItem("U&nindent", ChooseUnindent, on(writable), "Shift+Tab");
                (void)menu.addItem("Toggle Co&mment", ChooseComment,
                                   on(writable && e->syntax() != nullptr && e->syntax()->lineComment != nullptr),
                                   "Ctrl+/");
        } else if (index == 2) {
                menu.clear();
                self->_endingsMenu.clear();
                const bool crlf = e != nullptr && e->document().lineEnding() == TextDocument::LineEnding::CrLf;
                (void)self->_endingsMenu.addItem("&Unix (LF)", ChooseEndingsLf,
                                                 uint8_t((crlf ? 0 : MenuLayout::Checked) | on(e != nullptr)));
                (void)self->_endingsMenu.addItem("&Windows (CR LF)", ChooseEndingsCrLf,
                                                 uint8_t((crlf ? MenuLayout::Checked : 0) | on(e != nullptr)));
                // The language: found from the file, or chosen.
                self->_languageMenu.clear();
                const Syntax *now = e != nullptr ? e->syntax() : nullptr;
                (void)self->_languageMenu.addItem(
                        "&Plain Text", ChooseLanguage,
                        uint8_t((now == nullptr ? MenuLayout::Checked : 0) | on(e != nullptr)));
                const Span<const Syntax *const> all = Syntax::all();
                for (size_t i = 0; i < all.size(); ++i)
                        (void)self->_languageMenu.addItem(
                                all[i]->name, ChooseLanguage + 1 + int(i),
                                uint8_t((now == all[i] ? MenuLayout::Checked : 0) | on(e != nullptr)));
                self->_schemeMenu.clear();
                const Span<const CodeEdit::Scheme> schemes = CodeEdit::schemes();
                for (size_t i = 0; i < schemes.size(); ++i)
                        (void)self->_schemeMenu.addItem(schemes[i].name, ChooseScheme + int(i),
                                                        self->_scheme == &schemes[i] ? MenuLayout::Checked : 0);
                (void)menu.addSubmenu("&Language", self->_languageMenu);
                (void)menu.addSubmenu("&Colours", self->_schemeMenu);
                (void)menu.addItem("Line &Numbers", ChooseLineNumbers, self->_lineNumbers ? MenuLayout::Checked : 0);
                (void)menu.addSubmenu("Line &Endings", self->_endingsMenu);
        }
        return;
}

void EditWindow::chosen(void *arg, int item) {
        static_cast<EditWindow *>(arg)->choose(item);
        return;
}

void EditWindow::choose(int item) {
        Document  *doc = current();
        CodeEdit  *e = doc != nullptr ? doc->edit : nullptr;
        const auto index = static_cast<size_t>(_tabs->current());
        switch (item) {
                case ChooseNew: newDocument(); break;
                case ChooseOpen: (void)_openDialog.open(); break;
                case ChooseSave:
                        if (doc != nullptr) (void)save(index);
                        break;
                case ChooseSaveAs:
                        if (doc != nullptr) saveAs(index);
                        break;
                case ChooseSaveAll:
                        for (size_t i = 0; i < _count; ++i)
                                if (_docs[i].edit->document().isModified() && !save(i)) break;
                        break;
                case ChooseRevert:
                        if (doc != nullptr) revert(index);
                        break;
                case ChooseClose:
                        if (doc != nullptr) closeTab(index, true);
                        break;
                case ChooseQuit: quit(); break;
                case ChooseRun: run(); break;
                case ChooseStop: stopRun(); break;
                case ChooseCheck: scheduleCheck(0); break;
                case ChooseClearOutput:
                        if (_output != nullptr) _output->clear();
                        break;
                case ChoosePane:
                        if (_pane != nullptr && _pane->isVisible()) {
                                _pane->setVisible(false);
                                _split->setPosition(_split->position());
                                if (e != nullptr) e->setFocus();
                        } else {
                                showPane(_pane != nullptr ? _pane->current() : 0);
                        }
                        break;
                case ChooseExamples: {
                        String folder(MemSpace::Psram);
                        if (SysConf::get(StringView("editor/angel-examples"), folder).isOk())
                                (void)_examplesDialog.setFolder(folder.view());
                        (void)_examplesDialog.open();
                        break;
                }
                case ChooseAbout:
                        (void)MessageBox::show(application(), MessageBox::Kind::Information, "About Edit",
                                               "Edit, the BMDS text editor.\n\nOne place for all text: plain files, "
                                               "AngelScript scripts (F5 runs one), C and C++ to come.",
                                               MessageBox::Buttons::Ok, nullptr, nullptr);
                        break;
                default: break;
        }
        if (e == nullptr) return;
        switch (item) {
                case ChooseUndo: e->undo(); break;
                case ChooseRedo: e->redo(); break;
                case ChooseCut: e->cut(); break;
                case ChooseCopy: e->copy(); break;
                case ChoosePaste: e->paste(); break;
                case ChooseDelete: e->deleteSelection(); break;
                case ChooseSelectAll: e->selectAll(); break;
                case ChooseFind: _findBar->open(FindBar::Mode::Find); break;
                case ChooseReplace: _findBar->open(FindBar::Mode::Replace); break;
                case ChooseGoToLine: _findBar->open(FindBar::Mode::GoToLine); break;
                case ChooseFindNext:
                case ChooseFindPrevious:
                        if (!_findBar->findNext(item == ChooseFindPrevious)) _findBar->open(FindBar::Mode::Find);
                        break;
                case ChooseIndent: e->indentLines(false); break;
                case ChooseUnindent: e->indentLines(true); break;
                case ChooseEndingsLf:
                case ChooseEndingsCrLf:
                        e->document().setLineEnding(item == ChooseEndingsCrLf ? TextDocument::LineEnding::CrLf
                                                                              : TextDocument::LineEnding::Lf,
                                                    true);
                        showTitle(index);
                        showStatus();
                        break;
                case ChooseComment: e->toggleComment(); break;
                case ChooseNextProblem:
                case ChoosePreviousProblem:
                        if (e->nextDiagnostic(item == ChoosePreviousProblem) == nullptr)
                                _status->setText(0, "No problems");
                        break;
                case ChooseHelpWord: helpAtCursor(); break;
                case ChooseComplete: complete(); break;
                case ChooseLineNumbers: setSetting("editor/line-numbers", _lineNumbers ? "false" : "true"); break;
                default:
                        if (item >= ChooseLanguage && item < ChooseScheme) {
                                const Span<const Syntax *const> all = Syntax::all();
                                const size_t                    n = size_t(item - ChooseLanguage);
                                e->setSyntax(n == 0 || n > all.size() ? nullptr : all[n - 1]);
                                showStatus();
                        } else if (item >= ChooseTemplate && item < ChooseCompletion) {
                                newFromTemplate(size_t(item - ChooseTemplate));
                        } else if (item >= ChooseScheme && size_t(item - ChooseScheme) < CodeEdit::schemes().size()) {
                                setSetting("editor/scheme", CodeEdit::schemes()[size_t(item - ChooseScheme)].id);
                        }
                        break;
        }
        return;
}

// Run's keys work wherever the keys are, the output's terminal included (which would send them to
// the program).
bool EditWindow::keyFirst(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release || event.hasAlt()) return false;
        const bool ctrl = event.hasCtrl(), shift = event.hasShift();
        int        item = 0;
        if (event.usage == F5 && !(ctrl && shift)) item = ctrl ? ChooseCheck : shift ? ChooseStop : ChooseRun;
        if (event.usage == HidKey::F12 && !ctrl && !shift) item = ChoosePane;
        if (item == 0) return false;
        choose(item);
        return true;
}

bool EditWindow::keyUnused(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        if (event.usage == HidKey::Escape && _findBar->isVisible()) {
                _findBar->close();
                return true;
        }
        // The window's keys: each with its modifiers exactly (Ctrl+Tab is the tab widget's own).
        const uint8_t mods = uint8_t((event.hasCtrl() ? 1 : 0) | (event.hasShift() ? 2 : 0) | (event.hasAlt() ? 4 : 0));
        constexpr uint8_t None = 0, Ctrl = 1, Shift = 2, CtrlShift = 3;
        struct Binding {
                        uint8_t usage;
                        uint8_t mods;
                        int     item;
        };
        static constexpr Binding Bindings[] = {
                {letter('n'), Ctrl, ChooseNew},      {letter('o'), Ctrl, ChooseOpen},
                {letter('s'), Ctrl, ChooseSave},     {letter('s'), CtrlShift, ChooseSaveAs},
                {letter('w'), Ctrl, ChooseClose},    {letter('q'), Ctrl, ChooseQuit},
                {letter('f'), Ctrl, ChooseFind},     {letter('h'), Ctrl, ChooseReplace},
                {letter('g'), Ctrl, ChooseGoToLine}, {HidKey::Space, Ctrl, ChooseComplete},
                {F3, None, ChooseFindNext},          {F3, Shift, ChooseFindPrevious},
                {F8, None, ChooseNextProblem},       {F8, Shift, ChoosePreviousProblem},
                {HidKey::F1, None, ChooseHelpWord},
        };
        for (const Binding &b : Bindings)
                if (b.usage == event.usage && b.mods == mods) {
                        choose(b.item);
                        return true;
                }
        return false;
}

// ---- Documents

EditWindow::Document *EditWindow::current() {
        if (_count == 0) return nullptr;
        return &_docs[_tabs->current() < _count ? _tabs->current() : 0];
}

int EditWindow::indexOf(const TextEdit *edit) const {
        for (size_t i = 0; i < _count; ++i)
                if (_docs[i].edit == edit) return int(i);
        return -1;
}

int EditWindow::indexOfPath(StringView path) const {
        for (size_t i = 0; i < _count; ++i)
                if (_docs[i].path.view() == path) return int(i);
        return -1;
}

EditWindow::Document *EditWindow::addDocument() {
        if (_count == TabWidget::MaxTabs) {
                warn("Too many documents open (close one first)", StringView(), Error::Full);
                return nullptr;
        }
        auto *edit = Widget::create<CodeEdit>(_tabs);
        if (edit == nullptr) return nullptr;
        if (_tabs->addTab(edit, "Untitled").isError()) {
                edit->destroy();
                return nullptr;
        }
        Document &doc = _docs[_count++];
        doc = Document{};
        doc.edit = edit;
        doc.id = ++_nextId;
        apply(*edit);
        edit->setChanged(textChanged, this);
        edit->setCursorMoved(cursorMoved, this);
        return &doc;
}

void EditWindow::newDocument() {
        Document *doc = addDocument();
        if (doc == nullptr) return;
        doc->untitled = ++_untitled;
        const size_t index = _count - 1;
        _tabs->setCurrent(index);
        showTitle(index);
        doc->edit->setFocus();
        _findBar->setTarget(doc->edit);
        showStatus();
        return;
}

void EditWindow::open(StringView path) {
        const int already = indexOfPath(path);
        if (already >= 0) {
                _tabs->setCurrent(size_t(already));
                _docs[already].edit->setFocus();
                _findBar->setTarget(_docs[already].edit);
                showStatus();
                return;
        }
        // A single untitled, untouched document is replaced rather than kept beside.
        int reuse = -1;
        if (_count == 1 && _docs[0].path.isEmpty() && !_docs[0].edit->document().isModified() &&
            _docs[0].edit->document().length() == 0)
                reuse = 0;
        Document *doc = reuse >= 0 ? &_docs[reuse] : addDocument();
        if (doc == nullptr) return;
        const Error err = load(*doc, path);
        if (err.isError() && err != Error::NotExist) {
                warn("Can't open", path, err);
                if (reuse < 0) closeTab(_count - 1, false);
                return;
        }
        (void)doc->path.assign(path);
        doc->untitled = 0;
        chooseLanguage(*doc);
        const size_t index = size_t(doc - _docs);
        _tabs->setCurrent(index);
        showTitle(index);
        doc->edit->setFocus();
        _findBar->setTarget(doc->edit);
        if (err == Error::NotExist) _status->setText(0, "A new file");
        showStatus();
        return;
}

Error EditWindow::load(Document &doc, StringView path) {
        auto [st, serr] = Vfs::stat(path, Vfs::Context{});
        if (serr.isError()) return serr;
        if (st.type == FileType::Directory) return Error::IsDir;
        if (st.size > MaxFileBytes) return Error::OutOfRange;
        auto [file, oerr] = Vfs::open(path, Vfs::Read, Vfs::Context{});
        if (oerr.isError()) return oerr;
        String bytes(MemSpace::Psram);
        Error  err = bytes.reserve(size_t(st.size));
        char   chunk[4096];
        while (err.isOk()) {
                auto [n, rerr] =
                        file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(chunk), sizeof(chunk)), WaitForever);
                if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                if (rerr.isError()) {
                        err = rerr;
                        break;
                }
                if (bytes.size() + n > MaxFileBytes) {
                        err = Error::OutOfRange;
                        break;
                }
                err = bytes.append(StringView(chunk, n));
        }
        if (err.isOk()) err = doc.edit->setText(bytes.view());
        // A file it can't write (/rom's, a read-only card) is shown read-only: Save As saves a copy.
        if (err.isOk()) {
                file.reset();
                auto [probe, perr] = Vfs::open(path, Vfs::Write, Vfs::Context{});
                doc.readOnly = perr == Error::ReadOnly || perr == Error::PermissionDenied;
                doc.edit->setReadOnly(doc.readOnly);
        }
        return err;
}

// Writes to a new file beside it, then renames that over the old one: a cut power leaves the old
// file or the new, never half of either.
Error EditWindow::write(Document &doc) {
        String bytes(MemSpace::Psram);
        Error  err = doc.edit->document().save(bytes);
        if (err.isError()) return err;
        String temporary(MemSpace::Psram);
        err = temporary.assign(folderOf(doc.path.view()));
        if (err.isOk() && !temporary.view().endsWith('/')) err = temporary.append('/');
        if (err.isOk()) err = temporary.append(".");
        if (err.isOk()) err = temporary.append(fileName(doc.path.view()));
        if (err.isOk()) err = temporary.append(".edit-save");
        if (err.isError()) return err;
        {
                auto [file, oerr] =
                        Vfs::open(temporary.view(), uint8_t(Vfs::Write | Vfs::Create | Vfs::Truncate), Vfs::Context{});
                if (oerr.isError()) return oerr;
                size_t done = 0;
                while (done < bytes.size()) {
                        auto [n, werr] = file->write(
                                ByteSpan(reinterpret_cast<const uint8_t *>(bytes.data()) + done, bytes.size() - done),
                                WaitForever);
                        if (werr.isError() || n == 0) {
                                file.reset();
                                (void)Vfs::remove(temporary.view(), Vfs::Context{});
                                return werr.isError() ? werr : Error(Error::IoError);
                        }
                        done += n;
                }
        }
        err = Vfs::rename(temporary.view(), doc.path.view(), Vfs::RenameMode::Replace, Vfs::Context{});
        if (err.isError()) (void)Vfs::remove(temporary.view(), Vfs::Context{});
        return err;
}

bool EditWindow::save(size_t index) {
        if (index >= _count) return false;
        Document &doc = _docs[index];
        if (doc.path.isEmpty() || doc.readOnly) {
                saveAs(index);
                return false;
        }
        const Error err = write(doc);
        if (err.isError()) {
                warn("Can't save", doc.path.view(), err);
                return false;
        }
        doc.edit->document().setSaved();
        showTitle(index);
        _status->setText(0, "Saved");
        return true;
}

void EditWindow::saveAs(size_t index) {
        if (index >= _count) return;
        Document &doc = _docs[index];
        _savingAs = int(index);
        if (!doc.path.isEmpty()) {
                (void)_saveDialog.setFolder(folderOf(doc.path.view()));
                (void)_saveDialog.setFileName(fileName(doc.path.view()));
        } else {
                (void)_saveDialog.setFileName("Untitled.txt");
        }
        (void)_saveDialog.open();
        return;
}

void EditWindow::savedAs(void *arg, FileDialog &dialog, int result) {
        auto     *self = static_cast<EditWindow *>(arg);
        const int index = self->_savingAs;
        self->_savingAs = -1;
        if (result != FileDialog::Accepted || index < 0 || size_t(index) >= self->_count || dialog.count() == 0) {
                self->_quitting = false;
                self->_runAfterSave = false;
                return;
        }
        Document &doc = self->_docs[index];
        String    was(MemSpace::Psram);
        (void)was.assign(doc.path.view());
        (void)doc.path.assign(dialog.path(0));
        const bool wasReadOnly = doc.readOnly;
        doc.readOnly = false;
        if (!self->save(size_t(index))) {
                doc.readOnly = wasReadOnly;
                (void)doc.path.assign(was.view());
                self->_quitting = false;
                return;
        }
        doc.untitled = 0;
        doc.edit->setReadOnly(false);
        self->chooseLanguage(doc);
        self->showTitle(size_t(index));
        if (self->_runAfterSave) {
                self->_runAfterSave = false;
                self->run();
                return;
        }
        // Saved as part of closing: go on with it.
        if (self->_asking == index) {
                self->_asking = -1;
                if (self->_then == Then::CloseTab) {
                        self->closeTab(size_t(index), false);
                } else {
                        self->closeTab(size_t(index), false);
                        if (!self->askNext(Then::Quit)) self->application().quit(0);
                }
        }
        return;
}

void EditWindow::opened(void *arg, FileDialog &dialog, int result) {
        auto *self = static_cast<EditWindow *>(arg);
        if (result != FileDialog::Accepted) return;
        for (size_t i = 0; i < dialog.count(); ++i) self->open(dialog.path(i));
        return;
}

void EditWindow::revert(size_t index) {
        Document &doc = _docs[index];
        if (doc.path.isEmpty()) return;
        const Error err = load(doc, doc.path.view());
        if (err.isError()) warn("Can't read", doc.path.view(), err);
        showTitle(index);
        showStatus();
        return;
}

// ---- Closing: modified documents are asked about one at a time (Save, Discard, Cancel).

void EditWindow::closeTab(size_t index, bool ask) {
        if (index >= _count) return;
        Document &doc = _docs[index];
        if (ask && doc.edit->document().isModified()) {
                _asking = int(index);
                _then = Then::CloseTab;
                _tabs->setCurrent(index);
                String text(MemSpace::Psram);
                (void)text.appendf("Save the changes to %.*s before closing it?", int(nameOf(doc).size()),
                                   nameOf(doc).data());
                (void)MessageBox::show(application(), MessageBox::Kind::Question, "Edit", text.view(),
                                       MessageBox::Buttons::YesNoCancel, answered, this);
                return;
        }
        CodeEdit *gone = doc.edit;
        if (_findBar->target() == gone) _findBar->setTarget(nullptr);
        _tabs->removeTab(index);
        gone->destroy();
        for (size_t i = index; i + 1 < _count; ++i) {
                _docs[i].edit = _docs[i + 1].edit;
                (void)_docs[i].path.assign(_docs[i + 1].path.view());
                _docs[i].untitled = _docs[i + 1].untitled;
                _docs[i].wasModified = _docs[i + 1].wasModified;
        }
        _docs[--_count] = Document{};
        if (_count == 0 && !_quitting) newDocument(); // never empty
        if (Document *d = current(); d != nullptr) {
                _findBar->setTarget(d->edit);
                d->edit->setFocus();
        }
        showStatus();
        return;
}

void EditWindow::quit() {
        _quitting = true;
        if (!askNext(Then::Quit)) application().quit(0);
        return;
}

bool EditWindow::askNext(Then then) {
        for (size_t i = 0; i < _count; ++i) {
                if (!_docs[i].edit->document().isModified()) continue;
                _asking = int(i);
                _then = then;
                _tabs->setCurrent(i);
                String text(MemSpace::Psram);
                (void)text.appendf("Save the changes to %.*s before quitting?", int(nameOf(_docs[i]).size()),
                                   nameOf(_docs[i]).data());
                (void)MessageBox::show(application(), MessageBox::Kind::Question, "Edit", text.view(),
                                       MessageBox::Buttons::YesNoCancel, answered, this);
                return true;
        }
        return false;
}

void EditWindow::answered(void *arg, MessageBox::Answer answer) {
        auto     *self = static_cast<EditWindow *>(arg);
        const int index = self->_asking;
        if (index < 0 || size_t(index) >= self->_count) return;
        if (answer == MessageBox::Answer::Cancel) {
                self->_asking = -1;
                self->_quitting = false;
                return;
        }
        if (answer == MessageBox::Answer::Yes) {
                // An untitled one asks for a name first; savedAs() goes on from there.
                if (!self->save(size_t(index))) {
                        if (self->_savingAs != index) {
                                self->_asking = -1;
                                self->_quitting = false;
                        }
                        return;
                }
        }
        self->_asking = -1;
        if (self->_then == Then::CloseTab) {
                self->closeTab(size_t(index), false);
                return;
        }
        // Quitting: this one is settled (saved, or its changes let go); then the next.
        self->_docs[index].edit->document().setSaved();
        if (!self->askNext(Then::Quit)) self->application().quit(0);
        return;
}

// The window manager closed the window (its close button): with nothing unsaved, that's the
// end; otherwise it opens again to ask.
void EditWindow::closedByManager() {
        GuiWindow::closedByManager();
        bool modified = false;
        for (size_t i = 0; i < _count; ++i) modified = modified || _docs[i].edit->document().isModified();
        if (!modified) {
                application().quit(0);
                return;
        }
        const Rect was = lastRect();
        if ((was.width != 0 ? show(was) : show()).isError()) {
                application().quit(1);
                return;
        }
        quit();
        return;
}

// ---- What shows

String EditWindow::nameOf(const Document &doc) const {
        String name(MemSpace::Psram);
        if (!doc.path.isEmpty())
                (void)name.assign(fileName(doc.path.view()));
        else if (doc.untitled <= 1)
                (void)name.assign("Untitled");
        else
                (void)name.appendf("Untitled %u", unsigned(doc.untitled));
        return name;
}

void EditWindow::showTitle(size_t index) {
        if (index >= _count) return;
        Document  &doc = _docs[index];
        const bool modified = doc.edit->document().isModified();
        String     tab = nameOf(doc);
        if (modified) (void)tab.append(" *");
        _tabs->setTitle(index, tab.view());
        doc.wasModified = modified;
        if (index == _tabs->current()) {
                String title(MemSpace::Psram);
                (void)title.append(tab.view());
                if (doc.readOnly) (void)title.append(" (read-only)");
                (void)title.append(" - Edit");
                setTitle(title.view());
        }
        return;
}

void EditWindow::showStatus() {
        Document *doc = current();
        if (doc == nullptr) return;
        CodeEdit          &e = *doc->edit;
        const TextPosition p = e.selection().cursor;
        char               text[48];
        const uint32_t     column = e.document().columnOf(p.line, p.byte, e.tabWidth()) + 1;
        std::snprintf(text, sizeof(text), "Line %u, Col %u", unsigned(p.line + 1), unsigned(column));
        _status->setText(1, e.syntax() != nullptr ? StringView(e.syntax()->name) : StringView("Plain Text"));
        _status->setText(2, text);
        _status->setText(3, e.isOverwrite() ? "OVR" : "INS");
        _status->setText(4, e.document().lineEnding() == TextDocument::LineEnding::CrLf ? "CRLF" : "LF");
        return;
}

void EditWindow::warn(StringView text, StringView path, Error err) {
        String message(MemSpace::Psram);
        (void)message.append(text);
        if (!path.isEmpty()) {
                (void)message.append(" ");
                (void)message.append(path);
        }
        if (err != Error::Full) {
                (void)message.append(": ");
                (void)message.append(CommandContext::errorText(err));
        }
        (void)MessageBox::show(application(), MessageBox::Kind::Error, "Edit", message.view(), MessageBox::Buttons::Ok,
                               nullptr, nullptr);
        return;
}

void EditWindow::tabChanged(void *arg, TabWidget &, size_t index) {
        auto *self = static_cast<EditWindow *>(arg);
        if (index >= self->_count) return;
        self->_findBar->setTarget(self->_docs[index].edit);
        self->showTitle(index);
        self->showStatus();
        self->showProblems();
        return;
}

void EditWindow::textChanged(void *arg, TextEdit &edit) {
        auto     *self = static_cast<EditWindow *>(arg);
        const int index = self->indexOf(&edit);
        if (index < 0) return;
        if (self->_docs[index].wasModified != edit.document().isModified()) self->showTitle(size_t(index));
        self->_status->setText(0, StringView());
        // Checked when the typing pauses.
        if (self->supportFor(self->_docs[index]) != nullptr) self->scheduleCheck(CheckAfterMs);
        return;
}

void EditWindow::cursorMoved(void *arg, TextEdit &edit) {
        auto *self = static_cast<EditWindow *>(arg);
        if (self->current() != nullptr && self->current()->edit == &edit) self->showStatus();
        // An undo back to the saved text changes the title too.
        const int index = self->indexOf(&edit);
        if (index >= 0 && self->_docs[index].wasModified != edit.document().isModified())
                self->showTitle(size_t(index));
        return;
}

void EditWindow::findToggled(void *arg, FindBar &) {
        static_cast<EditWindow *>(arg)->_column->relayout();
        return;
}

// ---- Settings (followed while it runs)

void EditWindow::readSettings() {
        auto [width, werr] = SysConf::getInt(StringView("editor/tab-width"));
        auto [spaces, serr] = SysConf::getBool(StringView("editor/insert-spaces"));
        auto [indent, ierr] = SysConf::getBool(StringView("editor/auto-indent"));
        if (werr.isOk() && width >= 1 && width <= 16) _tabWidth = uint32_t(width);
        if (serr.isOk()) _insertSpaces = spaces;
        if (ierr.isOk()) _autoIndent = indent;
        String scheme(MemSpace::Psram);
        _scheme = SysConf::get(StringView("editor/scheme"), scheme).isOk() ? CodeEdit::scheme(scheme.view()) : nullptr;
        if (_scheme == nullptr) _scheme = &CodeEdit::schemes()[0];
        auto [numbers, nerr] = SysConf::getBool(StringView("editor/line-numbers"));
        if (nerr.isOk()) _lineNumbers = numbers;
        for (size_t i = 0; i < _count; ++i) apply(*_docs[i].edit);
        return;
}

void EditWindow::apply(CodeEdit &edit) {
        edit.setTabWidth(_tabWidth);
        edit.setInsertSpaces(_insertSpaces);
        edit.setAutoIndent(_autoIndent);
        if (_scheme != nullptr) edit.setScheme(*_scheme);
        edit.setLineNumbers(_lineNumbers);
        return;
}

// The language from the file's name, or its first line's `#!`.
void EditWindow::chooseLanguage(Document &doc) {
        const TextDocument &d = doc.edit->document();
        doc.edit->setSyntax(Syntax::forFile(doc.path.view(), d.line(0)));
        if (supportFor(doc) != nullptr) {
                namesChanged(); // its type names
                scheduleCheck(0);
        }
        return;
}

// A choice in the View menu is the setting's (kept, as the settings application keeps it); every
// window follows it through the watcher.
void EditWindow::setSetting(StringView key, StringView value) {
        if (SysConf::set(key, value).isOk()) (void)SysConf::save(key);
        return;
}

// On the task that changed the setting: posted to the loop's.
void EditWindow::settingsChanged(void *arg) {
        auto *self = static_cast<EditWindow *>(arg);
        (void)self->application().post(settingsDue, self);
        return;
}

void EditWindow::settingsDue(void *arg) {
        auto       *self = static_cast<EditWindow *>(arg);
        SysConf::Id id;
        while (self->_settings.next(id)) {}
        self->readSettings();
        self->showStatus();
        return;
}

BMDS_NAMESPACE_END
