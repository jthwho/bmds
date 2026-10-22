/**
 * @file      editrun.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * edit's language tools (devplan/editor.md §9): the pane (Output, Problems, Help), checks as you
 * type, Run and Stop, F1 help, completion, templates and the examples.
 */

#include <cstdio>
#include <new>
#include <bmds/memory.h>
#include <bmds/pipe.h>
#include <bmds/sysconf.h>
#include "editwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(editorExamples, "editor/angel-examples", Config::Type::String, "/rom/share/angel/examples",
                     "Where the text editor's Help > Examples looks for AngelScript examples", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(editorScripts, "editor/scripts", Config::Type::String, "${HOME}/scripts",
                     "Where the text editor saves new scripts by default", 0, 0, nullptr, nullptr)

        enum PaneTab : size_t {
                OutputTab = 0,
                ProblemsTab = 1,
                HelpTab = 2,
        };

        constexpr int      ReportFd = 3; // where a run's report is written
        constexpr uint32_t BusyRetryMs = 300;
        constexpr size_t   MaxCompletions = 24;

        // The templates (File > New from Template): their names in the menu, their text.
        struct Template {
                        const char *syntax; // its language (null: plain text)
                        const char *text;
        };

        constexpr Template Templates[] = {
                {nullptr, ""},
                {"angelscript", "#!/rom/bin/angel\n"
                                "// A program: say what it does here. F5 runs it.\n"
                                "\n"
                                "string name = input(\"What's your name? \");\n"
                                "println(\"Hello, \" + name + \"!\");\n"},
                {"angelscript", "#!/rom/bin/angel\n"
                                "// A game: the arrow keys steer the ball. F5 runs it; close its window to stop.\n"
                                "\n"
                                "Window w(\"My Game\", 640, 480);\n"
                                "\n"
                                "class MyGame : Game {\n"
                                "        float x = 320, y = 240;\n"
                                "\n"
                                "        // Sixty times a second: move.\n"
                                "        void tick() {\n"
                                "                if (w.keyDown(\"Left\")) x -= 4;\n"
                                "                if (w.keyDown(\"Right\")) x += 4;\n"
                                "                if (w.keyDown(\"Up\")) y -= 4;\n"
                                "                if (w.keyDown(\"Down\")) y += 4;\n"
                                "        }\n"
                                "\n"
                                "        // Each frame: draw.\n"
                                "        void draw() {\n"
                                "                w.clear(Color::Navy);\n"
                                "                w.fillCircle(int(x), int(y), 20, Color::Yellow);\n"
                                "                w.text(10, 10, \"The arrow keys move the ball\", Color::White);\n"
                                "        }\n"
                                "}\n"
                                "\n"
                                "w.run(MyGame());\n"},
        };

        StringView folderOf(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                if (slash == StringView::NPos) return StringView();
                return slash == 0 ? StringView("/") : path.left(slash);
        }

        StringView fileName(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

} // namespace

// ---- The pane

Error EditWindow::buildPane(Widget *parent) {
        _pane = Widget::create<TabWidget>(parent);
        if (_pane == nullptr) return Error::NoMem;
        _output = Widget::create<TerminalWidget>(_pane);
        _problems = Widget::create<TextEdit>(_pane);
        _helpView = Widget::create<TextEdit>(_pane);
        if (_output == nullptr || _problems == nullptr || _helpView == nullptr) return Error::NoMem;
        (void)_pane->addTab(_output, "Output");
        (void)_pane->addTab(_problems, "Problems");
        (void)_pane->addTab(_helpView, "Help");
        _problems->setReadOnly(true);
        _helpView->setReadOnly(true);
        _problems->setCursorMoved(problemPicked, this);
        _output->setEnded(runEnded, this);
        _pane->setVisible(false);
        (void)_examplesDialog.addFilter("AngelScript", "*.as");
        (void)_examplesDialog.addFilter("All files", "");
        _examplesDialog.setFinished(examplesChosen, this);
        return Error::Ok;
}

