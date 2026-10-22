/**
 * @file      editdebug.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * edit's debugger (devplan/editor.md §17): breakpoints in the documents, a debugged run's stops
 * shown in the editor (the line about to run), the Debug tab's call stack and variables, and
 * watches.
 */

#include <cstdio>
#include <new>
#include <bmds/memory.h>
#include <bmds/vfs.h>
#include "editwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   DebugTab = 3; // the pane's tab
        constexpr size_t   OutputTab = 0;
        constexpr uint32_t LocalsCode = 0xffff; // a token's code: the frame's variables
        constexpr uint32_t WatchCode = 0x8000;  // + the watch's index
        constexpr size_t   MaxWatches = 32;
        constexpr uint16_t StackShare = 3; // the call stack's part of the tab's width: a third

} // namespace

Error EditWindow::buildDebugTab() {
        _debugSplit = Widget::create<Splitter>(_pane, Splitter::Orientation::Horizontal);
        if (_debugSplit == nullptr) return Error::NoMem;
        _stackView = Widget::create<TextEdit>(_debugSplit);
        _varsView = Widget::create<TreeView>(_debugSplit);
        if (_stackView == nullptr || _varsView == nullptr) return Error::NoMem;
        _debugSplit->setWidgets(_stackView, _varsView);
        _debugSplit->setLeast(96, 96);
        _stackView->setReadOnly(true);
        _stackView->setCursorMoved(framePicked, this);
        _varsView->setExpanding(varsExpanding, this);
        (void)_stackView->setText("Not debugging.\n\nF9 sets a breakpoint on the cursor's line; F5 runs to it.\n"
                                  "F11 starts at the first line.\n");
        return _pane->addTab(_debugSplit, "Debug");
}

// ---- Breakpoints

bool EditWindow::hasBreakpoints() {
        for (size_t i = 0; i < _count; ++i)
                if (_docs[i].edit->areBreakpointsEnabled() && !_docs[i].edit->breakpoints().isEmpty()) return true;
        return false;
}

// What changed since the debugger was last told, told now (while a debugged run is under way).
void EditWindow::syncBreakpoints(Document &doc) {
        if (_debug == nullptr || !_debug->isActive() || doc.path.isEmpty() || !doc.edit->areBreakpointsEnabled())
                return;
        const Span<const uint32_t> now = doc.edit->breakpoints();
        const auto                 has = [](Span<const uint32_t> lines, uint32_t line) {
                for (uint32_t l : lines)
                        if (l == line) return true;
                return false;
        };
        for (uint32_t line : doc.sentBreaks.span())
                if (!has(now, line)) (void)_debug->setBreakpoint(doc.path.view(), line + 1, false);
        for (uint32_t line : now)
                if (!has(doc.sentBreaks.span(), line)) (void)_debug->setBreakpoint(doc.path.view(), line + 1, true);
        doc.sentBreaks.clear();
        for (uint32_t line : now) (void)doc.sentBreaks.append(uint32_t(line));
        return;
}

void EditWindow::breakpointsChanged(void *arg, CodeEdit &edit) {
        auto     *self = static_cast<EditWindow *>(arg);
        const int index = self->indexOf(&edit);
        if (index >= 0) self->syncBreakpoints(self->_docs[index]);
        return;
}

// ---- Running and stopping

void EditWindow::resume(DebugSession::Resume how) {
        if (_debug == nullptr || !_debug->isActive() || !_debug->isStopped()) return;
        if (_debug->resume(how).isError()) return;
        for (size_t i = 0; i < _count; ++i) _docs[i].edit->setExecutionLine(CodeEdit::NoLine);
        // What it reads is typed in the Output while it runs.
        showPane(OutputTab);
        _status->setText(0, "Running (Pause stops it where it is, Shift+F5 ends it)");
        return;
}

void EditWindow::debugEnded() {
        for (size_t i = 0; i < _count; ++i) {
                _docs[i].edit->setExecutionLine(CodeEdit::NoLine);
                _docs[i].sentBreaks.clear();
        }
        _frames.clear();
        _varNodes.clear();
        ++_generation;
        _stackShowing = true;
        (void)_stackView->setText("The program ended.\n");
        _stackShowing = false;
        _varsView->clear();
        return;
}

