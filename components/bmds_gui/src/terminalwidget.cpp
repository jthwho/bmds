/**
 * @file      terminalwidget.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A terminal in a widget (devplan/editor.md §7).
 */

#include <bmds/terminalwidget.h>

#include <cstdio>
#include <bmds/fdtable.h>
#include <bmds/bitmapfont.h>
#include <bmds/glyphrenderer.h>
#include <bmds/guiapplication.h>
#include <bmds/guiclipboard.h>
#include <bmds/guiwindow.h>
#include <bmds/memory.h>
#include <bmds/pty.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/terminalkeys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t CellWidth = 8;
        constexpr uint16_t CellHeight = 16;
        constexpr uint16_t DefaultColumns = 80;
        constexpr uint16_t DefaultRows = 8;
        constexpr int      WheelLines = 3;
        constexpr uint64_t MultiClickUs = 400000;
        constexpr size_t   ReaderStackBytes = 4096;
        constexpr size_t   RunnerStackBytes = 6144;
        constexpr int      SigTerm = 15; // Linux's numbers, the system's (sdk/include/bmds/sys.h)
        constexpr int      SigKill = 9;

        // The terminal application's colours (terminal/colors/NAME).
        constexpr const char *ColorNames[] = {"text",    "background", "cursor",  "color0",  "color1",
                                              "color2",  "color3",     "color4",  "color5",  "color6",
                                              "color7",  "color8",     "color9",  "color10", "color11",
                                              "color12", "color13",    "color14", "color15"};

        constexpr uint8_t letter(char ch) {
                return static_cast<uint8_t>(HidKey::A + (ch - 'a'));
        }

        GuiClipboard *clipboardOf(const Widget &w) {
                GuiWindow *window = w.window();
                return window != nullptr ? window->application().clipboard() : nullptr;
        }

        // A wait status (Linux's layout) as an exit status: the code, or 128 and the signal.
        int exitStatus(int32_t status) {
                if ((status & 0x7f) == 0) return (status >> 8) & 0xff;
                return 128 + (status & 0x7f);
        }

} // namespace

TerminalWidget::TerminalWidget(Widget *parent) : Widget(parent) {
        setFocusable(true);
        readColors();
        _cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Internal, sizeof(TextCell) * MaxColumns));
        if (_cells == nullptr)
                _cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Psram, sizeof(TextCell) * MaxColumns));
        Mutex::Locker lock(_lock, WaitForever);
        _screen.setReplySink(reply, this);
        if (_screen.resize(DefaultColumns, DefaultRows).isOk()) (void)_screen.setHistory(HistoryLines);
        _selection.setColumns(DefaultColumns);
}

TerminalWidget::~TerminalWidget() {
        if (isRunning()) (void)stop(true);
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (_pty != nullptr) _pty->hangUp(); // the reads end
        }
        joinThreads();
        GuiWindow *w = window();
        if (w != nullptr) {
                w->application().cancelPosts(this);
                GuiClipboard *clipboard = w->application().clipboard();
                if (clipboard != nullptr) clipboard->cancel(this);
        }
        Memory::free(_cells);
}

void TerminalWidget::readColors() {
        for (const char *name : ColorNames) {
                char key[40];
                std::snprintf(key, sizeof(key), "terminal/colors/%s", name);
                const Result<uint32_t> rgb = SysConf::getColor(StringView(key));
                uint32_t              *to = _colors.find(StringView(name));
                if (rgb.isOk() && to != nullptr) *to = rgb.value();
        }
        return;
}

GuiSize TerminalWidget::sizeHint() const {
        return GuiSize{uint16_t(DefaultColumns * CellWidth + 8), uint16_t(DefaultRows * CellHeight)};
}

// ---- Running