void EditWindow::showPane(size_t tab) {
        if (_pane == nullptr) return;
        if (!_pane->isVisible()) {
                _pane->setVisible(true);
                // A third of the height for the pane at first.
                const uint16_t h = _split->geometry().height;
                _split->setPosition(uint16_t(h > 300 ? h - h / 3 : h * 2 / 3));
        }
        _pane->setCurrent(tab);
        return;
}

// ---- Languages' tools

LanguageSupport *EditWindow::supportFor(const Document &doc) {
        const Syntax *syntax = doc.edit != nullptr ? doc.edit->syntax() : nullptr;
        if (syntax == nullptr || StringView(syntax->id) != "angelscript") return nullptr;
        if (_angel == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(AngelSupport));
                if (mem == nullptr) return nullptr;
                AngelSupport::Context context;
                context.launcher = _startup.launcher;
                context.environment = _startup.environment;
                context.cwd = _startup.cwd;
                _angel = ::new (mem) AngelSupport(application(), context, *this);
        }
        return _angel;
}

EditWindow::Document *EditWindow::documentById(uint32_t id) {
        for (size_t i = 0; i < _count; ++i)
                if (_docs[i].id == id) return &_docs[i];
        return nullptr;
}

void EditWindow::scheduleCheck(uint32_t delayMs) {
        if (_checkTimer >= 0) application().stopTimer(_checkTimer);
        _checkTimer = -1;
        auto r = application().startTimer(delayMs != 0 ? delayMs : 1, false, checkDue, this);
        if (r.isOk()) _checkTimer = r.value();
        return;
}

void EditWindow::checkDue(void *arg) {
        auto *self = static_cast<EditWindow *>(arg);
        self->_checkTimer = -1;
        self->checkCurrent();
        return;
}

// The current document checked as it is now (unsaved: its text goes to the checker).
void EditWindow::checkCurrent() {
        Document        *doc = current();
        LanguageSupport *support = doc != nullptr ? supportFor(*doc) : nullptr;
        if (support == nullptr) return;
        String text(MemSpace::Psram);
        if (doc->edit->document().save(text).isError()) return;
        // An untitled script is checked as if in the scripts' folder (for its #includes).
        String path(MemSpace::Psram);
        if (!doc->path.isEmpty())
                (void)path.assign(doc->path.view());
        else
                (void)path.appendf("%.*s/untitled.as", int(_cwd.size()), _cwd.data());
        const Error err = support->check(doc->id, path.view(), text.view());
        if (err == Error::Busy) scheduleCheck(BusyRetryMs); // after the one under way
        return;
}

void EditWindow::checked(uint32_t token, Span<const CodeEdit::Diagnostic> diagnostics) {
        Document *doc = documentById(token);
        if (doc == nullptr) return;
        (void)doc->edit->setDiagnostics(diagnostics);
        if (doc == current()) {
                showProblems();
                size_t errors = 0, warnings = 0;
                for (const CodeEdit::Diagnostic &d : diagnostics)
                        (d.severity == CodeEdit::Severity::Error ? errors : warnings) += 1;
                char text[48];
                if (errors + warnings == 0)
                        std::snprintf(text, sizeof(text), "No problems");
                else
                        std::snprintf(text, sizeof(text), "%u error%s, %u warning%s (F8)", unsigned(errors),
                                      errors == 1 ? "" : "s", unsigned(warnings), warnings == 1 ? "" : "s");
                _status->setText(0, text);
        }
        return;
}

// The API's types and the script's classes, coloured as types in every script.
void EditWindow::namesChanged() {
        for (size_t i = 0; i < _count; ++i) {
                LanguageSupport *support = supportFor(_docs[i]);
                if (support == nullptr) continue;
                List<String> names(MemSpace::Psram);
                support->typeNames(names);
                _docs[i].edit->setTypeNames(std::move(names));
        }
        return;
}