void EditWindow::debugStopped(StringView reason, StringView file, uint32_t line, StringView message) {
        _frames.clear();
        _frame = 0;
        showExecution(file, line);
        showPane(DebugTab);
        // The call stack a third of the tab at first (laid out by now: the tab is shown).
        if (!_debugSplitPlaced && _debugSplit->geometry().width != 0) {
                _debugSplit->setPosition(uint16_t(_debugSplit->geometry().width / StackShare));
                _debugSplitPlaced = true;
        }
        String text(MemSpace::Psram);
        if (reason == "exception")
                (void)text.appendf("Line %u: %.*s (stopped there: F5 ends it)", unsigned(line), int(message.size()),
                                   message.data());
        else
                (void)text.appendf("Stopped at line %u%s: F5 goes on, F10 steps over, F11 into, Shift+F11 out",
                                   unsigned(line), reason == "break" ? " (a breakpoint)" : "");
        _status->setText(0, text.view());
        activate(); // over the program's own windows
        (void)_debug->askStack();
        refreshVariables();
        return;
}

// The editor at @p file's @p line (from 1), marked as the line about to run; opened if it isn't.
void EditWindow::showExecution(StringView file, uint32_t line) {
        for (size_t i = 0; i < _count; ++i) _docs[i].edit->setExecutionLine(CodeEdit::NoLine);
        if (line == 0 || file.isEmpty()) return;
        auto [path, err] = Vfs::normalize(file, _cwd.view());
        if (err.isError()) return;
        int index = indexOfPath(path.view());
        if (index < 0) {
                open(path.view());
                index = indexOfPath(path.view());
        }
        if (index < 0) return;
        Document &doc = _docs[index];
        _tabs->setCurrent(size_t(index));
        doc.edit->setExecutionLine(line - 1);
        doc.edit->setSelection(TextSelection{TextPosition{line - 1, 0}, TextPosition{line - 1, 0}});
        doc.edit->setFocus();
        return;
}

void EditWindow::debugStack(Span<const DebugSession::Frame> frames) {
        _frames.clear();
        String text(MemSpace::Psram);
        for (const DebugSession::Frame &f : frames) {
                DebugSession::Frame copy;
                copy.level = f.level;
                (void)copy.function.assign(f.function.view());
                (void)copy.file.assign(f.file.view());
                copy.line = f.line;
                (void)_frames.append(std::move(copy));
                const StringView file = f.file.view();
                const size_t     slash = file.lastIndexOf('/');
                const StringView name = slash == StringView::NPos ? file : file.mid(slash + 1);
                (void)text.appendf("%.*s  %.*s:%u\n", int(f.function.size()), f.function.data(), int(name.size()),
                                   name.data(), unsigned(f.line));
        }
        _stackShowing = true;
        (void)_stackView->setText(text.view());
        _stackView->setSelection(TextSelection{TextPosition{uint32_t(_frame), 0}, TextPosition{uint32_t(_frame), 0}});
        _stackShowing = false;
        return;
}

// A click (or the arrows) on a frame of the call stack: its line shown, its variables.
void EditWindow::framePicked(void *arg, TextEdit &edit) {
        auto *self = static_cast<EditWindow *>(arg);
        if (self->_stackShowing || !edit.hasFocus()) return;
        const uint32_t line = edit.selection().cursor.line;
        if (line < self->_frames.size() && line != self->_frame) self->showFrame(line);
        return;
}

void EditWindow::showFrame(size_t index) {
        if (index >= _frames.size()) return;
        _frame = index;
        const DebugSession::Frame &f = _frames[index];
        auto [path, err] = Vfs::normalize(f.file.view(), _cwd.view());
        if (err.isOk()) {
                int at = indexOfPath(path.view());
                if (at < 0) {
                        open(path.view());
                        at = indexOfPath(path.view());
                }
                if (at >= 0 && f.line > 0) {
                        _tabs->setCurrent(size_t(at));
                        _docs[at].edit->setSelection(
                                TextSelection{TextPosition{f.line - 1, 0}, TextPosition{f.line - 1, 0}});
                }
        }
        refreshVariables();
        _stackView->setFocus(); // the arrows go on through the frames
        return;
}

// ---- The variables

String EditWindow::varKey(int level, StringView path) const {
        String key(MemSpace::Psram);
        (void)key.appendf("%c%.*s", level == DebugSession::Globals ? 'g' : 'l', int(path.size()), path.data());
        return key;
}