Error TerminalWidget::start(const Command &command) {
        if (isRunning()) return Error::Busy;
        if (command.launcher == nullptr || command.process == nullptr) return Error::NotSupported;
        joinThreads(); // the last run's
        Error err = _path.assign(command.path);
        if (err.isOk()) err = _cwd.assign(command.cwd);
        _argText.clear();
        _args.clear();
        for (const StringView &a : command.args) {
                if (err.isOk()) err = _argText.append(a);
                if (err.isOk()) err = _argText.append('\0');
        }
        if (err.isOk()) {
                // Views into the copy, made once it no longer grows.
                size_t at = 0;
                for (const StringView &a : command.args) {
                        if (err.isOk()) err = _args.append(_argText.view().mid(at, a.size()));
                        at += a.size() + 1;
                }
        }
        if (err.isError()) return err;
        _environment = command.environment;
        _launcher = command.launcher;
        _process = command.process;
        _extraCount = 0;
        for (ExtraFd &e : command.extraFds) {
                if (_extraCount == 2) break;
                _extra[_extraCount].fd = e.fd;
                _extra[_extraCount].handle = std::move(e.handle);
                ++_extraCount;
        }
        auto [pty, perr] = Pty::open(MemSpace::Psram);
        if (perr.isError()) return perr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                _pty = pty;
        }
        fit();
        _running.store(true, std::memory_order_release);
        _pid = -1;
        Thread::Config config;
        config.name = "termread";
        config.stackBytes = ReaderStackBytes;
        config.priority = 5;
        err = _reader.start(config, readerMain, this);
        if (err.isError()) {
                _running.store(false, std::memory_order_release);
                Mutex::Locker lock(_lock, WaitForever);
                pty->release();
                _pty = nullptr;
                return err;
        }
        _threads = true;
        config.name = "termrun";
        config.stackBytes = RunnerStackBytes;
        if (_runner.start(config, runnerMain, this).isError()) {
                // As a program that couldn't start: said, and ended (the reader sees the end).
                _status.store(127, std::memory_order_relaxed);
                (void)pty->console().write(StringView("can't start it: out of memory\r\n"), 100);
                pty->programEnded();
        }
        return Error::Ok;
}

// The command on the pty's program end, a session of its own; waited for, then the pty told.
void TerminalWidget::runnerMain(void *arg) {
        auto       *self = static_cast<TerminalWidget *>(arg);
        Pty        *pty = self->_pty;
        PtyConsole &console = pty->console();
        int         status = 127;
        auto [handle, err] = FileHandle::open(console, FileHandle::ReadWrite);
        if (err.isOk()) {
                FdTable fds;
                (void)fds.set(FdTable::Stdin, handle);
                (void)fds.set(FdTable::Stdout, handle);
                (void)fds.set(FdTable::Stderr, std::move(handle));
                for (size_t i = 0; i < self->_extraCount; ++i)
                        (void)fds.set(self->_extra[i].fd, std::move(self->_extra[i].handle));
                ProgramLauncher::Request request;
                request.path = self->_path.view();
                request.args = self->_args.span();
                request.environment = self->_environment;
                request.fds = &fds;
                request.cwd = self->_cwd.view();
                request.console = &console;
                request.session = true;
                request.waited = true;
                auto [pid, serr] = self->_launcher->start(request);
                err = serr;
                if (err.isOk()) {
                        self->_pid = pid;
                        int32_t raw = 0;
                        for (;;) {
                                auto [taken, werr] = self->_process->waitChild(pid, ProcessControl::WaitBlock, raw);
                                if (werr == Error::Interrupted) continue;
                                if (werr.isOk() && taken == 0) continue;
                                status = werr.isOk() ? exitStatus(raw) : 1;
                                break;
                        }
                }
        }
        if (err.isError()) {
                char      text[96];
                const int n = std::snprintf(text, sizeof(text), "%.*s: %s\r\n", int(self->_path.size()),
                                            self->_path.data(), err == Error::NotExist ? "not found" : err.desc());
                (void)console.write(StringView(text, n > 0 ? size_t(n) : 0), 100);
        }
        self->_status.store(status, std::memory_order_relaxed);
        pty->programEnded();
        return;
}

