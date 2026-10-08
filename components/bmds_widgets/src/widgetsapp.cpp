/**
 * @file      widgetsapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/widgets: a window with the toolkit's furniture, to try it (devplan/filebrowser.md §4e).
 * A menu bar, a tool bar, a splitter between a list and a large grid (each a scroll area),
 * a status bar, and two dialogs: a question, and a progress bar with Cancel.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/dialog.h>
#include <bmds/fdtable.h>
#include <bmds/font.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/menubar.h>
#include <bmds/scrollarea.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t RowHeight = 16;
        constexpr uint32_t Rows = 200;
        constexpr uint16_t Cell = 64;
        constexpr uint32_t GridWidth = 32 * Cell;
        constexpr uint32_t GridHeight = 24 * Cell;
        constexpr uint32_t ProgressMs = 50;

        enum Choice : int {
                Question = 1,
                Copy,
                Quit,
                ShowStatus,
                About,
        };

        // Window x and y cut to the viewport: what of a content rectangle may be filled.
        Rect visiblePart(const ScrollArea &area, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const Rect &viewport) {
                const int left = area.windowX(x), top = area.windowY(y);
                const int l = left > viewport.x ? left : viewport.x;
                const int t = top > viewport.y ? top : viewport.y;
                const int r = left + int(w) < int(viewport.right()) ? left + int(w) : int(viewport.right());
                const int b = top + int(h) < int(viewport.bottom()) ? top + int(h) : int(viewport.bottom());
                if (r <= l || b <= t) return Rect();
                return Rect{uint16_t(l), uint16_t(t), uint16_t(r - l), uint16_t(b - t)};
        }

        // A list of rows, one selected: the arrows move it, a press picks it.
        class RowList final : public ScrollArea {
                public:
                        using Selected = void (*)(void *arg, uint32_t row);

                        explicit RowList(Widget *parent) : ScrollArea(parent) {
                                setContentSize(240, Rows * RowHeight);
                                setLineSteps(8, RowHeight);
                                setFocusable(true);
                        }

                        void setSelected(Selected fn, void *arg) {
                                _selected = fn;
                                _arg = arg;
                                return;
                        }

                        void select(uint32_t row) {
                                if (row >= Rows) row = Rows - 1;
                                _row = row;
                                ensureVisible(0, row * RowHeight, 1, RowHeight);
                                update();
                                if (_selected != nullptr) _selected(_arg, row);
                                return;
                        }

                        void mousePressed(const WindowManager::MouseEvent &event) override {
                                const Rect v = viewport();
                                if (event.button != PointerEvent::Left || event.y < v.y || event.y >= v.bottom())
                                        return;
                                select((uint32_t(event.y - v.y) + scrollY()) / RowHeight);
                                return;
                        }

                        bool keyPressed(const KeyEvent &event) override {
                                const uint32_t page = viewport().height / RowHeight;
                                switch (event.usage) {
                                        case HidKey::Up: select(_row > 0 ? _row - 1 : 0); return true;
                                        case HidKey::Down: select(_row + 1); return true;
                                        case HidKey::PageUp: select(_row > page ? _row - page : 0); return true;
                                        case HidKey::PageDown: select(_row + page); return true;
                                        case HidKey::Home: select(0); return true;
                                        case HidKey::End: select(Rows - 1); return true;
                                        default: return false;
                                }
                        }

                protected:
                        void paintContent(Painter &painter, const Rect &viewport) override {
                                const WindowPalette &p = painter.palette();
                                const uint32_t       face = p.color(Role::FaceHighlight);
                                (void)painter.fill(viewport, face);
                                const uint32_t first = scrollY() / RowHeight;
                                const uint32_t last = (scrollY() + viewport.height) / RowHeight;
                                for (uint32_t row = first; row <= last && row < Rows; ++row) {
                                        const bool     on = row == _row;
                                        const uint32_t bg = on ? p.color(Role::Highlight) : face;
                                        if (on)
                                                (void)painter.fill(visiblePart(*this, 0, row * RowHeight,
                                                                               contentWidth(), RowHeight, viewport),
                                                                   bg);
                                        char text[32];
                                        std::snprintf(text, sizeof(text), "Row %lu: an item's name",
                                                      static_cast<unsigned long>(row + 1));
                                        const int x = windowX(4);
                                        (void)painter.text(x, windowY(row * RowHeight), StringView(text),
                                                           Fonts::unscii16,
                                                           p.color(on ? Role::HighlightText : Role::FaceText), bg,
                                                           uint16_t(contentWidth() - 8));
                                }
                                return;
                        }

                private:
                        Selected _selected = nullptr;
                        void    *_arg = nullptr;
                        uint32_t _row = 0;
        };

        // A large checkerboard, each square labelled with where it is.
        class Grid final : public ScrollArea {
                public:
                        explicit Grid(Widget *parent) : ScrollArea(parent) {
                                setContentSize(GridWidth, GridHeight);
                                setLineSteps(Cell / 2, Cell / 2);
                        }

                protected:
                        void paintContent(Painter &painter, const Rect &viewport) override {
                                const uint32_t x0 = scrollX() / Cell, y0 = scrollY() / Cell;
                                const uint32_t x1 = (scrollX() + viewport.width) / Cell;
                                const uint32_t y1 = (scrollY() + viewport.height) / Cell;
                                for (uint32_t cy = y0; cy <= y1 && cy * Cell < GridHeight; ++cy) {
                                        for (uint32_t cx = x0; cx <= x1 && cx * Cell < GridWidth; ++cx) {
                                                const uint32_t bg = ((cx + cy) & 1) != 0 ? 0x5878a8 : 0x88a8d8;
                                                (void)painter.fill(
                                                        visiblePart(*this, cx * Cell, cy * Cell, Cell, Cell, viewport),
                                                        bg);
                                                char label[24];
                                                std::snprintf(label, sizeof(label), "%lu,%lu",
                                                              static_cast<unsigned long>(cx),
                                                              static_cast<unsigned long>(cy));
                                                (void)painter.text(windowX(cx * Cell + 4), windowY(cy * Cell + 24),
                                                                   StringView(label), Fonts::unscii16, 0xffffff, bg,
                                                                   Cell - 8);
                                        }
                                }
                                return;
                        }
        };

        class WidgetsWindow final : public GuiWindow {
                public:
                        WidgetsWindow(GuiApplication &app, const Config &config)
                            : GuiWindow(app, config), _file(app), _view(app), _help(app),
                              _question(app, "Question", 280, 40), _copying(app, "Copying", 280, 48) {}

                        Error build();

                protected:
                        void closedByManager() override {
                                application().quit(0);
                                return;
                        }

                private:
                        static void chosen(void *arg, int item);
                        static void tool(void *arg, Button &button);
                        static void rowSelected(void *arg, uint32_t row);
                        static void answered(void *arg, Dialog &dialog, int result);
                        static void copied(void *arg, Dialog &dialog, int result);
                        static void tick(void *arg);
                        void        startCopy();
                        void        status(const char *text);

                        Menu         _file;
                        Menu         _view;
                        Menu         _help;
                        Dialog       _question;
                        Dialog       _copying;
                        StatusBar   *_status = nullptr;
                        Label       *_answer = nullptr;
                        ProgressBar *_progress = nullptr;
                        int          _timer = -1;
                        uint32_t     _done = 0;
                        bool         _statusShown = true;
        };

        Error WidgetsWindow::build() {
                auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
                if (column == nullptr) return Error::NoMem;
                setCentral(column);
                auto *bar = Widget::create<MenuBar>(column);
                auto *tools = Widget::create<ToolBar>(column);
                auto *split = Widget::create<Splitter>(column, Splitter::Orientation::Horizontal);
                _status = Widget::create<StatusBar>(column);
                if (bar == nullptr || tools == nullptr || split == nullptr || _status == nullptr) return Error::NoMem;

                (void)_file.addItem("&Question...", Question);
                (void)_file.addItem("&Copy...", Copy);
                (void)_file.addSeparator();
                (void)_file.addItem("E&xit", Quit, 0, "Alt+F4");
                (void)_view.addItem("&Status Bar", ShowStatus, MenuLayout::Checked);
                (void)_help.addItem("&About", About);
                for (Menu *m : {&_file, &_view, &_help}) m->setTriggered(chosen, this);
                (void)bar->addMenu("&File", _file);
                (void)bar->addMenu("&View", _view);
                (void)bar->addMenu("&Help", _help);
                setMenuBar(bar);

                (void)tools->addButton(nullptr, "Back", tool, this);
                (void)tools->addButton(nullptr, "Forward", tool, this);
                (void)tools->addSeparator();
                (void)tools->addButton(nullptr, "Question", tool, this);
                (void)tools->addButton(nullptr, "Copy", tool, this);

                auto *list = Widget::create<RowList>(split);
                auto *grid = Widget::create<Grid>(split);
                if (list == nullptr || grid == nullptr) return Error::NoMem;
                list->setSelected(rowSelected, this);
                split->setLeast(120, 120);
                split->setPosition(260);
                split->setWidgets(list, grid);
                split->setStretch(1);

                (void)_status->addField(0);
                (void)_status->addField(120);
                status("Ready");

                _answer = Widget::create<Label>(_question.content(), "Is the toolkit furnished now?");
                _question.addButton("&Yes", 1);
                _question.addButton("&No", 0);
                _question.setFinished(answered, this);

                (void)Widget::create<Label>(_copying.content(), "Copying 100 files...");
                _progress = Widget::create<ProgressBar>(_copying.content());
                _copying.addButton("&Cancel", 0);
                _copying.setFinished(copied, this);
                column->relayout();
                list->setFocus();
                return Error::Ok;
        }

        void WidgetsWindow::status(const char *text) {
                _status->setText(0, StringView(text));
                return;
        }

        void WidgetsWindow::chosen(void *arg, int item) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                switch (item) {
                        case Question: (void)self->_question.open(); break;
                        case Copy: self->startCopy(); break;
                        case Quit: self->application().quit(0); break;
                        case ShowStatus:
                                self->_statusShown = !self->_statusShown;
                                self->_status->setVisible(self->_statusShown);
                                static_cast<BoxLayout *>(self->_status->parent())->relayout();
                                self->_view.clear();
                                (void)self->_view.addItem("&Status Bar", ShowStatus,
                                                          self->_statusShown ? MenuLayout::Checked : 0);
                                break;
                        case About: self->status("BMDS widgets: the toolkit's furniture"); break;
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
                        char line[40];
                        std::snprintf(line, sizeof(line), "%.*s pressed", int(text.size()), text.data());
                        self->status(line);
                }
                return;
        }

        void WidgetsWindow::rowSelected(void *arg, uint32_t row) {
                auto *self = static_cast<WidgetsWindow *>(arg);
                char  text[24];
                std::snprintf(text, sizeof(text), "Row %lu", static_cast<unsigned long>(row + 1));
                self->_status->setText(1, StringView(text));
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
                                window->~WidgetsWindow();
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