// ---- Problems

void EditWindow::showProblems() {
        if (_problems == nullptr) return;
        Document *doc = current();
        String    text(MemSpace::Psram);
        if (doc != nullptr) {
                for (const CodeEdit::Diagnostic &d : doc->edit->diagnostics()) {
                        const char *kind = d.severity == CodeEdit::Severity::Error     ? "error"
                                           : d.severity == CodeEdit::Severity::Warning ? "warning"
                                                                                       : "note";
                        (void)text.appendf("%u:%u: %s: %.*s\n", unsigned(d.line + 1), unsigned(d.start + 1), kind,
                                           int(d.message.size()), d.message.data());
                }
        }
        if (text.isEmpty()) (void)text.assign("No problems.\n");
        _problemsShowing = true;
        (void)_problems->setText(text.view());
        _problemsShowing = false;
        return;
}

// A click on a problem's line: the document's cursor goes there.
void EditWindow::problemPicked(void *arg, TextEdit &edit) {
        auto *self = static_cast<EditWindow *>(arg);
        if (self->_problemsShowing || !edit.hasFocus()) return;
        Document *doc = self->current();
        if (doc == nullptr) return;
        const uint32_t                         line = edit.selection().cursor.line;
        const Span<const CodeEdit::Diagnostic> all = doc->edit->diagnostics();
        if (line >= all.size()) return;
        const CodeEdit::Diagnostic &d = all[line];
        doc->edit->setSelection(TextSelection{TextPosition{d.line, d.start}, TextPosition{d.line, d.start}});
        doc->edit->setFocus();
        return;
}

// ---- Running

void EditWindow::run() {
        Document *doc = current();
        if (doc == nullptr) return;
        LanguageSupport *support = supportFor(*doc);
        if (support == nullptr) {
                _status->setText(0, "This kind of file doesn't run (only AngelScript, for now)");
                return;
        }
        if (_output->isRunning()) {
                _status->setText(0, "It's running already (Shift+F5 stops it)");
                return;
        }
        // Saved first: what runs is the file. An untitled one is named first, then runs.
        const size_t index = size_t(doc - _docs);
        if (doc->path.isEmpty() || doc->readOnly) {
                _runAfterSave = true;
                if (doc->path.isEmpty()) {
                        // The scripts' folder (a setting, `${HOME}` and the like expanded), made if missing.
                        String raw(MemSpace::Psram), folder(MemSpace::Psram);
                        if (SysConf::get(StringView("editor/scripts"), raw).isOk() &&
                            SysConf::expand(raw.view(), folder).isOk()) {
                                (void)Vfs::mkdir(folder.view(), Vfs::Context{}); // there already, or made
                                (void)_saveDialog.setFolder(folder.view());
                        }
                        (void)_saveDialog.setFileName("program.as");
                        _savingAs = int(index);
                        (void)_saveDialog.open();
                } else {
                        saveAs(index);
                }
                return;
        }
        if (doc->edit->document().isModified() && !save(index)) return;
        auto [pipe, perr] = Pipe::create();
        List<String> argv(MemSpace::Psram);
        Error        err = perr.isError() ? perr : support->runCommand(doc->path.view(), ReportFd, argv);
        if (err.isError()) {
                warn("Can't run", doc->path.view(), err);
                return;
        }
        StringView args[8];
        size_t     n = 0;
        for (const String &a : argv)
                if (n < 8) args[n++] = a.view();
        TerminalWidget::ExtraFd extra[1];
        extra[0].fd = ReportFd;
        extra[0].handle = std::move(pipe.write);
        TerminalWidget::Command command;
        command.path = args[0];
        command.args = Span<const StringView>(args, n);
        command.environment = _startup.environment;
        command.cwd = folderOf(doc->path.view());
        command.launcher = _startup.launcher;
        command.process = _startup.process;
        command.extraFds = Span<TerminalWidget::ExtraFd>(extra, 1);
        // The doc's diagnostics from a run before go; a check puts its own back.
        (void)doc->edit->setDiagnostics(Span<const CodeEdit::Diagnostic>());
        String banner(MemSpace::Psram);
        (void)banner.appendf("\x1b[1;36m-- %.*s --\x1b[0m\r\n", int(fileName(doc->path.view()).size()),
                             fileName(doc->path.view()).data());
        showPane(OutputTab);
        _output->show(banner.view());
        err = _output->start(command);
        if (err.isError()) {
                warn("Can't run", doc->path.view(), err);
                return;
        }
        _report = std::move(pipe.read);
        _runDoc = doc->id;
        _stopAsked = false;
        _output->setFocus(); // what it reads is typed here
        _status->setText(0, "Running (Shift+F5 stops it)");
        return;
}

