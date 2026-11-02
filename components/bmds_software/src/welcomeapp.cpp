/**
 * @file      welcomeapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/welcome [--first-run] (devplan/packages.md §24): the first run's window. With nothing
 * installed on the card, it offers the standard set, a choice of packages, or not now; it finds
 * them through the configured sources, or the card's packages folder when those fail. An
 * ordinary GUI program on the toolkit, driving `pkg --json` (PkgJob).
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/fdtable.h>
#include <bmds/formlayout.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/scrollview.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>
#include <bmds/widget.h>

#include "pkgjob.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The set offered first.
        constexpr StringView StandardSet("bmds-standard");

        // "1.2 MB", "850 KB", "512 bytes".
        void sizeText(char (&out)[24], uint64_t bytes) {
                if (bytes >= 1024 * 1024) {
                        const uint64_t tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
                        std::snprintf(out, sizeof(out), "%llu.%llu MB", (unsigned long long)(tenths / 10),
                                      (unsigned long long)(tenths % 10));
                } else if (bytes >= 1024) {
                        std::snprintf(out, sizeof(out), "%llu KB", (unsigned long long)((bytes + 512) / 1024));
                } else {
                        std::snprintf(out, sizeof(out), "%llu bytes", (unsigned long long)bytes);
                }
                return;
        }

        void appendf(String &s, const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);

        void appendf(String &s, const char *fmt, ...) {
                char    text[512];
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(text, sizeof(text), fmt, args);
                va_end(args);
                (void)s.append(StringView(text));
                return;
        }

        // Whether ", "-separated @p list names @p name.
        bool listNames(StringView list, StringView name) {
                while (!list.isEmpty()) {
                        size_t     comma = list.indexOf(',');
                        StringView item = list.left(comma);
                        list = comma == StringView::NPos ? StringView() : list.mid(comma + 1);
                        item = item.trimmed();
                        const size_t space = item.indexOf(' '); // "lib (>= 2)"
                        if (item.left(space) == name) return true;
                }
                return false;
        }

        // A paragraph in a column: as many lines as its text takes at the width it was given (the
        // first layout guesses the width; the second knows it).
        class WrapText final : public TextBlock {
                public:
                        explicit WrapText(Widget *parent, StringView text = StringView()) : TextBlock(parent, text) {}

                        GuiSize sizeHint() const override {
                                const uint16_t width = geometry().width > 0 ? geometry().width : GuessedWidth;
                                return GuiSize{16, heightForWidth(width)};
                        }

                private:
                        static constexpr uint16_t GuessedWidth = 540;
        };

        // A package the lists offer.
        struct Offer {
                        String   name;
                        String   title;
                        String   summary;
                        String   depends;
                        uint64_t size = 0;
                        uint64_t installedSize = 0;
                        bool     installed = false;
                        bool     isSet = false; // a meta package: what it needs is the set
        };

        class WelcomeWindow final : public GuiWindow {
                public:
                        WelcomeWindow(GuiApplication &app, const Config &config, const BuiltinProgram::Context &context,
                                      bool firstRun)
                            : GuiWindow(app, config), _context(context), _job(app), _offers(MemSpace::Psram),
                              _rows(MemSpace::Psram), _folder(MemSpace::Psram), _sourceNotes(MemSpace::Psram),
                              _failure(MemSpace::Psram), _firstRun(firstRun) {
                                _job.setHandlers(onRecord, onFinished, this);
                        }

                        ~WelcomeWindow() override { application().cancelPosts(this); }

                        Error build();

                protected:
                        void closedByManager() override {
                                if (!_job.isRunning()) application().quit(0);
                                return;
                        }

                private:
                        // What the running job is for.
                        enum class Stage : uint8_t {
                                Installed, // --first-run: is anything installed?
                                Update,    // the sources' lists
                                List,      // what they offer
                                DryRun,    // what the standard set takes
                                Install,   // installing
                        };

                        struct Row {
                                        CheckBox *box;
                                        size_t    offer;
                        };

                        static void onRecord(void *arg, const PkgRecord &record);
                        static void onFinished(void *arg, int status);
                        static void clicked(void *arg, Button &button);
                        static void toggled(void *arg, CheckBox &box);
                        static void showToggled(void *arg, CheckBox &box);

                        void         run(Stage stage, StringView args);
                        void         lookAgainInFolder();
                        void         showStart();
                        void         showChoice();
                        void         showFailure(StringView what);
                        void         installNames(StringView names);
                        void         updateChosen();
                        void         setButtons(bool start, bool choice, bool close);
                        void         relayout();
                        void         say(TextBlock *block, const char *fmt, ...) BMDS_PRINTF_FORMAT(3, 4);
                        const Offer *find(StringView name) const;

                        const BuiltinProgram::Context &_context;
                        PkgJob                         _job;
                        Stage                          _stage = Stage::Update;
                        List<Offer>                    _offers;
                        List<Row>                      _rows;
                        String                         _folder;      // the card's packages folder, when used
                        String                         _sourceNotes; // sources that failed
                        String                         _failure;     // the last error record's text
                        bool                           _firstRun;
                        bool                           _anyInstalled = false;
                        bool                           _usingFolder = false;
                        bool                           _standardFound = false;
                        uint32_t                       _planned = 0; // packages the dry run or install plans
                        uint32_t                       _done = 0;
                        uint64_t                       _planBytes = 0;
                        uint64_t                       _planFetch = 0;
                        BoxLayout                     *_column = nullptr;
                        BoxLayout                     *_buttons = nullptr;
                        Label                         *_heading = nullptr;
                        TextBlock                     *_intro = nullptr;
                        TextBlock                     *_status = nullptr;
                        ScrollView                    *_list = nullptr;
                        ProgressBar                   *_bar = nullptr;
                        CheckBox                      *_showAtStart = nullptr;
                        Button                        *_installSet = nullptr;
                        Button                        *_choose = nullptr;
                        Button                        *_notNow = nullptr;
                        Button                        *_installChosen = nullptr;
                        Button                        *_back = nullptr;
                        Button                        *_close = nullptr;
        };

        void WelcomeWindow::say(TextBlock *block, const char *fmt, ...) {
                char    text[TextBlock::MaxText + 1];
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(text, sizeof(text), fmt, args);
                va_end(args);
                block->setText(StringView(text));
                return;
        }

        Error WelcomeWindow::build() {
                _column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
                if (_column == nullptr) return Error::NoMem;
                setCentral(_column);
                _column->setMargins(12, 12, 12, 12);
                _column->setSpacing(8);
                _heading = Widget::create<Label>(_column, "Welcome to BMDS");
                _intro = Widget::create<WrapText>(_column, "Nothing is installed on the card yet.");
                _list = Widget::create<ScrollView>(_column);
                _status = Widget::create<WrapText>(_column, "Looking for packages...");
                _bar = Widget::create<ProgressBar>(_column);
                _showAtStart = Widget::create<CheckBox>(_column, "&Show this when BMDS starts");
                _buttons = Widget::create<BoxLayout>(_column, BoxLayout::Direction::Horizontal);
                BoxLayout *buttons = _buttons;
                if (_heading == nullptr || _intro == nullptr || _list == nullptr || _status == nullptr ||
                    _bar == nullptr || _showAtStart == nullptr || buttons == nullptr)
                        return Error::NoMem;
                _list->setStretch(1);
                _list->setSpacing(6);
                _list->setVisible(false);
                _bar->setVisible(false);
                const Result<bool> show = SysConf::getBool(StringView("pkg/welcome"));
                _showAtStart->setChecked(!show.isOk() || show.value());
                _showAtStart->setToggled(showToggled, this);
                buttons->setSpacing(8);
                auto *gap = Widget::create<Label>(buttons);
                _installSet = Widget::create<Button>(buttons, "&Install the standard set");
                _choose = Widget::create<Button>(buttons, "&Choose packages...");
                _notNow = Widget::create<Button>(buttons, "&Not now");
                _installChosen = Widget::create<Button>(buttons, "&Install");
                _back = Widget::create<Button>(buttons, "&Back");
                _close = Widget::create<Button>(buttons, "&Close");
                if (gap == nullptr || _installSet == nullptr || _choose == nullptr || _notNow == nullptr ||
                    _installChosen == nullptr || _back == nullptr || _close == nullptr)
                        return Error::NoMem;
                gap->setStretch(1);
                for (Button *b : {_installSet, _choose, _notNow, _installChosen, _back, _close})
                        b->setClicked(clicked, this);
                setButtons(true, false, false);
                _installSet->setEnabled(false);
                _choose->setEnabled(false);
                setDefaultButton(_installSet);
                relayout();
                // A first run asks first whether anything is installed (the window shows only if not).
                run(_firstRun ? Stage::Installed : Stage::Update, _firstRun ? "list --installed" : "update");
                return Error::Ok;
        }

        // Twice: the paragraphs know their width after the first. The buttons' row too, whose place
        // doesn't change when buttons are shown or hidden.
        void WelcomeWindow::relayout() {
                _column->relayout();
                _column->relayout();
                _buttons->relayout();
                if (_list->isVisible()) {
                        _list->relayout();
                        _list->relayout();
                }
                return;
        }

        void WelcomeWindow::setButtons(bool start, bool choice, bool close) {
                _installSet->setVisible(start);
                _choose->setVisible(start);
                _notNow->setVisible(start);
                _installChosen->setVisible(choice);
                _back->setVisible(choice);
                _close->setVisible(close);
                return;
        }

        // Runs `pkg --json ARGS`, with the card's folder as a source once it is used.
        void WelcomeWindow::run(Stage stage, StringView args) {
                _stage = stage;
                String line(MemSpace::Psram);
                if (_usingFolder) appendf(line, "--source 'file:%.*s' ", int(_folder.size()), _folder.view().data());
                (void)line.append(args);
                const Error err = _job.start(_context, line.view());
                if (err.isError()) {
                        if (_stage == Stage::Installed) {
                                application().quit(0);
                                return;
                        }
                        String why(MemSpace::Psram);
                        appendf(why, "pkg couldn't be started: %s", err.desc());
                        showFailure(why.view());
                }
                return;
        }

        const Offer *WelcomeWindow::find(StringView name) const {
                for (const Offer &o : _offers)
                        if (o.name.view() == name) return &o;
                return nullptr;
        }

        void WelcomeWindow::onRecord(void *arg, const PkgRecord &r) {
                auto *self = static_cast<WelcomeWindow *>(arg);
                switch (self->_stage) {
                        case Stage::Installed:
                                if (r.type == PkgRecord::Type::Package && r.installed) self->_anyInstalled = true;
                                break;
                        case Stage::Update:
                                if (r.type == PkgRecord::Type::Source && !r.error.isEmpty()) {
                                        if (!self->_sourceNotes.isEmpty()) (void)self->_sourceNotes.append('\n');
                                        (void)self->_sourceNotes.append(r.text.view());
                                }
                                break;
                        case Stage::List:
                                if (r.type == PkgRecord::Type::Package) {
                                        Offer o;
                                        (void)o.name.assign(r.name.view());
                                        (void)o.title.assign(r.title.isEmpty() ? r.name.view() : r.title.view());
                                        (void)o.summary.assign(r.summary.view());
                                        (void)o.depends.assign(r.depends.view());
                                        o.size = r.size;
                                        o.installedSize = r.installedSize;
                                        o.installed = r.installed;
                                        o.isSet = !r.depends.isEmpty() && r.installedSize == 0;
                                        if (r.name.view() == StandardSet) self->_standardFound = true;
                                        (void)self->_offers.append(std::move(o));
                                }
                                break;
                        case Stage::DryRun:
                        case Stage::Install:
                                if (r.type == PkgRecord::Type::Plan) {
                                        ++self->_planned;
                                        self->_planBytes += r.installedSize;
                                        self->_planFetch += r.fetch;
                                } else if (r.type == PkgRecord::Type::Error) {
                                        (void)self->_failure.assign(r.text.view());
                                } else if (self->_stage == Stage::Install && r.type == PkgRecord::Type::Done) {
                                        ++self->_done;
                                        self->_bar->setMaximum(self->_planned > 0 ? self->_planned : 1);
                                        self->_bar->setValue(self->_done);
                                } else if (self->_stage == Stage::Install && r.type == PkgRecord::Type::Progress) {
                                        const uint32_t n =
                                                self->_done + 1 < self->_planned ? self->_done + 1 : self->_planned;
                                        self->say(self->_status, "Installing %.*s (%u of %u)...", int(r.name.size()),
                                                  r.name.view().data(), unsigned(n), unsigned(self->_planned));
                                }
                                break;
                }
                return;
        }

        void WelcomeWindow::onFinished(void *arg, int status) {
                auto *self = static_cast<WelcomeWindow *>(arg);
                switch (self->_stage) {
                        case Stage::Installed:
                                // A card with packages isn't new: no window.
                                if (self->_anyInstalled) {
                                        self->application().quit(0);
                                        return;
                                }
                                (void)self->show();
                                self->run(Stage::Update, "update");
                                return;
                        case Stage::Update:
                                self->_offers.clear();
                                self->_standardFound = false;
                                self->run(Stage::List, "list");
                                return;
                        case Stage::List:
                                if (!self->_standardFound) {
                                        self->lookAgainInFolder();
                                        return;
                                }
                                self->_planned = 0;
                                self->_planBytes = self->_planFetch = 0;
                                self->_failure.clear();
                                self->run(Stage::DryRun, "install --dry-run bmds-standard");
                                return;
                        case Stage::DryRun:
                                if (status != 0) {
                                        self->showFailure(self->_failure.isEmpty() ? StringView("pkg couldn't plan it")
                                                                                   : self->_failure.view());
                                        return;
                                }
                                self->showStart();
                                return;
                        case Stage::Install:
                                self->_bar->setVisible(false);
                                if (status == 0) {
                                        char size[24];
                                        sizeText(size, self->_planBytes);
                                        self->say(self->_intro, "Installed: %u package%s, %s.", unsigned(self->_done),
                                                  self->_done == 1 ? "" : "s", size);
                                        self->say(self->_status, "The new programs are in the launcher.");
                                        self->_list->setVisible(false);
                                        self->_showAtStart->setVisible(false);
                                        self->setButtons(false, false, true);
                                        self->setDefaultButton(self->_close);
                                } else {
                                        self->showFailure(self->_failure.isEmpty() ? StringView("pkg failed")
                                                                                   : self->_failure.view());
                                }
                                self->relayout();
                                return;
                }
        }

        // The standard set wasn't in the configured sources: the card's packages folder, if it
        // has one that isn't one of them already.
        void WelcomeWindow::lookAgainInFolder() {
                String root(MemSpace::Psram), sources(MemSpace::Psram);
                (void)SysConf::get(StringView("pkg/root"), root);
                (void)SysConf::get(StringView("pkg/sources"), sources);
                String folder(MemSpace::Psram);
                appendf(folder, "%.*s%spackages", int(root.size()), root.view().data(),
                        root.view().endsWith('/') ? "" : "/");
                String asSource(MemSpace::Psram);
                appendf(asSource, "file:%.*s", int(folder.size()), folder.view().data());
                bool listed = false;
                for (StringView rest = sources.view(); !rest.isEmpty();) {
                        const size_t semi = rest.indexOf(';');
                        StringView   item = rest.left(semi).trimmed();
                        rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
                        const size_t space = item.indexOf(' '); // "file:/x signed-by=..."
                        if (item.left(space) == asSource.view()) listed = true;
                }
                auto [stat, serr] = Vfs::stat(folder.view(), Vfs::Context{"/", nullptr});
                if (!_usingFolder && !listed && serr.isOk() && stat.isDirectory()) {
                        _usingFolder = true;
                        _folder = std::move(folder);
                        run(Stage::Update, "update");
                        return;
                }
                String why(MemSpace::Psram);
                appendf(why, "The standard set wasn't found in the sources (pkg/sources: %.*s)%s%.*s.",
                        int(sources.size()), sources.view().data(), listed || _usingFolder ? "" : " or in ",
                        listed || _usingFolder ? 0 : int(folder.size()), folder.view().data());
                showFailure(why.view());
                return;
        }

        void WelcomeWindow::showStart() {
                const Offer *set = find(StandardSet);
                char         installed[24], fetch[24];
                sizeText(installed, _planBytes);
                sizeText(fetch, _planFetch);
                bool anyInstalled = false;
                for (const Offer &o : _offers) anyInstalled = anyInstalled || o.installed;
                String text(MemSpace::Psram);
                (void)text.append(anyInstalled ? "Some packages are installed already. "
                                               : "Nothing is installed on the card yet. ");
                if (set != nullptr && !set->summary.isEmpty()) {
                        (void)text.append(set->summary.view());
                        (void)text.append(". ");
                }
                if (_planned == 0) {
                        (void)text.append("It is installed already.");
                } else {
                        appendf(text, "%s %u package%s, taking %s on the card", anyInstalled ? "It adds" : "It is",
                                unsigned(_planned), _planned == 1 ? "" : "s", installed);
                        if (_planFetch > 0) appendf(text, " (%s to download)", fetch);
                        (void)text.append('.');
                }
                _intro->setText(text.view());
                String where(MemSpace::Psram);
                if (_usingFolder) {
                        appendf(where, "From the card's folder %.*s", int(_folder.size()), _folder.view().data());
                        if (!_sourceNotes.isEmpty()) (void)where.append(": the sources didn't answer.");
                } else {
                        (void)where.append("From the configured sources (pkg/sources).");
                }
                _status->setText(where.view());
                _installSet->setEnabled(_planned > 0);
                _choose->setEnabled(true);
                _list->setVisible(false);
                setButtons(true, false, false);
                setDefaultButton(_planned > 0 ? _installSet : _choose);
                relayout();
                return;
        }

        void WelcomeWindow::showFailure(StringView what) {
                _status->setText(what);
                _bar->setVisible(false);
                _list->setVisible(false);
                _showAtStart->setVisible(true);
                setButtons(true, false, false);
                _installSet->setEnabled(_standardFound && _planned > 0);
                _choose->setEnabled(!_offers.isEmpty());
                setDefaultButton(_notNow);
                relayout();
                return;
        }

        // Every package not installed, sets first, a box each; the standard set's members ticked.
        void WelcomeWindow::showChoice() {
                if (_rows.isEmpty()) {
                        const Offer *set = find(StandardSet);
                        for (int pass = 0; pass < 2; ++pass) {
                                for (size_t i = 0; i < _offers.size(); ++i) {
                                        const Offer &o = _offers[i];
                                        if (o.installed || o.isSet != (pass == 0)) continue;
                                        auto *row = Widget::create<BoxLayout>(_list, BoxLayout::Direction::Vertical);
                                        if (row == nullptr) break;
                                        // A set: what it lists (their own needs come on top).
                                        uint64_t bytes = o.installedSize;
                                        if (o.isSet) {
                                                bytes = 0;
                                                for (const Offer &m : _offers)
                                                        if (listNames(o.depends.view(), m.name.view()))
                                                                bytes += m.installedSize;
                                        }
                                        char size[24];
                                        sizeText(size, bytes);
                                        char text[CheckBox::MaxText + 1];
                                        std::snprintf(text, sizeof(text), "%.*s, %s", int(o.title.size()),
                                                      o.title.view().data(), size);
                                        auto *box = Widget::create<CheckBox>(row, StringView(text));
                                        auto *summary = Widget::create<WrapText>(row, o.summary.view());
                                        if (box == nullptr || summary == nullptr) break;
                                        summary->setColor(WindowPalette::Role::DisabledText);
                                        box->setChecked(set != nullptr &&
                                                        listNames(set->depends.view(), o.name.view()));
                                        box->setToggled(toggled, this);
                                        (void)_rows.append(Row{box, i});
                                }
                        }
                }
                _intro->setText("Tick the packages to install. Sets bring in what they list.");
                _list->setVisible(true);
                _list->relayout();
                setButtons(false, true, false);
                setDefaultButton(_installChosen);
                updateChosen();
                relayout();
                return;
        }

        void WelcomeWindow::updateChosen() {
                uint32_t n = 0;
                uint64_t bytes = 0;
                for (const Row &r : _rows) {
                        if (!r.box->isChecked()) continue;
                        ++n;
                        bytes += _offers[r.offer].installedSize;
                }
                char size[24];
                sizeText(size, bytes);
                say(_status, "%u chosen, %s (and what they need)", unsigned(n), size);
                _installChosen->setEnabled(n > 0);
                return;
        }

        void WelcomeWindow::installNames(StringView names) {
                _planned = 0;
                _done = 0;
                _planBytes = _planFetch = 0;
                _failure.clear();
                setButtons(false, false, false);
                _showAtStart->setVisible(false);
                _list->setVisible(false);
                _bar->setMaximum(1);
                _bar->setValue(0);
                _bar->setVisible(true);
                _status->setText("Installing...");
                relayout();
                String args(MemSpace::Psram);
                appendf(args, "install -y %.*s", int(names.size()), names.data());
                run(Stage::Install, args.view());
                return;
        }

        void WelcomeWindow::clicked(void *arg, Button &button) {
                auto *self = static_cast<WelcomeWindow *>(arg);
                if (self->_job.isRunning()) return;
                if (&button == self->_notNow || &button == self->_close) {
                        self->application().quit(0);
                } else if (&button == self->_installSet) {
                        self->installNames(StandardSet);
                } else if (&button == self->_choose) {
                        self->showChoice();
                } else if (&button == self->_back) {
                        self->showStart();
                } else if (&button == self->_installChosen) {
                        // Names are package names (letters, digits, '.', '+', '-'): no quoting needed.
                        String names(MemSpace::Psram);
                        for (const Row &r : self->_rows) {
                                if (!r.box->isChecked()) continue;
                                if (!names.isEmpty()) (void)names.append(' ');
                                (void)names.append(self->_offers[r.offer].name.view());
                        }
                        if (!names.isEmpty()) self->installNames(names.view());
                }
                return;
        }

        void WelcomeWindow::toggled(void *arg, CheckBox &) {
                static_cast<WelcomeWindow *>(arg)->updateChosen();
                return;
        }

        // Kept for the next starts: set and saved.
        void WelcomeWindow::showToggled(void *, CheckBox &box) {
                const StringView key("pkg/welcome");
                if (SysConf::set(key, box.isChecked() ? StringView("on") : StringView("off")).isOk())
                        (void)SysConf::save(key);
                return;
        }

        int welcomeMain(const BuiltinProgram::Context &context) {
                FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                const auto  complain = [err](const char *text) {
                        if (err != nullptr)
                                (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                  WaitForever);
                };
                bool firstRun = false;
                if (context.args.size() == 2 && context.args[1] == StringView("--first-run")) {
                        firstRun = true;
                } else if (context.args.size() != 1) {
                        complain("usage: welcome [--first-run]\n");
                        return 2;
                }
                if (firstRun) {
                        // Switched off, or no card where packages go: nothing to welcome to.
                        const Result<bool> on = SysConf::getBool(StringView("pkg/welcome"));
                        if (on.isOk() && !on.value()) return 0;
                        String root(MemSpace::Psram);
                        (void)SysConf::get(StringView("pkg/root"), root);
                        auto [stat, serr] = Vfs::stat(root.view(), Vfs::Context{"/", nullptr});
                        if (serr.isError() || !stat.isDirectory()) return 0;
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                int   status = 1;
                if (app->connect(0).isError()) {
                        // At startup without a desktop: quietly.
                        if (!firstRun) complain("welcome: the window system isn't running ('win start')\n");
                        status = firstRun ? 0 : 1;
                } else {
                        GuiWindow::Config config;
                        config.rect = Rect{200, 120, 600, 400};
                        config.title = "Welcome";
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(WelcomeWindow));
                        if (wmem != nullptr) {
                                auto *window = ::new (wmem) WelcomeWindow(*app, config, context, firstRun);
                                Error e;
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        e = window->build();
                                        // A first run shows once it knows the card is new.
                                        if (e.isOk() && !firstRun) e = window->show();
                                }
                                if (e.isOk()) {
                                        status = app->exec();
                                } else {
                                        complain("welcome: can't open its window\n");
                                }
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        window->~WelcomeWindow();
                                }
                                Memory::free(wmem);
                        }
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM(welcome, welcomeMain, 12 * 1024)

BMDS_NAMESPACE_END
