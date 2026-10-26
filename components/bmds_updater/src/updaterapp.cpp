/**
 * @file      updaterapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/updater [FILE] (devplan/update.md §14): an update package's version against the one
 * installed, how it checks, its release notes, and Install, which writes the recovery firmware if
 * it differs and restarts into recovery to install the rest. Without FILE, the newest package in
 * update/dir, or the last update's result. An ordinary GUI program on the toolkit; checking and
 * installing run on a thread of their own, told to the window through the loop.
 */

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/bevel.h>
#include <bmds/builtinprograms.h>
#include <bmds/datetime.h>
#include <bmds/dialog.h>
#include <bmds/fdtable.h>
#include <bmds/foldermodel.h>
#include <bmds/bitmapfont.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/painter.h>
#include <bmds/scrollarea.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/thread.h>
#include <bmds/update.h>
#include <bmds/vfs.h>
#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t CellHeight = 16;
        constexpr uint16_t NotesPad = 4;
        constexpr size_t   MaxNotesBytes = 64 * 1024;
        constexpr uint32_t RestartDelayMs = 1000; // "Restarting to install..." seen first
        constexpr size_t   WorkerStackBytes = 8 * 1024;

        // Release notes: plain text, wrapped to the width at word ends, scrolled.
        class NotesView final : public ScrollArea {
                public:
                        explicit NotesView(Widget *parent)
                            : ScrollArea(parent), _text(MemSpace::Psram), _lines(MemSpace::Psram) {
                                setLineSteps(8, CellHeight);
                                setFocusable(true); // Tab or a click: the keys scroll it
                        }

                        // Up, Down, Page Up, Page Down, Home and End scroll it.
                        bool keyPressed(const KeyEvent &event) override {
                                if (event.action == KeyEvent::Action::Release) return false;
                                const int64_t page =
                                        viewport().height > CellHeight ? viewport().height - CellHeight : CellHeight;
                                int64_t y = scrollY();
                                switch (event.usage) {
                                        case HidKey::Up: y -= CellHeight; break;
                                        case HidKey::Down: y += CellHeight; break;
                                        case HidKey::PageUp: y -= page; break;
                                        case HidKey::PageDown: y += page; break;
                                        case HidKey::Home: y = 0; break;
                                        case HidKey::End: y = contentHeight(); break;
                                        default: return false;
                                }
                                scrollTo(0, y); // kept in range
                                return true;
                        }

                        void setText(StringView text) {
                                (void)_text.assign(text);
                                rewrap();
                                update();
                                return;
                        }

                        void resized() override {
                                rewrap();
                                return;
                        }

                protected:
                        void paintContent(Painter &painter, const Rect &viewport) override {
                                const uint32_t bg = painter.palette().color(WindowPalette::Role::Face);
                                const uint32_t fg = painter.palette().color(WindowPalette::Role::FaceText);
                                (void)painter.fill(viewport, bg);
                                const int x = (windowX(NotesPad) + 3) & ~3;
                                for (size_t i = 0; i < _lines.size(); ++i) {
                                        const int y = windowY(NotesPad + uint32_t(i) * CellHeight);
                                        if (y + CellHeight <= viewport.y) continue;
                                        if (y >= viewport.bottom()) break;
                                        if (x >= viewport.right()) break;
                                        const Line &l = _lines[i];
                                        (void)painter.text(x, y, StringView(_text.view().data() + l.at, l.bytes),
                                                           Fonts::unscii16, fg, bg, uint16_t(viewport.right() - x));
                                }
                                return;
                        }

                private:
                        struct Line {
                                        uint32_t at = 0;
                                        uint16_t bytes = 0;
                        };

                        // Wrapped to the viewport; again if that made the vertical bar appear (or go), which
                        // changes the viewport's width: no horizontal bar, nothing cut at the right.
                        void rewrap() {
                                for (int pass = 0; pass < 2; ++pass) {
                                        const uint16_t before = viewport().width;
                                        wrapTo(before);
                                        if (viewport().width == before) break;
                                }
                                return;
                        }

                        // Lines of at most @p viewportWidth's columns, broken after a space where there is one.
                        void wrapTo(uint16_t viewportWidth) {
                                _lines.clear();
                                const uint16_t   width = viewportWidth > 2 * NotesPad + 160
                                                                 ? uint16_t(viewportWidth - 2 * NotesPad)
                                                                 : 160;
                                const size_t     columns = width / Fonts::unscii16.width();
                                const StringView t = _text.view();
                                size_t           start = 0;
                                while (start < t.size()) {
                                        size_t i = start, chars = 0, lastSpace = StringView::NPos;
                                        while (i < t.size() && t[i] != '\n' && chars < columns) {
                                                if (t[i] == ' ') lastSpace = i;
                                                i += 1;
                                                while (i < t.size() && (uint8_t(t[i]) & 0xc0) == 0x80)
                                                        ++i; // a character's rest
                                                ++chars;
                                        }
                                        size_t end = i, next = i;
                                        if (i < t.size() && t[i] == '\n') {
                                                next = i + 1;
                                        } else if (i < t.size() && lastSpace != StringView::NPos) {
                                                end = lastSpace;
                                                next = lastSpace + 1;
                                        }
                                        (void)_lines.append(Line{uint32_t(start), uint16_t(end - start)});
                                        start = next;
                                }
                                setContentSize(viewportWidth, uint32_t(_lines.size()) * CellHeight + 2 * NotesPad);
                                return;
                        }

                        String     _text;
                        List<Line> _lines;
        };

        void say(Label *label, const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

        void say(Label *label, const char *fmt, ...) {
                char    text[Label::MaxText + 1];
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(text, sizeof(text), fmt, args);
                va_end(args);
                label->setText(StringView(text));
                return;
        }

        class UpdaterWindow final : public GuiWindow {
                public:
                        UpdaterWindow(GuiApplication &app, const Config &config)
                            : GuiWindow(app, config), _confirm(app, "Install", 440, 48), _path(MemSpace::Psram),
                              _notesText(MemSpace::Psram) {}

                        ~UpdaterWindow() override {
                                _worker.join();
                                application().cancelPosts(this);
                        }

                        Error build(StringView path);

                protected:
                        void closedByManager() override {
                                if (!_busy) application().quit(0);
                                return;
                        }

                private:
                        enum class Job : uint8_t {
                                Check,
                                Install
                        };

                        static void workerMain(void *arg);
                        static void checked(void *arg);
                        static void progressed(void *arg);
                        static void installed(void *arg);
                        static void restart(void *arg);
                        static void clicked(void *arg, Button &button);
                        static void confirmed(void *arg, Dialog &dialog, int result);
                        static void onProgress(void *arg, Update::Stage stage, uint64_t done, uint64_t total);
                        void        start(Job job);
                        void        showNothing();
                        void        showReport();
                        void        install(bool force);

                        Dialog       _confirm;
                        Label       *_title = nullptr;
                        Label       *_versions = nullptr;
                        Label       *_made = nullptr;
                        Label       *_verdict = nullptr;
                        Label       *_notesLabel = nullptr;
                        Label       *_status = nullptr;
                        NotesView   *_notes = nullptr;
                        ProgressBar *_bar = nullptr;
                        Button      *_install = nullptr;
                        Button      *_close = nullptr;
                        // The worker's, read on the loop once it posted.
                        Thread                _worker;
                        Job                   _job = Job::Check;
                        bool                  _force = false;
                        bool                  _busy = false; // installing: not closed
                        String                _path;
                        String                _notesText;
                        Update::Report        _report;
                        Error                 _result;
                        std::atomic<uint32_t> _permille{0};
                        std::atomic<bool>     _progressPosted{false};
        };

        // The newest package (by when it was written) in update/dir into @p out; false if none.
        bool newestPackage(String &out) {
                String dir(MemSpace::Psram);
                if (SysConf::get(StringView("update/dir"), dir).isError() || dir.isEmpty()) return false;
                FolderModel folder;
                if (folder.open(dir.view(), Vfs::Context{"/", nullptr}).isError()) return false;
                while (!folder.isComplete()) (void)folder.fillSome(64);
                int     best = -1;
                int64_t bestTime = 0;
                for (size_t i = 0; i < folder.count(); ++i) {
                        const StringView name = folder.name(i);
                        if (folder.entry(i).isDirectory() || !name.endsWith(".bmup")) continue;
                        const int64_t t = folder.entry(i).modified.seconds();
                        if (best < 0 || t > bestTime) {
                                best = int(i);
                                bestTime = t;
                        }
                }
                if (best < 0) return false;
                Error err = out.assign(dir.view());
                if (err.isOk() && !dir.view().endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(folder.name(size_t(best)));
                return err.isOk();
        }

        Error UpdaterWindow::build(StringView path) {
                auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
                if (column == nullptr) return Error::NoMem;
                setCentral(column);
                column->setMargins(12, 12, 12, 12);
                column->setSpacing(6);
                _title = Widget::create<Label>(column);
                _versions = Widget::create<Label>(column);
                _made = Widget::create<Label>(column);
                _verdict = Widget::create<Label>(column);
                _notesLabel = Widget::create<Label>(column, "Release notes:");
                // The notes in a field's frame, so they read as their own pane.
                auto *notesFrame = Widget::create<BoxLayout>(column, BoxLayout::Direction::Vertical);
                if (notesFrame == nullptr) return Error::NoMem;
                notesFrame->setFrame(Bevel::Style::Field);
                notesFrame->setMargins(2, 2, 2, 2); // the bevel's
                notesFrame->setStretch(1);
                _notes = Widget::create<NotesView>(notesFrame);
                _status = Widget::create<Label>(column);
                _bar = Widget::create<ProgressBar>(column);
                auto *buttons = Widget::create<BoxLayout>(column, BoxLayout::Direction::Horizontal);
                if (_title == nullptr || _versions == nullptr || _made == nullptr || _verdict == nullptr ||
                    _notesLabel == nullptr || _notes == nullptr || _status == nullptr || _bar == nullptr ||
                    buttons == nullptr)
                        return Error::NoMem;
                _notes->setStretch(1);
                _bar->setMaximum(1000);
                _bar->setVisible(false);
                buttons->setSpacing(8);
                auto *gap = Widget::create<Label>(buttons);
                _install = Widget::create<Button>(buttons, "&Install");
                _close = Widget::create<Button>(buttons, "&Close");
                if (gap == nullptr || _install == nullptr || _close == nullptr) return Error::NoMem;
                gap->setStretch(1);
                _install->setClicked(clicked, this);
                _close->setClicked(clicked, this);
                _install->setEnabled(false);

                (void)Widget::create<Label>(_confirm.content(), "It isn't newer than what is installed. Install it?");
                _confirm.addButton("&Install", 1);
                _confirm.addButton("&Cancel", 0);
                _confirm.setFinished(confirmed, this);

                Error err = path.isEmpty() ? Error::Ok : _path.assign(path);
                if (err.isOk() && path.isEmpty() && !newestPackage(_path)) {
                        showNothing();
                        column->relayout();
                        return Error::Ok;
                }
                if (err.isError()) return err;
                say(_title, "Update package: %.*s", int(_path.size()), _path.view().data());
                say(_verdict, "Checking it...");
                column->relayout();
                start(Job::Check);
                return Error::Ok;
        }

        // No package: where they go, and how the last update went.
        void UpdaterWindow::showNothing() {
                String dir(MemSpace::Psram);
                (void)SysConf::get(StringView("update/dir"), dir);
                say(_title, "No update packages in %.*s", int(dir.size()), dir.view().data());
                say(_versions, "BMDS %s is installed", System::info().buildVersion);
                say(_verdict, "Copy a .bmup there, or open one from Files");
                _made->setVisible(false);
                _notesLabel->setText("The last update:");
                Update::Status status;
                String         text(MemSpace::Psram);
                if (Update::status(status).isOk() && !status.result.isEmpty()) {
                        (void)text.assign(status.result.view());
                } else {
                        (void)text.assign("No update has been installed on this system yet.");
                }
                if (!status.pending.isEmpty()) {
                        (void)text.append("\nWaiting to be installed by recovery: ");
                        (void)text.append(status.pending.view());
                }
                _notes->setText(text.view());
                return;
        }

        void UpdaterWindow::start(Job job) {
                _job = job;
                _worker.join(); // the last one's, ended
                Thread::Config config;
                config.name = "updater";
                config.stackBytes = WorkerStackBytes;
                config.stackSpace = MemSpace::Psram; // the update's flash work has its own internal stack
                if (_worker.start(config, workerMain, this).isError()) say(_verdict, "Out of memory");
                return;
        }

        // The worker: a check (and the notes read), or an install; then told to the loop.
        void UpdaterWindow::workerMain(void *arg) {
                auto *self = static_cast<UpdaterWindow *>(arg);
                if (self->_job == Job::Check) {
                        self->_result = Update::check(self->_path.view(), self->_report, nullptr, nullptr);
                        const UpdatePackage::Section *notes =
                                self->_report.package.find(UpdatePackage::SectionType::Notes);
                        // At most MaxNotesBytes shown, and said to be cut.
                        const uint32_t shown = notes == nullptr              ? 0
                                               : notes->size < MaxNotesBytes ? notes->size
                                                                             : uint32_t(MaxNotesBytes);
                        if (notes != nullptr && self->_notesText.reserve(shown + 64).isOk()) {
                                Vfs::Context context;
                                auto [file, err] = Vfs::open(self->_path.view(), Vfs::Read, context);
                                if (err.isOk() && file->stream().seek(notes->offset, Stream::SeekFrom::Start).isOk()) {
                                        char buf[512];
                                        for (uint32_t left = shown; left > 0;) {
                                                auto [n, rerr] = file->read(
                                                        WritableByteSpan(reinterpret_cast<uint8_t *>(buf),
                                                                         left < sizeof(buf) ? left : sizeof(buf)),
                                                        WaitForever);
                                                if (rerr.isError() || n == 0) break;
                                                (void)self->_notesText.append(StringView(buf, n));
                                                left -= uint32_t(n);
                                        }
                                }
                                if (shown < notes->size) {
                                        // Cut at the last whole line (never inside a character), and said so.
                                        const size_t nl = self->_notesText.view().lastIndexOf('\n');
                                        if (nl != StringView::NPos) self->_notesText.truncate(nl + 1);
                                        (void)self->_notesText.append(
                                                "\n(The rest isn't shown: the notes are over 64 KB.)\n");
                                }
                        }
                        (void)self->application().post(checked, self);
                } else {
                        self->_result =
                                Update::apply(self->_path.view(), self->_force, self->_report, onProgress, self);
                        (void)self->application().post(installed, self);
                }
                return;
        }

        void UpdaterWindow::checked(void *arg) {
                auto *self = static_cast<UpdaterWindow *>(arg);
                if (self->_result.isError() && self->_result != Error::CorruptData) {
                        say(self->_verdict, "Can't read it: %s", self->_result.desc());
                        return;
                }
                self->showReport();
                return;
        }

        void UpdaterWindow::showReport() {
                const Update::Report         &r = _report;
                const UpdatePackage::Package &p = r.package;
                if (p.versionText[0] != '\0') say(_title, "BMDS %s", p.versionText);
                say(_versions, "Installed: %s (this is %s)", r.mainVersion[0] != '\0' ? r.mainVersion : "nothing",
                    r.order > 0   ? "newer"
                    : r.order < 0 ? "older"
                                  : "the same version");
                const DateTime t = DateTime::fromSeconds(p.madeAt);
                say(_made, "Made %04ld-%02u-%02u, signed with %s", static_cast<long>(t.year), t.month, t.day,
                    r.problem == nullptr ? (r.devKey ? "the dev key" : "a release key") : "?");
                if (r.problem != nullptr) {
                        say(_verdict, "Can't be installed: %s", r.problem);
                } else {
                        say(_verdict, "Checked: it can be installed");
                }
                _install->setEnabled(r.problem == nullptr);
                _notes->setText(_notesText.isEmpty() ? StringView("(none)") : _notesText.view());
                return;
        }

        void UpdaterWindow::clicked(void *arg, Button &button) {
                auto *self = static_cast<UpdaterWindow *>(arg);
                if (&button == self->_close) {
                        if (!self->_busy) self->application().quit(0);
                        return;
                }
                if (self->_report.order <= 0) {
                        (void)self->_confirm.open();
                } else {
                        self->install(false);
                }
                return;
        }

        void UpdaterWindow::confirmed(void *arg, Dialog &, int result) {
                if (result == 1) static_cast<UpdaterWindow *>(arg)->install(true);
                return;
        }

        void UpdaterWindow::install(bool force) {
                _busy = true;
                _force = force;
                _install->setEnabled(false);
                _close->setEnabled(false);
                _bar->setValue(0);
                _bar->setVisible(true);
                say(_status, "Checking it again...");
                static_cast<BoxLayout *>(_bar->parent())->relayout();
                start(Job::Install);
                return;
        }

        // From the update's job: kept, and told to the loop at most once until it is drawn.
        void UpdaterWindow::onProgress(void *arg, Update::Stage stage, uint64_t done, uint64_t total) {
                auto *self = static_cast<UpdaterWindow *>(arg);
                // The stage in the top bits: the bar's text follows it.
                self->_permille.store((uint32_t(stage) << 16) | uint32_t(total != 0 ? done * 1000 / total : 0),
                                      std::memory_order_relaxed);
                if (!self->_progressPosted.exchange(true, std::memory_order_acq_rel)) {
                        if (self->application().post(progressed, self).isError())
                                self->_progressPosted.store(false, std::memory_order_release);
                }
                return;
        }

        void UpdaterWindow::progressed(void *arg) {
                auto *self = static_cast<UpdaterWindow *>(arg);
                self->_progressPosted.store(false, std::memory_order_release);
                const uint32_t v = self->_permille.load(std::memory_order_relaxed);
                say(self->_status, "%s...", Update::stageText(static_cast<Update::Stage>(v >> 16)));
                self->_bar->setValue(v & 0xffff);
                return;
        }

        void UpdaterWindow::installed(void *arg) {
                auto *self = static_cast<UpdaterWindow *>(arg);
                if (self->_result.isOk()) {
                        self->_bar->setValue(1000);
                        say(self->_status, "Restarting to install...");
                        (void)self->application().startTimer(RestartDelayMs, false, restart, self);
                        return;
                }
                self->_busy = false;
                self->_close->setEnabled(true);
                self->_bar->setVisible(false);
                if (self->_result == Error::Exists) {
                        say(self->_status, "Everything in it is installed already");
                } else {
                        say(self->_status, "Not installed: %s",
                            self->_report.problem != nullptr ? self->_report.problem : self->_result.desc());
                        self->_install->setEnabled(self->_report.problem == nullptr);
                }
                static_cast<BoxLayout *>(self->_bar->parent())->relayout();
                return;
        }

        void UpdaterWindow::restart(void *) {
                (void)Vfs::sync();
                System::restart();
        }

        int updaterMain(const BuiltinProgram::Context &context) {
                FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                const auto  complain = [err](const char *text) {
                        if (err != nullptr)
                                (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                  WaitForever);
                };
                if (context.args.size() > 2) {
                        complain("usage: updater [FILE]\n");
                        return 2;
                }
                String path(MemSpace::Psram);
                if (context.args.size() == 2) {
                        auto [full, nerr] =
                                Vfs::normalize(context.args[1], context.cwd.isEmpty() ? StringView("/") : context.cwd);
                        if (nerr.isError()) {
                                complain("updater: not a path\n");
                                return 2;
                        }
                        path = std::move(full);
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                int   status = 1;
                if (app->connect(0).isError()) {
                        complain("updater: the window system isn't running ('win start')\n");
                } else {
                        GuiWindow::Config config;
                        config.rect = Rect{200, 120, 640, 420};
                        config.title = "Updater";
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(UpdaterWindow));
                        if (wmem != nullptr) {
                                auto *window = ::new (wmem) UpdaterWindow(*app, config);
                                Error e;
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        e = window->build(path.view());
                                        if (e.isOk()) e = window->show();
                                }
                                if (e.isOk()) {
                                        status = app->exec();
                                } else {
                                        complain("updater: can't open its window\n");
                                }
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        window->~UpdaterWindow();
                                }
                                Memory::free(wmem);
                        }
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM(updater, updaterMain, 12 * 1024)

BMDS_NAMESPACE_END