void EditWindow::stopRun() {
        if (_output == nullptr || !_output->isRunning()) return;
        // Asked once: SIGTERM; again: SIGKILL.
        (void)_output->stop(_stopAsked);
        _stopAsked = true;
        _status->setText(0, "Stopping (Shift+F5 again ends it at once)");
        return;
}

// The run ended: its status shown; where it failed, the editor goes there.
void EditWindow::runEnded(void *arg, TerminalWidget &terminal, int status) {
        auto  *self = static_cast<EditWindow *>(arg);
        String report(MemSpace::Psram);
        if (self->_report) {
                for (;;) {
                        uint8_t buf[256];
                        auto [n, err] = self->_report->read(WritableByteSpan(buf, sizeof(buf)), 1);
                        if (err.isError() || n == 0) break;
                        (void)report.append(StringView(reinterpret_cast<const char *>(buf), n));
                }
                self->_report.reset();
        }
        // Stopped (Shift+F5): said so, not as a failure.
        const bool stopped = self->_stopAsked;
        self->_stopAsked = false;
        char note[64];
        if (stopped)
                std::snprintf(note, sizeof(note), "\r\n\x1b[2m-- stopped --\x1b[0m\r\n");
        else if (status == 0)
                std::snprintf(note, sizeof(note), "\r\n\x1b[2m-- done --\x1b[0m\r\n");
        else
                std::snprintf(note, sizeof(note), "\r\n\x1b[1;31m-- ended with %d --\x1b[0m\r\n", status);
        terminal.show(StringView(note));
        if (stopped) {
                self->_runDoc = 0;
                self->_status->setText(0, "Stopped");
                return;
        }
        Document *doc = self->documentById(self->_runDoc);
        self->_runDoc = 0;
        LanguageSupport         *support = doc != nullptr ? self->supportFor(*doc) : nullptr;
        LanguageSupport::Failure failure;
        if (support == nullptr || !support->failure(report.view(), failure)) {
                self->_status->setText(0, status == 0 ? "Done" : "It ended with an error");
                return;
        }
        // In another file (one it includes): opened there.
        if (failure.file.view() != doc->path.view()) {
                self->open(failure.file.view());
                doc = self->current();
        } else {
                self->_tabs->setCurrent(size_t(doc - self->_docs));
        }
        if (doc == nullptr) return;
        CodeEdit::Diagnostic d;
        d.line = failure.line > 0 ? failure.line - 1 : 0;
        d.start = failure.column > 0 ? failure.column - 1 : 0;
        d.end = d.start;
        d.severity = CodeEdit::Severity::Error;
        (void)d.message.assign(failure.message.view());
        (void)doc->edit->setDiagnostics(Span<const CodeEdit::Diagnostic>(&d, 1));
        doc->edit->setSelection(TextSelection{TextPosition{d.line, d.start}, TextPosition{d.line, d.start}});
        doc->edit->setFocus();
        self->showProblems();
        String text(MemSpace::Psram);
        (void)text.appendf("Line %u: %.*s", unsigned(failure.line), int(failure.message.size()),
                           failure.message.data());
        self->_status->setText(0, text.view());
        return;
}