// Asked again for the frame shown: the watches, its variables, then the globals; what was
// expanded is expanded again as the answers come.
void EditWindow::refreshVariables() {
        if (_debug == nullptr || !_debug->isStopped()) return;
        // What is expanded now stays so (a row collapsed is let go).
        if (_varsView->rows().count() != 0) {
                _expanded.clear();
                for (size_t r = 0; r < _varsView->rows().count(); ++r) {
                        const TreeRows::Row &row = _varsView->rows().row(r);
                        if ((row.flags & TreeRows::Expanded) == 0 || row.data == 0 || row.data > _varNodes.size())
                                continue;
                        const VarNode &node = _varNodes[row.data - 1];
                        (void)_expanded.append(varKey(node.level, node.path.view()));
                }
        }
        ++_generation;
        _varsView->clear();
        _varNodes.clear();
        const int      level = _frame < _frames.size() ? int(_frames[_frame].level) : 0;
        const uint32_t base = (_generation & 0xffffu) << 16;
        for (size_t i = 0; i < _watches.size(); ++i)
                (void)_debug->askValue(level, _watches[i].view(), base | WatchCode | uint32_t(i));
        (void)_debug->askVars(level, StringView(), base | LocalsCode);
        return;
}

void EditWindow::debugVars(uint32_t token, Span<const DebugSession::Var> vars, StringView error) {
        if ((token >> 16) != (_generation & 0xffffu)) return; // asked before the last stop
        const uint32_t code = token & 0xffffu;
        const int      level = _frame < _frames.size() ? int(_frames[_frame].level) : 0;
        if (code == LocalsCode) {
                addVarRows(-1, level, StringView(), -1, vars);
                _localsCount = uint32_t(vars.size());
                // Then the globals: open at first where a script's variables are (the top level).
                VarNode node;
                node.level = DebugSession::Globals;
                if (_varNodes.append(std::move(node)).isError() || _varsView->addRoot("Globals").isError()) return;
                const size_t row = _varsView->rows().count() - 1;
                _varsView->setRowData(row, _varNodes.size());
                const bool top =
                        _frame < _frames.size() ? _frames[_frame].function.view() == "the top level" : _frame == 0;
                bool kept = false;
                for (const String &k : _expanded) kept = kept || k.view() == "g";
                if (kept || top || vars.isEmpty()) _varsView->expand(row);
                return;
        }
        if ((code & WatchCode) != 0) {
                const size_t watch = code & ~WatchCode;
                if (watch >= _watches.size()) return;
                if (!vars.isEmpty()) {
                        addVarRows(-1, level, StringView(), int(watch), vars.first(1));
                        return;
                }
                // It can't be shown here: why, on its row.
                String text(MemSpace::Psram);
                (void)text.appendf("watch %.*s: %.*s", int(_watches[watch].size()), _watches[watch].data(),
                                   int(error.size()), error.data());
                VarNode node;
                node.level = level;
                (void)node.path.assign(_watches[watch].view());
                node.watch = int(watch);
                if (_varNodes.append(std::move(node)).isError() || _varsView->addRoot(text.view()).isError()) return;
                const size_t row = _varsView->rows().count() - 1;
                _varsView->setRowData(row, _varNodes.size());
                _varsView->setHasChildren(row, false);
                return;
        }
        // A row's children: the row found by its node (rows may have moved since it asked).
        const uintptr_t data = uintptr_t(code) + 1;
        for (size_t r = 0; r < _varsView->rows().count(); ++r) {
                if (_varsView->rows().row(r).data != data) continue;
                const VarNode &node = _varNodes[code];
                String         parent(MemSpace::Psram);
                (void)parent.assign(node.path.view());
                addVarRows(int(r), node.level, parent.view(), -1, vars);
                return;
        }
        return;
}