// What the program writes, onto the screen; drawn on the loop's thread.
void TerminalWidget::readerMain(void *arg) {
        auto *self = static_cast<TerminalWidget *>(arg);
        Pty  *pty = self->_pty;
        for (;;) {
                uint8_t buf[512];
                auto [n, err] = pty->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                if (err == Error::EndOfFile) break;
                if (err.isError()) continue;
                {
                        Mutex::Locker lock(self->_lock, WaitForever);
                        self->_screen.write(StringView(reinterpret_cast<const char *>(buf), n));
                }
                GuiWindow *w = self->window();
                if (w != nullptr && !self->_outputPosted.exchange(true, std::memory_order_acq_rel))
                        (void)w->application().post(outputDue, self);
        }
        {
                Mutex::Locker lock(self->_lock, WaitForever);
                self->_pty = nullptr;
        }
        pty->release();
        GuiWindow *w = self->window();
        if (w != nullptr) (void)w->application().post(endDue, self);
        return;
}

void TerminalWidget::outputDue(void *arg) {
        auto *self = static_cast<TerminalWidget *>(arg);
        self->_outputPosted.store(false, std::memory_order_release);
        self->update();
        return;
}

void TerminalWidget::endDue(void *arg) {
        auto *self = static_cast<TerminalWidget *>(arg);
        self->joinThreads();
        self->_running.store(false, std::memory_order_release);
        self->_pid = -1;
        self->update();
        if (self->_ended != nullptr)
                self->_ended(self->_endedArg, *self, self->_status.load(std::memory_order_relaxed));
        return;
}

void TerminalWidget::joinThreads() {
        if (!_threads) return;
        _runner.join();
        _reader.join();
        _threads = false;
        return;
}

Error TerminalWidget::stop(bool kill) {
        if (!isRunning() || _launcher == nullptr) return Error::InvalidState;
        if (_pid < 0) return Error::NotReady;
        return _launcher->sendSignal(_pid, kill ? SigKill : SigTerm);
}

void TerminalWidget::reply(void *context, StringView text) {
        auto *self = static_cast<TerminalWidget *>(context);
        // The lock is held (the screen is being written): the pty is safe to use.
        if (self->_pty != nullptr)
                (void)self->_pty->write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), 0);
        return;
}

void TerminalWidget::clear() {
        {
                Mutex::Locker lock(_lock, WaitForever);
                _screen.write("\x1b[3J\x1b[H\x1b[2J"); // the history, then the screen
                _selection.clear();
                _offset = 0;
        }
        update();
        return;
}

void TerminalWidget::show(StringView text) {
        {
                Mutex::Locker lock(_lock, WaitForever);
                _screen.write(text);
        }
        update();
        return;
}

// ---- Size

void TerminalWidget::fit() {
        const Rect     g = geometry();
        const uint16_t usable = g.width > 4 ? uint16_t(g.width - 4) : 0;
        uint16_t       columns = uint16_t(usable / CellWidth);
        if (columns > MaxColumns) columns = MaxColumns;
        const uint16_t rows = uint16_t(g.height / CellHeight);
        if (columns == 0 || rows == 0) return;
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (columns != _screen.columns() || rows != _screen.rows()) {
                        if (_screen.resize(columns, rows).isError()) return;
                        _selection.clear();
                        _selection.setColumns(columns);
                }
                if (_pty != nullptr) _pty->setSize(Console::Size{columns, rows});
        }
        if (columns == _columnsShown && rows == _rowsShown) return;
        _columnsShown = columns;
        _rowsShown = rows;
        if (_sized != nullptr) _sized(_sizedArg, *this, columns, rows);
        return;
}

void TerminalWidget::resized() {
        fit();
        update();
        return;
}

// ---- Drawing

