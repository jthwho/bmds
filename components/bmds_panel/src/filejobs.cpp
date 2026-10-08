/**
 * @file      filejobs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel's long file operations and their windows (devplan/filebrowser.md §4j, FB13).
 */

#include <cstdio>
#include <new>
#include <bmds/fileops.h>
#include <bmds/memory.h>

#include "filejobs.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t DialogWidth = 360;
        constexpr uint16_t Rows = 5; // a dialog's: labels, the bar, the check box
        constexpr uint16_t DialogHeight = Rows * GuiStyle::ButtonHeight + (Rows - 1) * 8;

        using Kind = FileWorker::Kind;

        const char *verbOf(Kind kind) {
                return kind == Kind::Copy ? "Copying" : kind == Kind::Move ? "Moving" : "Deleting";
        }

        // The folder @p path is in, by its name ("/" for the root).
        StringView folderNameOf(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                if (slash == StringView::NPos || slash == 0) return StringView("/");
                return FileOps::baseName(path.left(slash));
        }

        template <typename T> void destroy(T *&window) {
                if (window == nullptr) return;
                window->~T();
                Memory::free(window);
                window = nullptr;
                return;
        }

} // namespace

void formatBytes(uint64_t bytes, char *out, size_t size) {
        const auto v = static_cast<unsigned long long>(bytes);
        if (bytes < 1024) {
                std::snprintf(out, size, "%llu bytes", v);
        } else if (bytes < 10ull * 1024 * 1024) {
                std::snprintf(out, size, "%llu KB", (v + 1023) / 1024);
        } else if (bytes < 10ull * 1024 * 1024 * 1024) {
                std::snprintf(out, size, "%llu MB", (v + (1 << 20) - 1) >> 20);
        } else {
                std::snprintf(out, size, "%llu GB", (v + (1ull << 30) - 1) >> 30);
        }
        return;
}

FileJobs::~FileJobs() {
        if (_showTimer >= 0) _app.stopTimer(_showTimer);
        destroy(_progress);
        destroy(_conflict);
}

void FileJobs::copy(StringView paths, StringView target) {
        add(Kind::Copy, paths, target);
        return;
}

void FileJobs::move(StringView paths, StringView target) {
        add(Kind::Move, paths, target);
        return;
}

void FileJobs::add(Kind kind, StringView paths, StringView target) {
        const Error err = _worker.add(kind, paths, target).error();
        if (err.isError() && err != Error::Invalid) {
                char message[120];
                std::snprintf(message, sizeof(message), "Can't start it: %s.", err.desc());
                (void)MessageBox::show(_app, MessageBox::Kind::Error, StringView(verbOf(kind)), StringView(message),
                                       MessageBox::Buttons::Ok, nullptr, nullptr);
        }
        return;
}

// ---- News from the worker

void FileJobs::notified(void *arg) {
        auto *self = static_cast<FileJobs *>(arg);
        if (self->_posted.exchange(true, std::memory_order_acq_rel)) return;
        // A full queue: the next news posts again.
        if (self->_app.post(due, self).isError()) self->_posted.store(false, std::memory_order_release);
        return;
}

void FileJobs::due(void *arg) {
        auto *self = static_cast<FileJobs *>(arg);
        self->_posted.store(false, std::memory_order_release);
        self->update();
        return;
}

void FileJobs::update() {
        _worker.reap();
        FileWorker::Done done;
        while (_worker.takeDone(done)) tellDone(done);
        _worker.status(_status);
        if (_status.job != 0 && _status.job != _seenJob) {
                // Shown once work has gone on for a second.
                _seenJob = _status.job;
                if (!_progressShown && _showTimer < 0) {
                        auto [timer, err] = _app.startTimer(ShowAfterMs, false, showDue, this);
                        if (err.isOk()) _showTimer = timer;
                }
        }
        if (_status.job == 0 && !_worker.isBusy()) {
                if (_showTimer >= 0) {
                        _app.stopTimer(_showTimer);
                        _showTimer = -1;
                }
                closeProgress();
        }
        if (_progressShown) showProgress();
        if (_status.asking && !_conflictShown) ask();
        return;
}

void FileJobs::showDue(void *arg) {
        auto *self = static_cast<FileJobs *>(arg);
        self->_showTimer = -1;
        self->_worker.status(self->_status);
        if (self->_status.job != 0) self->showProgress();
        return;
}

// ---- Progress

Error FileJobs::buildProgress() {
        if (_progress != nullptr) return Error::Ok;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Dialog));
        if (mem == nullptr) return Error::NoMem;
        _progress = ::new (mem) Dialog(_app, "Copying", DialogWidth, DialogHeight);
        BoxLayout *c = _progress->content();
        if (c != nullptr) {
                _what = Widget::create<Label>(c);
                _where = Widget::create<Label>(c);
                _bar = Widget::create<ProgressBar>(c);
                _counts = Widget::create<Label>(c);
                _waiting = Widget::create<Label>(c);
        }
        if (c == nullptr || _what == nullptr || _where == nullptr || _bar == nullptr || _counts == nullptr ||
            _waiting == nullptr || _progress->addButton("Cancel", 1) == nullptr) {
                destroy(_progress);
                return Error::NoMem;
        }
        _progress->setFinished(progressFinished, this);
        return Error::Ok;
}