// ---- Help and completion

// The word at the cursor (or just before it) and what it's a member of: `w.fill` gives w and
// fill, `Color::Red` gives Color and Red.
void EditWindow::wordAtCursor(StringView &owner, StringView &word, TextRange &range) {
        owner = word = StringView();
        Document *doc = current();
        if (doc == nullptr) return;
        const TextDocument &d = doc->edit->document();
        const TextPosition  p = doc->edit->selection().cursor;
        range = d.wordAt(p);
        if (range.isEmpty()) range = TextRange{p, p};
        const StringView line = d.line(p.line);
        word = line.mid(range.start.byte, range.end.byte - range.start.byte);
        // What's before it: `.` or `::`, then a word.
        uint32_t at = range.start.byte;
        if (at >= 1 && line[at - 1] == '.')
                at -= 1;
        else if (at >= 2 && line[at - 1] == ':' && line[at - 2] == ':')
                at -= 2;
        else
                return;
        uint32_t start = at;
        while (start > 0 && TextDocument::isWordByte(line[start - 1])) --start;
        owner = line.mid(start, at - start);
        return;
}

// @p owner's type from the text, for a script that doesn't build yet: the nearest declaration
// before the cursor of `owner` after a known type's name (`Window w(`, `Ball@ b =`). Empty if
// none (the checker's symbols decide then).
String EditWindow::declaredType(StringView owner) {
        String           type(MemSpace::Psram);
        Document        *doc = current();
        LanguageSupport *support = doc != nullptr ? supportFor(*doc) : nullptr;
        if (support == nullptr || owner.isEmpty()) return type;
        List<String> types(MemSpace::Psram);
        support->typeNames(types);
        const auto known = [&](StringView t) {
                for (const String &k : types)
                        if (k.view() == t) return true;
                return false;
        };
        const TextDocument &d = doc->edit->document();
        for (uint32_t line = doc->edit->selection().cursor.line + 1; line-- > 0 && type.isEmpty();) {
                const StringView text = d.line(line);
                for (size_t at = text.indexOf(owner); at != StringView::NPos; at = text.indexOf(owner, at + 1)) {
                        const size_t end = at + owner.size();
                        if ((at > 0 && TextDocument::isWordByte(text[at - 1])) ||
                            (end < text.size() && TextDocument::isWordByte(text[end])))
                                continue;
                        // Back over spaces and a handle's @, then the type's name.
                        size_t b = at;
                        while (b > 0 && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '@')) --b;
                        size_t a = b;
                        while (a > 0 && TextDocument::isWordByte(text[a - 1])) --a;
                        if (a < b && b < at && known(text.mid(a, b - a))) {
                                (void)type.assign(text.mid(a, b - a));
                                break;
                        }
                }
        }
        return type;
}

void EditWindow::helpAtCursor() {
        Document        *doc = current();
        LanguageSupport *support = doc != nullptr ? supportFor(*doc) : nullptr;
        StringView       owner, word;
        TextRange        range;
        wordAtCursor(owner, word, range);
        String text(MemSpace::Psram);
        if (support == nullptr) {
                (void)text.assign("Help is for AngelScript scripts, for now.\n");
        } else if (word.isEmpty()) {
                (void)text.assign("Put the cursor on a name and press F1.\n");
        } else {
                const String type = declaredType(owner);
                support->help(type.isEmpty() ? owner : type.view(), word, text);
                if (text.isEmpty())
                        (void)text.appendf("Nothing known about %.*s%s%.*s.\n", int(owner.size()), owner.data(),
                                           owner.isEmpty() ? "" : ".", int(word.size()), word.data());
        }
        (void)_helpView->setText(text.view());
        showPane(HelpTab);
        return;
}

