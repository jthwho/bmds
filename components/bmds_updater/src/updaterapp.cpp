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
 * update/dir, or the last update's result. A package with a new bootloader asks first whether to
 * write it (devplan/update.md §20). An ordinary GUI program on the toolkit; checking and
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
#include <bmds/formlayout.h>
#include <bmds/bitmapfont.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/lineeditmodel.h>
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
        constexpr uint16_t ColumnMargin = 12; // the window's column, each side

        // Release notes: Markdown shown as text, its soft breaks joined, wrapped to the width at word ends
        // (a list item's lines hanging under its text), scrolled.
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
                                if (_text.assign(text).isOk())
                                        _text.truncate(TextWrap::joinSoftBreaks(_text.view(), _text.data()));
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
                                        const int   lx = x + int(l.indent) * Fonts::unscii16.width();
                                        if (lx >= viewport.right()) continue;
                                        (void)painter.text(lx, y, StringView(_text.view().data() + l.at, l.bytes),
                                                           Fonts::unscii16, fg, bg, uint16_t(viewport.right() - lx));
                                }
                                return;
                        }

                private:
                        struct Line {
                                        uint32_t at = 0;
                                        uint16_t bytes = 0;
                                        uint8_t  indent = 0; // columns: a wrapped line hangs under its text
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

                        // Lines of at most @p viewportWidth's columns, broken after a space where there is one;
                        // a paragraph's lines after its first hang at its text's column (at most half the width).
                        void wrapTo(uint16_t viewportWidth) {
                                _lines.clear();
                                const uint16_t   width = viewportWidth > 2 * NotesPad + 160
                                                                 ? uint16_t(viewportWidth - 2 * NotesPad)
                                                                 : 160;
                                const size_t     columns = width / Fonts::unscii16.width();
                                const StringView t = _text.view();
                                size_t           start = 0;
                                size_t           hang = 0; // this paragraph's, for its lines after the first
                                bool             first = true;
                                while (start < t.size()) {
                                        if (first) {
                                                const size_t nl = t.indexOf('\n', start);
                                                hang = TextWrap::hangingIndent(t.mid(
                                                        start, nl == StringView::NPos ? StringView::NPos : nl - start));
                                                if (hang > columns / 2) hang = 0;
                                        }
                                        const size_t indent = first ? 0 : hang;
                                        size_t i = start, chars = 0, lastSpace = StringView::NPos;
                                        while (i < t.size() && t[i] != '\n' && chars < columns - indent) {
                                                if (t[i] == ' ') lastSpace = i;
                                                i += 1;
                                                while (i < t.size() && (uint8_t(t[i]) & 0xc0) == 0x80)
                                                        ++i; // a character's rest
                                                ++chars;
                                        }
                                        size_t end = i, next = i;
                                        first = false;
                                        if (i < t.size() && t[i] == '\n') {
                                                next = i + 1;
                                                first = true;
                                        } else if (i < t.size() && lastSpace != StringView::NPos) {
                                                end = lastSpace;
                                                next = lastSpace + 1;
                                        }
                                        (void)_lines.append(
                                                Line{uint32_t(start), uint16_t(end - start), uint8_t(indent)});
                                        start = next;
                                }
                                setContentSize(viewportWidth, uint32_t(_lines.size()) * CellHeight + 2 * NotesPad);
                                return;
                        }

                        String     _text;
                        List<Line> _lines;
        };

        // A line of the window's column (a verdict, a status) as a Label shows it, but wrapped below
        // when it is longer than the column is wide or a Label holds (63 bytes): a package's problem
        // ("Can't be installed: it isn't signed by a key this firmware accepts") is shown whole.
        class MessageLine final : public Widget {
                public:
                        static constexpr size_t MaxText = 255;
                        static constexpr size_t MaxLines = 4;

                        explicit MessageLine(Widget *parent) : Widget(parent) {}

                        // Sets the text; the column lays out again when its lines changed.
                        void setText(StringView text) {
                                const uint16_t before = sizeHint().height;
                                size_t         n = text.size() < MaxText ? text.size() : MaxText;
                                while (n > 0 && n < text.size() && (uint8_t(text[n]) & 0xc0) == 0x80) --n;
                                std::memcpy(_text, text.data(), n);
                                _text[n] = '\0';
                                if (sizeHint().height != before && parent() != nullptr)
                                        static_cast<BoxLayout *>(parent())->relayout();
                                update();
                                return;
                        }

                        // A Label's height, a line more for each wrapped one: measured at the column's
                        // width, which its layout has before it asks.
                        GuiSize sizeHint() const override {
                                StringView     lines[MaxLines];
                                const size_t   n = wrapped(lines);
                                const uint16_t more = n > 1 ? uint16_t((n - 1) * textHeight()) : 0;
                                return GuiSize{0, uint16_t(buttonHeight() + more)};
                        }

                        void paint(Painter &painter) override {
                                const Rect    &r = geometry();
                                const uint32_t bg = painter.palette().color(WindowPalette::Role::Face);
                                const uint32_t fg =
                                        painter.palette().color(isEnabled() ? WindowPalette::Role::FaceText
                                                                            : WindowPalette::Role::DisabledText);
                                (void)painter.fill(r, bg);
                                const uint16_t x = textX();
                                if (x >= r.right()) return;
                                StringView   lines[MaxLines];
                                const size_t n = wrapped(lines);
                                // The first where a Label's text is (centred in a button's height).
                                uint32_t y = GuiStyle::textTop(Rect{r.x, r.y, r.width, buttonHeight()}, font());
                                for (size_t i = 0; i < n && y + textHeight() <= r.bottom(); ++i, y += textHeight())
                                        (void)painter.text(x, int(y), lines[i], font(), fg, bg,
                                                           static_cast<uint16_t>(r.right() - x));
                                return;
                        }

                private:
                        // Where a Label starts its text: past the pad, on a text column.
                        uint16_t textX() const {
                                return uint16_t((geometry().x + GuiStyle::ButtonPad) & ~uint32_t(GuiStyle::Column - 1));
                        }

                        // The lines at the column's width (inside its margins), less the pad.
                        size_t wrapped(StringView (&lines)[MaxLines]) const {
                                if (_text[0] == '\0') return 0;
                                const Widget  *column = parent();
                                const uint32_t outer = column != nullptr && column->geometry().width > 2 * ColumnMargin
                                                               ? column->geometry().width - 2 * ColumnMargin
                                                               : geometry().width;
                                const uint32_t room =
                                        outer > 2 * GuiStyle::ButtonPad ? outer - 2 * GuiStyle::ButtonPad : 0;
                                if (room == 0) {
                                        lines[0] = StringView(_text);
                                        return 1;
                                }
                                return GuiStyle::wrap(StringView(_text), font(), room, Span<StringView>(lines));
                        }

                        char _text[MaxText + 1] = {};
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

        void say(MessageLine *line, const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

        void say(MessageLine *line, const char *fmt, ...) {
                char    text[MessageLine::MaxText + 1];
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(text, sizeof(text), fmt, args);
                va_end(args);
                line->setText(StringView(text));
                return;
        }

        class UpdaterWindow final : public GuiWindow {
                public:
                        UpdaterWindow(GuiApplication &app, const Config &config)
                            : GuiWindow(app, config), _confirm(app, "Install", 440, 48),
                              _bootConfirm(app, "Install the Bootloader?", 440, 0), _path(MemSpace::Psram),
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
                        static void bootConfirmed(void *arg, Dialog &dialog, int result);
                        static void onProgress(void *arg, Update::Stage stage, uint64_t done, uint64_t total);
                        void        start(Job job);
                        void        showNothing();
                        void        showReport();
                        void        askBootloader(bool force);
                        void        install(bool force, bool bootloader);
                        void        showOnlyBootloaderLeft();
                        bool        onlyBootloaderNew() const;

                        Dialog       _confirm;
                        Dialog       _bootConfirm; // a package with a new bootloader (devplan/update.md §20)
                        Label       *_title = nullptr;
                        Label       *_versions = nullptr;
                        Label       *_made = nullptr;
                        MessageLine *_verdict = nullptr;
                        Label       *_notesLabel = nullptr;
                        MessageLine *_status = nullptr;
                        NotesView   *_notes = nullptr;
                        ProgressBar *_bar = nullptr;
                        Button      *_install = nullptr;
                        Button      *_close = nullptr;
                        // The worker's, read on the loop once it posted.
                        Thread                _worker;
                        Job                   _job = Job::Check;
                        bool                  _force = false;
                        bool                  _bootloader = false; // the user said yes to writing it
                        bool                  _withoutAsked = false; // "not newer?" asked after Install Without
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
                column->setMargins(ColumnMargin, ColumnMargin, ColumnMargin, ColumnMargin);
                column->setSpacing(6);
                _title = Widget::create<Label>(column);
                _versions = Widget::create<Label>(column);
                _made = Widget::create<Label>(column);
                _verdict = Widget::create<MessageLine>(column);
                _notesLabel = Widget::create<Label>(column, "Release notes:");
                // The notes in a field's frame, so they read as their own pane.
                auto *notesFrame = Widget::create<BoxLayout>(column, BoxLayout::Direction::Vertical);
                if (notesFrame == nullptr) return Error::NoMem;
                notesFrame->setFrame(Bevel::Style::Field);
                notesFrame->setMargins(2, 2, 2, 2); // the bevel's
                notesFrame->setStretch(1);
                _notes = Widget::create<NotesView>(notesFrame);
                _status = Widget::create<MessageLine>(column);
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

                // Asked only for a package whose bootloader differs: the one write a power cut can't be
                // recovered from without a cable.
                // One block wrapped at the dialog's width (a Label is cut at 63 bytes), stretching to the
                // height the dialog grows to for it.
                auto *warning = Widget::create<TextBlock>(
                        _bootConfirm.content(),
                        "This update also brings a new bootloader, which starts the system. It is written last, in "
                        "under a second. Keep the board powered until the system is back: if the power is cut "
                        "while it is written, the board must be installed again over USB (the web installer).\n\n"
                        "Without it, the update installs and the bootloader stays as it is.");
                if (warning != nullptr) warning->setStretch(1);
                _bootConfirm.addButton("Install &with It", 2);
                _bootConfirm.addButton("Install With&out", 1);
                _bootConfirm.addButton("&Cancel", 0);
                _bootConfirm.setFinished(bootConfirmed, this);

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
                        self->_result = Update::apply(self->_path.view(), self->_force, self->_bootloader,
                                                      self->_report, onProgress, self);
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
                    r.problem == nullptr ? (r.devKey ? "the dev key" : "a trusted key") : "?");
                if (r.problem != nullptr) {
                        say(_verdict, "Can't be installed: %s", r.problem);
                } else if (r.hasNewBootloader()) {
                        say(_verdict, "Checked: it can be installed, with a new bootloader");
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
                // The same version whose news is its bootloader needs no "not newer" question.
                const Update::Report &r = self->_report;
                if (r.order < 0 || (r.order == 0 && !r.hasNewBootloader())) {
                        (void)self->_confirm.open();
                } else {
                        self->askBootloader(false);
                }
                return;
        }

        void UpdaterWindow::confirmed(void *arg, Dialog &, int result) {
                auto      *self = static_cast<UpdaterWindow *>(arg);
                const bool without = self->_withoutAsked;
                self->_withoutAsked = false;
                if (result != 1) return;
                if (without) {
                        self->install(true, false); // the bootloader was declined already
                } else {
                        self->askBootloader(true);
                }
                return;
        }

        // Installs at once, or first asks about a new bootloader.
        void UpdaterWindow::askBootloader(bool force) {
                _force = force;
                if (_report.hasNewBootloader()) {
                        (void)_bootConfirm.open();
                } else {
                        install(force, false);
                }
                return;
        }

        void UpdaterWindow::bootConfirmed(void *arg, Dialog &, int result) {
                auto *self = static_cast<UpdaterWindow *>(arg);
                if (result == 0) return;
                // Without, at the version installed (asked here with no "not newer?" first, the bootloader
                // being the news): nothing to do when that was all, a reinstall asked as one when another
                // part differs (another build of the version). Update::apply() would refuse it either way.
                if (result == 1 && !self->_force && self->_report.order == 0) {
                        if (self->onlyBootloaderNew()) {
                                self->showOnlyBootloaderLeft();
                        } else {
                                self->_withoutAsked = true;
                                (void)self->_confirm.open();
                        }
                        return;
                }
                self->install(self->_force, result == 2);
                return;
        }

        // True when the bootloader is the only part that differs from what is flashed.
        bool UpdaterWindow::onlyBootloaderNew() const {
                const UpdatePackage::Package &p = _report.package;
                for (size_t i = 0; i < p.sectionCount; ++i) {
                        const auto type = UpdatePackage::SectionType(p.sections[i].type);
                        if ((type == UpdatePackage::SectionType::Firmware || type == UpdatePackage::SectionType::Rom ||
                             type == UpdatePackage::SectionType::Recovery) &&
                            _report.flashed[i] == Update::Flashed::Differs)
                                return false;
                }
                return true;
        }

        void UpdaterWindow::showOnlyBootloaderLeft() {
                say(_status, "Everything else in it is installed already; not its bootloader");
                _install->setEnabled(true); // Install with It stays a choice
                static_cast<BoxLayout *>(_bar->parent())->relayout();
                return;
        }

        void UpdaterWindow::install(bool force, bool bootloader) {
                _busy = true;
                _force = force;
                _bootloader = bootloader;
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
                        say(self->_status, self->_report.hasNewBootloader()
                                                   ? "Everything else in it is installed already; not its bootloader"
                                                   : "Everything in it is installed already");
                        self->_install->setEnabled(self->_report.hasNewBootloader());
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