// The window brought up to date with the status, opened if it isn't.
void FileJobs::showProgress() {
        const FileWorker::Status &s = _status;
        if (s.job == 0) return;
        if (buildProgress().isError()) return; // no window: the job goes on unseen
        char line[160];                        // a label keeps what fits (Label::MaxText)
        _progress->setTitle(StringView(verbOf(s.kind)));
        if (s.counting) {
                std::snprintf(line, sizeof(line), "Counting the items...");
        } else if (s.item.isEmpty()) {
                std::snprintf(line, sizeof(line), "%s...", verbOf(s.kind)); // asking before the first
        } else {
                const StringView name = FileOps::baseName(s.item.view());
                std::snprintf(line, sizeof(line), "%s \"%.*s\"", verbOf(s.kind), int(name.size()), name.data());
        }
        _what->setText(StringView(line));
        const StringView from = folderNameOf(s.item.view());
        if (s.item.isEmpty()) {
                line[0] = '\0';
        } else if (s.kind == Kind::Remove) {
                std::snprintf(line, sizeof(line), "From %.*s", int(from.size()), from.data());
        } else {
                const StringView to = FileOps::baseName(s.target.view());
                std::snprintf(line, sizeof(line), "From %.*s to %.*s", int(from.size()), from.data(), int(to.size()),
                              to.data());
        }
        _where->setText(StringView(line));
        // Bytes when there are any, else items.
        const bool bytes = s.totalBytes > 0;
        _bar->setMaximum(bytes ? s.totalBytes : s.totalItems);
        _bar->setValue(bytes ? s.bytes : s.items);
        const auto items = static_cast<unsigned long long>(s.items < s.totalItems ? s.items : s.totalItems);
        if (s.counting) {
                line[0] = '\0';
        } else if (bytes) {
                char done[24], all[24];
                formatBytes(s.bytes, done, sizeof(done));
                formatBytes(s.totalBytes, all, sizeof(all));
                std::snprintf(line, sizeof(line), "%llu of %llu items, %s of %s", items,
                              static_cast<unsigned long long>(s.totalItems), done, all);
        } else {
                std::snprintf(line, sizeof(line), "%llu of %llu items", items,
                              static_cast<unsigned long long>(s.totalItems));
        }
        _counts->setText(StringView(line));
        if (s.waiting > 0) {
                std::snprintf(line, sizeof(line), "%zu more waiting", s.waiting);
        } else {
                line[0] = '\0';
        }
        _waiting->setText(StringView(line));
        if (!_progressShown) {
                _progressShown = _progress->open().isOk();
                // Opened over the middle of the screen: a question there stays on top.
                if (_conflictShown) _conflict->activate();
        }
        return;
}

void FileJobs::closeProgress() {
        if (!_progressShown) return;
        _progressShown = false;
        _progress->close();
        return;
}

// Cancel (its only button, Escape, the close box): the job shown stopped.
void FileJobs::progressFinished(void *arg, Dialog &dialog, int result) {
        (void)dialog;
        (void)result;
        auto *self = static_cast<FileJobs *>(arg);
        self->_progressShown = false;
        self->_worker.cancel(self->_status.job);
        return;
}

// ---- A name taken

Error FileJobs::buildConflict() {
        if (_conflict != nullptr) return Error::Ok;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Dialog));
        if (mem == nullptr) return Error::NoMem;
        _conflict = ::new (mem) Dialog(_app, "Confirm File Replace", DialogWidth, DialogHeight);
        BoxLayout *c = _conflict->content();
        bool       made = c != nullptr;
        for (Label *&l : _lines)
                if (made) made = (l = Widget::create<Label>(c)) != nullptr;
        if (made) made = (_forAll = Widget::create<CheckBox>(c, "Do the same for the &others")) != nullptr;
        if (made) made = (_replace = _conflict->addButton("&Replace", AnswerReplace)) != nullptr;
        if (made) made = _conflict->addButton("Keep &Both", AnswerKeepBoth) != nullptr;
        if (made) made = _conflict->addButton("&Skip", AnswerSkip) != nullptr;
        if (made) made = _conflict->addButton("Cancel", AnswerCancel) != nullptr;
        if (!made) {
                destroy(_conflict);
                return Error::NoMem;
        }
        _conflict->setFinished(conflictFinished, this);
        return Error::Ok;
}