void TerminalWidget::paint(Painter &painter) {
        const Rect     g = geometry();
        const uint16_t x = uint16_t((g.x + 3) & ~3u); // cells start on a 4-pixel column
        (void)painter.fill(g, _colors.background);
        if (_cells == nullptr) return;
        const GuiWindow *w = window();
        const bool       focused = hasFocus() && w != nullptr && w->isActive();
        Mutex::Locker    lock(_lock, WaitForever);
        const uint16_t   columns = _screen.columns(), rows = _screen.rows();
        // Scrolled back: the view stays on the same lines as output adds to the history.
        const uint32_t added = _screen.historyAdded();
        if (_offset > 0) {
                _offset += added - _added;
                if (_offset > _screen.historyCount()) _offset = uint32_t(_screen.historyCount());
        }
        _added = added;
        const int cursorRow = focused && isRunning() ? TerminalView::viewCursorRow(_screen, _offset) : -1;
        for (uint16_t r = 0; r < rows; ++r) {
                const int y = int(g.y) + r * CellHeight;
                if (y + CellHeight > int(g.bottom())) break;
                uint16_t            length = 0;
                const TerminalCell *line = TerminalView::viewRow(_screen, _offset, r, length);
                uint16_t            from = 0, to = 0;
                (void)_selection.columns(TerminalView::lineOfViewRow(_screen, _offset, r), from, to);
                const size_t n =
                        TerminalView::cells(line, length, columns, _colors,
                                            cursorRow == r ? _screen.cursorColumn() : -1, _cells, MaxColumns, from, to);
                (void)painter.cells(x, y, Span<const TextCell>(_cells, n), Fonts::unscii16,
                                    uint16_t(g.right() > x ? g.right() - x : 0));
                _screen.clearRowDamage(r);
        }
        return;
}

void TerminalWidget::scrollBy(int lines) {
        {
                Mutex::Locker lock(_lock, WaitForever);
                const int64_t want = int64_t(_offset) + lines;
                const int64_t most = int64_t(_screen.historyCount());
                _offset = uint32_t(want < 0 ? 0 : want > most ? most : want);
        }
        update();
        return;
}

bool TerminalWidget::wheelTurned(int notches) {
        scrollBy(notches * WheelLines);
        return true;
}

WindowManager::Pointer TerminalWidget::pointerAt(int, int) const {
        return WindowManager::Pointer::Text;
}

void TerminalWidget::focusIn() {
        update();
        return;
}

void TerminalWidget::focusOut() {
        update();
        return;
}

// ---- Keys

bool TerminalWidget::keyPressed(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        const bool ctrlShift = event.hasCtrl() && event.hasShift() && !event.hasAlt();
        if (ctrlShift && event.usage == letter('c')) {
                copySelection(Clipboard::Selection::Clipboard);
                return true;
        }
        if ((ctrlShift && event.usage == letter('v')) ||
            (event.hasShift() && !event.hasCtrl() && event.usage == HidKey::Insert)) {
                GuiClipboard *clipboard = clipboardOf(*this);
                if (clipboard != nullptr)
                        (void)clipboard->paste(Clipboard::Selection::Clipboard, ClipboardType::Text, pasted, this);
                return true;
        }
        bool alternate = false;
        int  rows = 0;
        {
                Mutex::Locker lock(_lock, WaitForever);
                alternate = _screen.isAlternateScreen();
                rows = _screen.rows();
        }
        if ((event.usage == HidKey::PageUp || event.usage == HidKey::PageDown) && event.hasShift() && !alternate) {
                scrollBy(event.usage == HidKey::PageUp ? rows - 1 : 1 - rows);
                return true;
        }
        const bool running = isRunning();
        if (!running && _typed == nullptr) return false; // nothing to type to: the window's keys
        char   bytes[TerminalKeys::MaxBytes];
        size_t n = 0;
        {
                Mutex::Locker lock(_lock, WaitForever);
                n = TerminalKeys::encode(event, _screen.isApplicationCursorKeys(), bytes);
                if (n > 0 && running && _pty != nullptr)
                        (void)_pty->write(ByteSpan(reinterpret_cast<const uint8_t *>(bytes), n), 0);
        }
        if (n == 0) return false;
        if (!running) _typed(_typedArg, *this, StringView(bytes, n)); // its owner's program
        if (_offset != 0) scrollBy(-int(HistoryLines)); // typing returns to the bottom
        return true;
}

