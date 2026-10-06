/**
 * @file      terminalapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The terminal application, /bin/terminal (devplan/windows.md §8 step 9): a
 * window on the window manager, a pseudo-terminal (devplan/pty.md), and an
 * interactive shell on the pseudo-terminal's console: `/bin/sh -i`, its child
 * process, leading a session there (devplan/processes.md §3.3). One process
 * a window. It uses only what any program may: the window manager's API, a
 * pty, the program launcher, a thread, and the font and glyph drawing of the
 * GUI.
 *
 *   terminal [X Y]   a window with its top-left corner at X, Y (100 x 30 cells)
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sdkconfig.h>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/painter.h>
#include <bmds/programlauncher.h>
#include <bmds/pty.h>
#include <bmds/string.h>
#include <bmds/terminalkeys.h>
#include <bmds/terminalscreen.h>
#include <bmds/terminalview.h>
#include <bmds/thread.h>
#include <bmds/utf8.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t CellWidth = 8;
        constexpr uint16_t CellHeight = 16;
        constexpr uint16_t MaxColumns = 240; // the widest row drawn (1920 pixels)
        constexpr uint16_t HistoryLines = 1000;
        constexpr int      WheelLines = 3; // a notch of the wheel
        constexpr uint16_t Columns = 100;  // a new window's
        constexpr uint16_t Rows = 30;
        constexpr size_t   ShellThreadStackBytes = 8192; // starts the shell and waits for it

        class TerminalApp {
                public:
                        explicit TerminalApp(const BuiltinProgram::Context &context) : _context(context) {}
                        ~TerminalApp() { Memory::free(_cells); }

                        int run();

                private:
                        // The window's (on the window manager's task, which holds its lock).
                        static void redraw(void *arg, Painter &painter);
                        static void closed(void *arg);
                        static void keyTyped(void *arg, const KeyEvent &event);
                        static void resized(void *arg, uint16_t width, uint16_t height);
                        static void wheeled(void *arg, int notches);
                        static void dump(void *arg, Stream &out);
                        static void reference(void *arg, uint8_t *pixels, size_t stride, uint16_t width,
                                              uint16_t height, const WindowPalette &palette);
                        // The connection's, and the thread that runs the shell.
                        static void gone(void *arg);
                        static void shellMain(void *arg);
                        static void reply(void *context, StringView text);

                        void   say(const char *text); // to stderr
                        void   draw(Painter &painter);
                        size_t send(const KeyEvent &event);
                        void   scrollBy(int lines);
                        void   composeTitle(char (&out)[96]);
                        void   update();

                        const BuiltinProgram::Context &_context;
                        WindowManager                 *_wm = nullptr;
                        uint8_t                        _client = 0;
                        std::atomic<uint8_t>           _window{0};
                        std::atomic<bool>              _gone{false}; // the window manager stopped
                        Pty                           *_pty = nullptr;
                        char                           _name[8] = {}; // the shell's thread: "termN"
                        Mutex                          _lock;         // the screen
                        TerminalScreen                 _screen{MemSpace::Psram};
                        uint32_t                       _titleSeen = 0;
                        TextCell                      *_cells = nullptr; // a row's, internal RAM
                        std::atomic<uint32_t>          _offset{0};       // lines scrolled back
                        uint32_t                       _added = 0;       // the history's lines added, as drawn
                        bool                           _all = false;     // every row to redraw
                        uint32_t                       _cursorRows[(TerminalScreen::MaxSide + 31) / 32] = {};
                        Thread                         _shell; // runs the shell, until it ends
        };

        void TerminalApp::say(const char *text) {
                FileHandle *err = _context.fds != nullptr ? _context.fds->handle(FdTable::Stderr) : nullptr;
                if (err != nullptr)
                        (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                         WaitForever);
                return;
        }

        int TerminalApp::run() {
                // A window where asked: terminal [X Y].
                Rect rect{32, 32, WindowFrame::outerWidth(Columns * CellWidth, WindowFrame::Standard),
                          WindowFrame::outerHeight(Rows * CellHeight, WindowFrame::Standard)};
                if (_context.args.size() == 3) {
                        rect.x = static_cast<uint16_t>(std::atoi(String::fromView(_context.args[1]).value().cStr()));
                        rect.y = static_cast<uint16_t>(std::atoi(String::fromView(_context.args[2]).value().cStr()));
                } else if (_context.args.size() != 1) {
                        say("usage: terminal [X Y]\n");
                        return 2;
                }
                auto [wm, merr] = WindowManager::manager(0);
                if (merr.isOk()) {
                        auto [client, cerr] = wm->connect(gone, this);
                        merr = cerr;
                        _client = client;
                }
                if (merr.isError()) {
                        say(merr == Error::InvalidState ? "terminal: the window system isn't running ('win start')\n"
                                                        : "terminal: can't connect to the window manager\n");
                        return 1;
                }
                _wm = wm;
                _cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Internal, sizeof(TextCell) * MaxColumns));
                auto [pty, perr] = Pty::open(MemSpace::Psram);
                Error err = _cells == nullptr ? Error(Error::NoMem) : perr;
                if (err.isOk()) {
                        _pty = pty;
                        std::snprintf(_name, sizeof(_name), "term%u", pty->number());
                        Mutex::Locker lock(_lock, WaitForever);
                        _screen.setReplySink(reply, this);
                        err = _screen.resize(Columns, Rows);
                        if (err.isOk()) (void)_screen.setHistory(HistoryLines); // none without the memory
                }
                uint8_t id = 0;
                if (err.isOk()) {
                        WindowManager::WindowConfig config;
                        config.rect = rect;
                        config.title = "Terminal";
                        config.redraw = redraw;
                        config.closed = closed;
                        config.key = keyTyped;
                        config.resized = resized;
                        config.widthStep = CellWidth; // sized by hand in whole cells
                        config.heightStep = CellHeight;
                        config.pointer = WindowManager::Pointer::Text;
                        config.hidePointerWhileTyping = true;
                        config.wheel = wheeled;
                        config.client = _client;
                        config.kind = "terminal";
                        config.dump = dump;
                        config.reference = reference;
                        config.arg = this;
                        auto [opened, oerr] = _wm->open(config);
                        err = oerr;
                        id = opened;
                }
                if (err.isOk()) {
                        _window.store(id, std::memory_order_release);
                        // Placed smaller than asked (the screen's edge): the size it got.
                        auto [placed, cerr] = _wm->clientRect(id);
                        if (cerr.isOk()) resized(this, placed.width, placed.height);
                        if (_context.launcher == nullptr) err = Error::NotSupported;
                }
                if (err.isOk()) {
                        Thread::Config config;
                        config.name = _name;
                        config.stackBytes = ShellThreadStackBytes;
                        config.priority = CONFIG_BMDS_SHELL_PRIORITY;
                        err = _shell.start(config, shellMain, this);
                }
                if (err.isError()) {
                        say("terminal: ");
                        say(err.desc());
                        say("\n");
                        if (id != 0) (void)_wm->close(id);
                        _wm->disconnect(_client);
                        if (_pty != nullptr) _pty->release();
                        return 1;
                }

                // What the shell writes, onto the screen; drawn at the window's next round.
                bool quitting = false;
                for (;;) {
                        uint8_t buf[512];
                        auto [n, rerr] = _pty->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                        if (rerr == Error::EndOfFile) break;
                        if (rerr == Error::Interrupted) {
                                // Ctrl-C to the terminal itself (run in the foreground): the shell hung up.
                                quitting = true;
                                break;
                        }
                        if (rerr.isError()) continue;
                        bool titled = false;
                        {
                                Mutex::Locker lock(_lock, WaitForever);
                                _screen.write(StringView(reinterpret_cast<const char *>(buf), n));
                                titled = _screen.titleGeneration() != _titleSeen;
                                _titleSeen = _screen.titleGeneration();
                        }
                        const uint8_t window = _window.load(std::memory_order_acquire);
                        if (window != 0 && !_gone.load(std::memory_order_acquire)) {
                                (void)_wm->update(window);
                                if (titled) {
                                        char title[96];
                                        composeTitle(title);
                                        (void)_wm->setTitle(window, StringView(title));
                                }
                        }
                }
                // The shell ends at the hangup (SIGHUP, end of input), if it hasn't.
                if (quitting) _pty->hangUp();
                _shell.join();
                // The window closes with the connection (if the window manager still runs).
                if (!_gone.load(std::memory_order_acquire)) _wm->disconnect(_client);
                _pty->release();
                return 0;
        }

        void TerminalApp::redraw(void *arg, Painter &painter) {
                static_cast<TerminalApp *>(arg)->draw(painter);
                return;
        }

        // The window closed (its close button, `win close`, the window manager stopping): the shell
        // is hung up, and ends.
        void TerminalApp::closed(void *arg) {
                auto *self = static_cast<TerminalApp *>(arg);
                self->_window.store(0, std::memory_order_release);
                self->_pty->hangUp();
                return;
        }

        void TerminalApp::gone(void *arg) {
                static_cast<TerminalApp *>(arg)->_gone.store(true, std::memory_order_release);
                return;
        }

        // The shell: `/bin/sh -i` on the pseudo-terminal, a session of its own, until it ends (exit,
        // or the hangup); then the output's end, which ends the loop in run().
        void TerminalApp::shellMain(void *arg) {
                auto       *self = static_cast<TerminalApp *>(arg);
                PtyConsole &console = self->_pty->console();
                auto [handle, err] = FileHandle::open(console, FileHandle::ReadWrite);
                if (err.isOk()) {
                        FdTable fds;
                        (void)fds.set(FdTable::Stdin, handle);
                        (void)fds.set(FdTable::Stdout, handle);
                        (void)fds.set(FdTable::Stderr, std::move(handle));
                        static constexpr StringView Args[] = {"sh", "-i"};
                        ProgramLauncher::Request    request;
                        request.path = "/bin/sh";
                        request.args = Span<const StringView>(Args, 2);
                        request.environment = self->_context.environment;
                        request.fds = &fds;
                        request.cwd = self->_context.cwd;
                        request.console = &console;
                        request.session = true;
                        // No interrupt: the terminal waits for its shell even while it ends (the
                        // hangup ends the shell), so the pty outlives it.
                        auto [status, rerr] = self->_context.launcher->run(request);
                        (void)status;
                        err = rerr;
                }
                if (err.isError()) {
                        const char *why = err.desc();
                        (void)self->_pty->console().write(StringView("terminal: can't start the shell: "), 100);
                        (void)self->_pty->console().write(StringView(why), 100);
                }
                self->_pty->programEnded();
                return;
        }

        // The screen's replies (status and size reports), back to the program as input.
        void TerminalApp::reply(void *context, StringView text) {
                auto *self = static_cast<TerminalApp *>(context);
                (void)self->_pty->write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), 0);
                return;
        }

        void TerminalApp::update() {
                const uint8_t window = _window.load(std::memory_order_acquire);
                if (window != 0 && !_gone.load(std::memory_order_acquire)) (void)_wm->update(window);
                return;
        }

        // A key, as a terminal sends it. Shift+Page Up and Down scroll the view instead; any other
        // key that sends something returns it to the bottom.
        void TerminalApp::keyTyped(void *arg, const KeyEvent &event) {
                auto *self = static_cast<TerminalApp *>(arg);
                bool  alternate = false;
                int   rows = 0;
                {
                        Mutex::Locker lock(self->_lock, WaitForever);
                        alternate = self->_screen.isAlternateScreen();
                        rows = self->_screen.rows();
                }
                const bool page = event.usage == HidKey::PageUp || event.usage == HidKey::PageDown;
                if (page && event.hasShift() && !alternate) {
                        if (event.action != KeyEvent::Action::Release)
                                self->scrollBy(event.usage == HidKey::PageUp ? rows - 1 : 1 - rows);
                        return;
                }
                if (self->send(event) > 0 && self->_offset.load(std::memory_order_relaxed) != 0)
                        self->scrollBy(-int(HistoryLines));
                return;
        }

        size_t TerminalApp::send(const KeyEvent &event) {
                bool application = false;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        application = _screen.isApplicationCursorKeys();
                }
                char         bytes[TerminalKeys::MaxBytes];
                const size_t n = TerminalKeys::encode(event, application, bytes);
                if (n > 0) (void)_pty->write(ByteSpan(reinterpret_cast<const uint8_t *>(bytes), n), 0);
                return n;
        }

        // The wheel: the view scrolled, or on the alternate screen (a full-screen program) arrows.
        void TerminalApp::wheeled(void *arg, int notches) {
                auto *self = static_cast<TerminalApp *>(arg);
                bool  alternate = false;
                {
                        Mutex::Locker lock(self->_lock, WaitForever);
                        alternate = self->_screen.isAlternateScreen();
                }
                if (!alternate) {
                        self->scrollBy(notches * WheelLines);
                        return;
                }
                KeyEvent key;
                key.usage = notches > 0 ? HidKey::Up : HidKey::Down;
                for (int i = 0; i < (notches > 0 ? notches : -notches) * WheelLines; ++i) (void)self->send(key);
                return;
        }

        void TerminalApp::scrollBy(int lines) {
                size_t history = 0;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        history = _screen.historyCount();
                }
                const int64_t want = int64_t(_offset.load(std::memory_order_relaxed)) + lines;
                const auto    to = static_cast<uint32_t>(want < 0 ? 0 : want > int64_t(history) ? history : want);
                if (to == _offset.load(std::memory_order_relaxed)) return;
                _offset.store(to, std::memory_order_relaxed);
                _all = true;
                update();
                return;
        }

        // The client area's new size: the screen to the cells that fit, the program told (SIGWINCH).
        void TerminalApp::resized(void *arg, uint16_t width, uint16_t height) {
                auto          *self = static_cast<TerminalApp *>(arg);
                const uint16_t columns =
                        static_cast<uint16_t>(width / CellWidth < MaxColumns ? width / CellWidth : MaxColumns);
                const uint16_t rows = static_cast<uint16_t>(height / CellHeight);
                if (columns == 0 || rows == 0) return;
                {
                        Mutex::Locker lock(self->_lock, WaitForever);
                        if (self->_screen.resize(columns, rows).isError()) return;
                }
                self->_pty->setSize(Console::Size{columns, rows});
                return;
        }

        // The program's title (OSC 0 or 2) and " - Terminal", or "Terminal".
        void TerminalApp::composeTitle(char (&out)[96]) {
                Mutex::Locker    lock(_lock, WaitForever);
                const StringView title = _screen.title();
                if (title.isEmpty()) {
                        std::snprintf(out, sizeof(out), "Terminal");
                } else {
                        std::snprintf(out, sizeof(out), "%.*s - Terminal", int(title.size()), title.data());
                }
                return;
        }

        // The rows that changed, those under the damage, those the cursor was drawn on and is on;
        // the margin past the cells where damaged. The screen is locked a row at a time, so output
        // goes on while rows are drawn: a row written meanwhile is dirty again, for the next round.
        void TerminalApp::draw(Painter &painter) {
                const Font          &font = Fonts::unscii16;
                const WindowPalette &palette = painter.palette();
                uint16_t             columns = 0, rows = 0;
                size_t               offset = 0;
                bool                 all = _all;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        columns = _screen.columns();
                        rows = _screen.rows();
                        // Scrolled back: the view stays on the same lines as output adds to the
                        // history, and is redrawn whole on any change.
                        const uint32_t added = _screen.historyAdded();
                        offset = _offset.load(std::memory_order_relaxed);
                        if (offset > 0) {
                                offset += added - _added;
                                if (offset > _screen.historyCount()) offset = _screen.historyCount();
                                if (added != _added || _screen.hasDamage()) all = true;
                                _offset.store(uint32_t(offset), std::memory_order_relaxed);
                        }
                        _added = added;
                }
                _all = false;
                uint32_t   damaged[(TerminalScreen::MaxSide + 31) / 32] = {};
                const Rect cellsArea{0, 0, uint16_t(columns * CellWidth), uint16_t(rows * CellHeight)};
                for (size_t i = 0; i < painter.damageCount(); ++i) {
                        const Rect d = painter.damageAt(i).intersected(cellsArea);
                        if (d.isEmpty()) continue;
                        for (uint32_t r = d.y / CellHeight; r <= (d.bottom() - 1u) / CellHeight; ++r)
                                damaged[r / 32] |= 1u << (r % 32);
                }
                for (uint16_t r = 0; r < rows; ++r) {
                        const uint32_t bit = 1u << (r % 32);
                        size_t         n = 0;
                        bool           cursorHere = false;
                        {
                                Mutex::Locker lock(_lock, WaitForever);
                                if (_screen.rows() != rows || _screen.columns() != columns) break; // resized
                                cursorHere = TerminalView::viewCursorRow(_screen, offset) == r;
                                const bool need = all || _screen.isRowDirty(r) || (damaged[r / 32] & bit) != 0 ||
                                                  (_cursorRows[r / 32] & bit) != 0 || cursorHere;
                                if (!need) continue;
                                uint16_t            length = 0;
                                const TerminalCell *line = TerminalView::viewRow(_screen, offset, r, length);
                                n = TerminalView::cells(line, length, columns, palette,
                                                        cursorHere ? _screen.cursorColumn() : -1, _cells, MaxColumns);
                                _screen.clearRowDamage(r);
                        }
                        if (cursorHere) {
                                _cursorRows[r / 32] |= bit;
                        } else {
                                _cursorRows[r / 32] &= ~bit;
                        }
                        (void)painter.cells(0, static_cast<uint16_t>(r * CellHeight), Span<const TextCell>(_cells, n),
                                            font, cellsArea.width);
                }
                // The margins right of and below the cells, where damaged.
                const uint32_t back = palette.color(WindowPalette::Role::TerminalBackground);
                for (size_t i = 0; i < painter.damageCount(); ++i) {
                        const Rect d = painter.damageAt(i);
                        if (d.right() > cellsArea.width) {
                                const uint16_t x = d.x > cellsArea.width ? d.x : cellsArea.width;
                                (void)painter.fill(Rect{x, d.y, uint16_t(d.right() - x), d.height}, back);
                        }
                        if (d.bottom() > cellsArea.height) {
                                const uint16_t y = d.y > cellsArea.height ? d.y : cellsArea.height;
                                (void)painter.fill(Rect{d.x, y, d.width, uint16_t(d.bottom() - y)}, back);
                        }
                }
                return;
        }

        // `win dump`: the screen's rows as text, the cursor's place, the history.
        void TerminalApp::dump(void *arg, Stream &out) {
                auto                 *self = static_cast<TerminalApp *>(arg);
                Mutex::Locker         lock(self->_lock, WaitForever);
                const TerminalScreen &screen = self->_screen;
                char                  text[MaxColumns * 4 + 16];
                for (uint16_t r = 0; r < screen.rows(); ++r) {
                        size_t n = static_cast<size_t>(std::snprintf(text, sizeof(text), "%2u|", r)), end = n;
                        for (uint16_t c = 0; c < screen.columns() && n + 4 < sizeof(text); ++c) {
                                const TerminalCell &cell = screen.row(r)[c];
                                if (cell.tail != 0) continue;
                                n += Utf8::encode(cell.codepoint, text + n);
                                if (cell.codepoint != ' ') end = n;
                        }
                        text[end] = '\n';
                        (void)out.write(ByteSpan(reinterpret_cast<const uint8_t *>(text), end + 1), WaitForever);
                }
                const int n = std::snprintf(
                        text, sizeof(text),
                        "cursor:   row %u, column %u%s\nhistory:  %u lines, "
                        "the view %u back\n",
                        screen.cursorRow(), screen.cursorColumn(), screen.isCursorVisible() ? "" : " (hidden)",
                        unsigned(screen.historyCount()), unsigned(self->_offset.load(std::memory_order_relaxed)));
                (void)out.write(ByteSpan(reinterpret_cast<const uint8_t *>(text), size_t(n)), WaitForever);
                return;
        }

        // `win check`: the client area as it should be, drawn from the screen by the scalar kernel.
        void TerminalApp::reference(void *arg, uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                    const WindowPalette &palette) {
                auto          *self = static_cast<TerminalApp *>(arg);
                const uint32_t back = palette.color(WindowPalette::Role::TerminalBackground);
                for (uint16_t y = 0; y < height; ++y)
                        for (uint16_t x = 0; x < width; ++x) {
                                uint8_t *p = pixels + size_t(y) * stride + size_t(x) * 3;
                                p[0] = static_cast<uint8_t>(back);
                                p[1] = static_cast<uint8_t>(back >> 8);
                                p[2] = static_cast<uint8_t>(back >> 16);
                        }
                GlyphRenderer         renderer(GlyphRenderer::Kernel::Scalar);
                Mutex::Locker         lock(self->_lock, WaitForever);
                const TerminalScreen &screen = self->_screen;
                const size_t          offset = self->_offset.load(std::memory_order_relaxed);
                for (uint16_t r = 0; r < screen.rows() && size_t(r + 1) * CellHeight <= height; ++r) {
                        const int cursor =
                                TerminalView::viewCursorRow(screen, offset) == r ? screen.cursorColumn() : -1;
                        uint16_t            length = 0;
                        const TerminalCell *line = TerminalView::viewRow(screen, offset, r, length);
                        const size_t        cells = TerminalView::cells(line, length, screen.columns(), palette, cursor,
                                                                        self->_cells, MaxColumns);
                        size_t              used = 0;
                        (void)renderer.drawCells(Span<const TextCell>(self->_cells, cells), Fonts::unscii16,
                                                 pixels + size_t(r) * CellHeight * stride, stride,
                                                 uint16_t(screen.columns() * CellWidth), used);
                }
                return;
        }

        int terminalMain(const BuiltinProgram::Context &context) {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(TerminalApp));
                if (memory == nullptr) return 1;
                auto     *app = new (memory) TerminalApp(context);
                const int status = app->run();
                app->~TerminalApp();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM(terminal, terminalMain, 16 * 1024)

BMDS_NAMESPACE_END