void FileJobs::ask() {
        const FileWorker::Status &s = _status;
        if (buildConflict().isError()) {
                _worker.answer(FileOps::Resolution::Skip, false); // no window to ask in
                return;
        }
        const bool       folder = s.askIsDirectory;
        const StringView name = FileOps::baseName(s.askTarget.view());
        const StringView in = folderNameOf(s.askTarget.view());
        const char      *moved = s.kind == Kind::Move ? "moved" : "copied";
        char             line[160]; // a label keeps what fits (Label::MaxText)
        _conflict->setTitle(folder ? "Confirm Folder Replace" : "Confirm File Replace");
        _lines[0]->setText(folder ? "There is already a folder named" : "There is already a file named");
        std::snprintf(line, sizeof(line), "\"%.*s\" in %.*s.", int(name.size()), name.data(), int(in.size()),
                      in.data());
        _lines[1]->setText(StringView(line));
        if (folder) {
                std::snprintf(line, sizeof(line), "Merge the one being %s into it?", moved);
        } else {
                std::snprintf(line, sizeof(line), "Replace it with the one being %s?", moved);
        }
        _lines[2]->setText(StringView(line));
        _replace->setText(folder ? "&Merge" : "&Replace");
        _forAll->setChecked(false);
        _conflictShown = _conflict->open().isOk();
        if (!_conflictShown) _worker.answer(FileOps::Resolution::Skip, false);
        return;
}

void FileJobs::conflictFinished(void *arg, Dialog &dialog, int result) {
        (void)dialog;
        auto *self = static_cast<FileJobs *>(arg);
        self->_conflictShown = false;
        const FileOps::Resolution r = result == AnswerReplace    ? FileOps::Resolution::Replace
                                      : result == AnswerKeepBoth ? FileOps::Resolution::KeepBoth
                                      : result == AnswerSkip     ? FileOps::Resolution::Skip
                                                                 : FileOps::Resolution::Cancel;
        self->_worker.answer(r, self->_forAll->isChecked());
        return;
}

// ---- Done

void FileJobs::tellDone(const FileWorker::Done &done) {
        if (done.err.isOk() || done.err == Error::Interrupted) return; // cancelled: asked for
        const char      *verb = done.kind == Kind::Copy ? "copied" : done.kind == Kind::Move ? "moved" : "deleted";
        const char      *title = done.kind == Kind::Copy ? "Copy" : done.kind == Kind::Move ? "Move" : "Delete";
        const StringView path = done.failed.view();
        char             message[MessageBox::MaxText + 1];
        if (done.failures <= 1) {
                std::snprintf(message, sizeof(message), "%.*s couldn't be %s: %s.", int(path.size()), path.data(), verb,
                              done.err.desc());
        } else {
                std::snprintf(message, sizeof(message), "%zu items couldn't be %s. The first, %.*s: %s.", done.failures,
                              verb, int(path.size()), path.data(), done.err.desc());
        }
        (void)MessageBox::show(_app, MessageBox::Kind::Error, StringView(title), StringView(message),
                               MessageBox::Buttons::Ok, nullptr, nullptr);
        return;
}

// ---- Deleting for good

void FileJobs::removeAsking(StringView paths) {
        if (!_removing.isEmpty()) return; // one question at a time
        size_t     count = 0;
        StringView names[3];
        StringView rest = paths;
        while (!rest.isEmpty()) {
                const size_t     end = rest.indexOf('\n');
                const StringView path = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (path.isEmpty()) continue;
                if (count < 3) names[count] = FileOps::baseName(path);
                ++count;
        }
        if (count == 0 || _removing.assign(paths).isError()) return;
        char   message[MessageBox::MaxText + 1];
        size_t n = 0;
        if (count == 1) {
                n = size_t(std::snprintf(message, sizeof(message), "Delete \"%.*s\" for good?", int(names[0].size()),
                                         names[0].data()));
        } else {
                n = size_t(std::snprintf(message, sizeof(message), "Delete these %zu items for good?\n\n", count));
                for (size_t i = 0; i < count && i < 3 && n < sizeof(message); ++i)
                        n += size_t(std::snprintf(message + n, sizeof(message) - n, "%s\"%.*s\"",
                                                  i == 0 ? "" : (i + 1 == count ? " and " : ", "), int(names[i].size()),
                                                  names[i].data()));
                if (count > 3 && n < sizeof(message))
                        n += size_t(std::snprintf(message + n, sizeof(message) - n, " and %zu more", count - 3));
                if (n < sizeof(message)) n += size_t(std::snprintf(message + n, sizeof(message) - n, "."));
        }
        if (n < sizeof(message))
                std::snprintf(message + n, sizeof(message) - n, "\n\n%s won't go to the trash.",
                              count == 1 ? "It" : "They");
        if (MessageBox::show(_app, MessageBox::Kind::Warning, "Delete", StringView(message), MessageBox::Buttons::YesNo,
                             confirmed, this)
                    .isError())
                _removing.clear();
        return;
}

void FileJobs::confirmed(void *arg, MessageBox::Answer answer) {
        auto *self = static_cast<FileJobs *>(arg);
        if (answer == MessageBox::Answer::Yes) self->add(Kind::Remove, self->_removing.view(), StringView());
        self->_removing.clear();
        return;
}

BMDS_NAMESPACE_END