void EditWindow::complete() {
        Document        *doc = current();
        LanguageSupport *support = doc != nullptr ? supportFor(*doc) : nullptr;
        if (support == nullptr) return;
        StringView owner, word;
        TextRange  range;
        wordAtCursor(owner, word, range);
        // Only what is before the cursor is the prefix.
        const TextPosition p = doc->edit->selection().cursor;
        if (range.end > p) range.end = p;
        const StringView line = doc->edit->document().line(p.line);
        const StringView prefix = line.mid(range.start.byte, range.end.byte - range.start.byte);
        _completions.clear();
        const String type = declaredType(owner);
        support->complete(type.isEmpty() ? owner : type.view(), prefix, _completions);
        if (_completions.isEmpty()) {
                _status->setText(0, "Nothing to complete");
                return;
        }
        _completionRange = range;
        _completionMenu.clear();
        for (size_t i = 0; i < _completions.size() && i < MaxCompletions; ++i) {
                StringView   name = _completions[i]->name.view();
                const size_t dot = name.lastIndexOf('.');
                const size_t colon = name.lastIndexOf(':');
                const size_t cut = dot != StringView::NPos ? dot + 1 : colon != StringView::NPos ? colon + 1 : 0;
                name = name.mid(cut);
                // The name, then what it is (as much as fits).
                char      text[Menu::MaxText + 1];
                const int n = std::snprintf(text, sizeof(text), "%.*s  %.*s", int(name.size()), name.data(),
                                            int(_completions[i]->decl.size()), _completions[i]->decl.data());
                (void)_completionMenu.addItem(
                        StringView(text, n > 0 ? size_t(n) < sizeof(text) ? size_t(n) : sizeof(text) - 1 : 0),
                        ChooseCompletion + int(i));
        }
        const Rect c = doc->edit->cursorRect();
        const Rect s = screenRect();
        if (_completionMenu.popup(Rect{uint16_t(s.x + c.x), uint16_t(s.y + c.y), c.width, c.height}).isOk())
                _completionMenu.highlightFirst();
        return;
}

void EditWindow::completionChosen(void *arg, int item) {
        auto        *self = static_cast<EditWindow *>(arg);
        const size_t i = size_t(item - ChooseCompletion);
        Document    *doc = self->current();
        if (doc == nullptr || i >= self->_completions.size()) return;
        StringView   name = self->_completions[i]->name.view();
        const size_t dot = name.lastIndexOf('.');
        const size_t colon = name.lastIndexOf(':');
        name = name.mid(dot != StringView::NPos ? dot + 1 : colon != StringView::NPos ? colon + 1 : 0);
        doc->edit->setSelection(TextSelection{self->_completionRange.end, self->_completionRange.start});
        doc->edit->insertText(name);
        doc->edit->setFocus();
        return;
}

// ---- Templates and examples

void EditWindow::newFromTemplate(size_t index) {
        if (index >= sizeof(Templates) / sizeof(Templates[0])) return;
        // An untitled, untouched, empty document is used rather than kept beside.
        Document *doc = current();
        if (doc == nullptr || !doc->path.isEmpty() || doc->edit->document().isModified() ||
            doc->edit->document().length() != 0) {
                newDocument();
                doc = current();
        }
        if (doc == nullptr) return;
        (void)doc->edit->setText(StringView(Templates[index].text));
        doc->edit->setSyntax(Templates[index].syntax != nullptr ? Syntax::find(Templates[index].syntax) : nullptr);
        if (supportFor(*doc) != nullptr) {
                namesChanged();
                scheduleCheck(0);
        }
        showStatus();
        return;
}

void EditWindow::examplesChosen(void *arg, FileDialog &dialog, int result) {
        auto *self = static_cast<EditWindow *>(arg);
        if (result != FileDialog::Accepted) return;
        for (size_t i = 0; i < dialog.count(); ++i) self->open(dialog.path(i));
        return;
}

BMDS_NAMESPACE_END