// Rows for @p vars: roots (@p row < 0), else @p row's children; each row's node is what its
// children are asked with.
void EditWindow::addVarRows(int row, int level, StringView parent, int watch, Span<const DebugSession::Var> vars) {
        List<String>     texts(MemSpace::Psram);
        List<StringView> names(MemSpace::Psram);
        for (const DebugSession::Var &v : vars) {
                String           text(MemSpace::Psram);
                const StringView name = watch >= 0 ? _watches[size_t(watch)].view() : v.name.view();
                (void)text.appendf("%s%.*s: %.*s = %.*s", watch >= 0 ? "watch " : "", int(name.size()), name.data(),
                                   int(v.type.size()), v.type.data(), int(v.value.size()), v.value.data());
                (void)texts.append(std::move(text));
        }
        for (const String &t : texts) (void)names.append(t.view());
        // The rows the vars land on, in order.
        List<size_t> rows(MemSpace::Psram);
        if (row < 0) {
                for (const StringView n : names) {
                        if (_varsView->addRoot(n).isError()) break;
                        (void)rows.append(_varsView->rows().count() - 1);
                }
        } else {
                if (_varsView->setChildren(size_t(row), names.span()).isError()) return;
                const uint8_t depth = uint8_t(_varsView->rows().row(size_t(row)).depth + 1);
                for (size_t r = size_t(row) + 1;
                     r < _varsView->rows().count() && _varsView->rows().row(r).depth >= depth; ++r)
                        if (_varsView->rows().row(r).depth == depth) (void)rows.append(r);
        }
        for (size_t i = 0; i < rows.size() && i < vars.size(); ++i) {
                const DebugSession::Var &v = vars[i];
                VarNode                  node;
                node.level = level;
                node.watch = watch;
                if (watch >= 0)
                        (void)node.path.assign(_watches[size_t(watch)].view());
                else if (parent.isEmpty())
                        (void)node.path.assign(v.name.view());
                else
                        (void)node.path.appendf("%.*s%s%.*s", int(parent.size()), parent.data(),
                                                v.name.view().startsWith('[') ? "" : ".", int(v.name.size()),
                                                v.name.data());
                const String key = varKey(level, node.path.view());
                if (_varNodes.append(std::move(node)).isError()) return;
                _varsView->setRowData(rows[i], _varNodes.size());
                _varsView->setHasChildren(rows[i], v.children > 0);
                if (v.children == 0) continue;
                for (const String &k : _expanded)
                        if (k.view() == key.view()) {
                                _varsView->expand(rows[i]);
                                break;
                        }
        }
        return;
}

// A row expanded: its members or elements asked for.
void EditWindow::varsExpanding(void *arg, TreeView &view, size_t row) {
        auto           *self = static_cast<EditWindow *>(arg);
        const uintptr_t data = view.rows().row(row).data;
        if (data == 0 || data > self->_varNodes.size() || self->_debug == nullptr || !self->_debug->isStopped()) return;
        const VarNode &node = self->_varNodes[data - 1];
        const uint32_t token = ((self->_generation & 0xffffu) << 16) | uint32_t(data - 1);
        (void)self->_debug->askVars(node.level, node.path.view(), token);
        return;
}

// ---- Watches

// The selection (on one line), or the name at the cursor with what it's a member of.
void EditWindow::addWatch() {
        Document *doc = current();
        if (doc == nullptr) return;
        String what(MemSpace::Psram);
        if (doc->edit->selection().hasSelection()) {
                if (doc->edit->selectedText(what).isError()) return;
                if (what.view().contains('\n')) what.clear();
        } else {
                StringView owner, word;
                TextRange  range;
                wordAtCursor(owner, word, range);
                if (!owner.isEmpty() && !word.isEmpty())
                        (void)what.appendf("%.*s.%.*s", int(owner.size()), owner.data(), int(word.size()), word.data());
                else
                        (void)what.assign(word);
        }
        (void)what.assign(what.view().trimmed());
        if (what.isEmpty()) {
                _status->setText(0, "Select what to watch, or put the cursor on a name");
                return;
        }
        for (const String &w : _watches)
                if (w.view() == what.view()) return;
        if (_watches.size() == MaxWatches) {
                _status->setText(0, "Too many watches (Debug > Clear Watches)");
                return;
        }
        String text(MemSpace::Psram);
        (void)text.appendf("Watching %.*s", int(what.size()), what.data());
        (void)_watches.append(std::move(what));
        _status->setText(0, text.view());
        showPane(DebugTab);
        refreshVariables();
        return;
}

// Delete on a watch's row (or anything under it): the watch goes.
void EditWindow::removeWatch() {
        int row = _varsView->current();
        while (row >= 0 && _varsView->rows().row(size_t(row)).depth > 0) row = _varsView->rows().parent(size_t(row));
        if (row < 0) return;
        const uintptr_t data = _varsView->rows().row(size_t(row)).data;
        if (data == 0 || data > _varNodes.size()) return;
        const int watch = _varNodes[data - 1].watch;
        if (watch < 0 || size_t(watch) >= _watches.size()) return;
        _watches.removeAt(size_t(watch));
        refreshVariables();
        return;
}

void EditWindow::clearWatches() {
        _watches.clear();
        refreshVariables();
        return;
}

BMDS_NAMESPACE_END