void TerminalWidget::pasted(void *arg, Error err, StringView type, StringView data) {
        (void)type;
        auto *self = static_cast<TerminalWidget *>(arg);
        if (err.isError()) return;
        if (!self->isRunning()) {
                if (self->_typed != nullptr) self->_typed(self->_typedArg, *self, data);
                return;
        }
        Mutex::Locker lock(self->_lock, WaitForever);
        if (self->_pty != nullptr)
                (void)self->_pty->write(ByteSpan(reinterpret_cast<const uint8_t *>(data.data()), data.size()), 0);
        return;
}

// ---- Selection

TerminalSelection::Point TerminalWidget::pointAt(int x, int y, bool cell) {
        const Rect g = geometry();
        const int  rows = _screen.rows(), columns = _screen.columns();
        int        r = y < int(g.y) ? 0 : (y - int(g.y)) / CellHeight;
        if (r >= rows) r = rows - 1;
        const int left = int((g.x + 3) & ~3u);
        int       c = cell ? (x - left) / CellWidth : (x - left + CellWidth / 2) / CellWidth;
        if (c < 0) c = 0;
        if (c > (cell ? columns - 1 : columns)) c = cell ? columns - 1 : columns;
        return TerminalSelection::Point{TerminalView::lineOfViewRow(_screen, _offset, uint16_t(r)), uint16_t(c)};
}

const TerminalCell *TerminalWidget::selectionLine(void *arg, int64_t line, uint16_t &length) {
        return TerminalView::line(static_cast<TerminalWidget *>(arg)->_screen, line, length);
}

void TerminalWidget::mousePressed(const WindowManager::MouseEvent &event) {
        setFocus();
        if (event.button == PointerEvent::Middle) {
                GuiClipboard *clipboard = clipboardOf(*this);
                if (clipboard != nullptr && isRunning())
                        (void)clipboard->paste(Clipboard::Selection::Primary, ClipboardType::Text, pasted, this);
                return;
        }
        if (event.button != PointerEvent::Left) return;
        const uint64_t now = System::uptimeUs();
        _clicks = now - _pressUs < MultiClickUs ? uint8_t(_clicks % 3 + 1) : uint8_t(1);
        _pressUs = now;
        {
                Mutex::Locker lock(_lock, WaitForever);
                const auto    unit = _clicks == 1   ? TerminalSelection::Unit::Character
                                     : _clicks == 2 ? TerminalSelection::Unit::Word
                                                    : TerminalSelection::Unit::Line;
                _selection.begin(pointAt(event.x, event.y, _clicks != 1), unit, false, selectionLine, this);
        }
        _selecting = true;
        update();
        return;
}

void TerminalWidget::mouseMoved(const WindowManager::MouseEvent &event) {
        if (!_selecting || (event.buttons & PointerEvent::Left) == 0) return;
        {
                Mutex::Locker lock(_lock, WaitForever);
                _selection.extend(pointAt(event.x, event.y, _clicks != 1), selectionLine, this);
        }
        update();
        return;
}

void TerminalWidget::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !_selecting) return;
        _selecting = false;
        copySelection(Clipboard::Selection::Primary);
        return;
}

Error TerminalWidget::collect(void *arg, StringView piece) {
        return static_cast<TerminalWidget *>(arg)->_copy->append(piece);
}

void TerminalWidget::copySelection(Clipboard::Selection to) {
        GuiClipboard *clipboard = clipboardOf(*this);
        if (clipboard == nullptr) return;
        String text(MemSpace::Psram);
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (_selection.isEmpty()) return;
                _copy = &text;
                (void)_selection.text(selectionLine, this, collect, this);
                _copy = nullptr;
        }
        if (!text.isEmpty()) (void)clipboard->copyText(to, text.view());
        return;
}

BMDS_NAMESPACE_END
