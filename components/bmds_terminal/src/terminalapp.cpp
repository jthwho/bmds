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
 *   terminal [X Y] [-e COMMAND [ARG...]]
 *
 * A window of 100 x 30 cells with its top-left corner at X, Y, or where the
 * window manager cascades new windows (devplan/gui.md §4f). With -e, COMMAND
 * runs on the pty instead of the shell, found through PATH, and the window
 * closes when it ends; if it can't start, the window stays open to say why
 * (devplan/gui.md G15).
 *
 * Its colors (devplan/gui.md §4l): /boot/config/terminal.conf's [Colors],
 * then the window manager's resources terminal-text, terminal-background and
 * terminal-cursor (`win color`), which it follows while it runs.
 *
 * Copy and paste (devplan/clipboard.md C10): the left button selects (one
 * click characters, two words, three lines; Ctrl a block; Shift extends), and
 * the selection becomes the primary selection; Ctrl+Shift+C copies it to the
 * clipboard; Ctrl+Shift+V and Shift+Insert paste the clipboard, the middle
 * button the primary selection. Copies and pastes run on a thread of their own
 * (started at the first), so a long paste never holds up the window manager.
 * Programs set the clipboard with OSC 52, and read it only when
 * terminal.conf's [Clipboard] AllowRead is true (C11).
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sdkconfig.h>
#include <bmds/base64.h>
#include <bmds/builtinprograms.h>
#include <bmds/clipboard.h>
#include <bmds/clipboardreader.h>
#include <bmds/fdtable.h>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/icontheme.h>
#include <bmds/inireader.h>
#include <bmds/memory.h>
#include <bmds/menulayout.h>
#include <bmds/mutex.h>
#include <bmds/painter.h>
#include <bmds/programlauncher.h>
#include <bmds/pty.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/system.h>
#include <bmds/terminalkeys.h>
#include <bmds/terminalscreen.h>
#include <bmds/terminalselection.h>
#include <bmds/terminalview.h>
#include <bmds/thread.h>
#include <bmds/utf8.h>
#include <bmds/vfs.h>
#include <bmds/windowmanager.h>

#include "terminalicons.h"

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
        constexpr size_t      MaxConfigBytes = 4096;
        constexpr const char *ConfigPath = "/boot/config/terminal.conf";
        constexpr StringView  ResourcePrefix("terminal-"); // terminal-text: [Colors] Text
        constexpr size_t      WorkerStackBytes = 6144;     // copies and pastes
        constexpr uint64_t    MultiClickUs = 400000;       // a second press this soon on the cell: words
        constexpr unsigned    PasteTimeoutMs = 5000;       // for the owner's bytes, and the program to read
        constexpr StringView  PasteStart("\x1b[200~");     // bracketed paste (mode 2004)
        constexpr StringView  PasteEnd("\x1b[201~");

        // What the worker is asked to do (bits).
        enum Job : uint8_t {
                CopyPrimary = 1u << 0,
                CopyClipboard = 1u << 1,
                PasteClipboard = 1u << 2,
                PastePrimary = 1u << 3,
                ReplyClipboard = 1u << 4, // OSC 52 asked for it
                ReplyPrimary = 1u << 5,
                ShowMenu = 1u << 6, // a right press: the window manager's menu (devplan/files.md F9)
        };

        // The right press menu's items, in order.
        enum MenuEntry : size_t {
                MenuCopy,
                MenuPaste,
                MenuSeparator,
                MenuSelectAll
        };

        // OSC 52's targets (bits).
        enum Target : uint8_t {
                ToClipboard = 1u << 0,
                ToPrimary = 1u << 1,
        };

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
                        static void mouse(void *arg, const WindowManager::MouseEvent &event);
                        static void dump(void *arg, Stream &out);
                        static void reference(void *arg, uint8_t *pixels, size_t stride, uint16_t width,
                                              uint16_t height, const WindowPalette &palette);
                        // The connection's, and the thread that runs the shell.
                        static void gone(void *arg);
                        static void shellMain(void *arg);
                        static void reply(void *context, StringView text);
                        static void resourceChanged(void *arg, StringView name, StringView value);
                        // The copies and pastes, on the worker.
                        static void                workerMain(void *arg);
                        static const TerminalCell *selectionLine(void *arg, int64_t line, uint16_t &length);
                        static void                menuChosen(void *arg, size_t index);
                        static void                clipboardOsc(void *context, TerminalScreen::ClipboardEvent event,
                                                                StringView targets, ByteSpan data);

                        void   say(const char *text); // to stderr
                        void   draw(Painter &painter);
                        size_t send(const KeyEvent &event);
                        void   scrollBy(int lines);
                        void   composeTitle(char (&out)[96]);
                        void   update();
                        void   readConfig();
                        void                     redrawAll();
                        bool                     clipboardKey(const KeyEvent &event);
                        void                     queue(uint8_t jobs);
                        void                     stopWorker();
                        void                     copySelection(Clipboard::Selection to);
                        void                     paste(Clipboard::Selection from);
                        void                     replyClipboard(Clipboard::Selection from);
                        void                     showMenu();
                        void                     selectAll();
                        void                     setFromProgram(); // what OSC 52 set, onto the clipboard
                        TerminalSelection::Point pointAt(int x, int y, bool cell); // the lock held

                        const BuiltinProgram::Context &_context;
                        Span<const StringView>         _command; // -e's: the command and its arguments
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
                        std::atomic<bool>              _all{false};      // every row to redraw
                        uint32_t                       _cursorRows[(TerminalScreen::MaxSide + 31) / 32] = {};
                        Thread                         _shell; // runs the shell, until it ends
                        IconTheme                      _icons; // its own icon, for its window
                        TerminalColors                 _configured; // the defaults and the file's
                        TerminalColors                 _colors;     // and the resources': the window manager's lock
                        TerminalSelection              _selection;  // the lock
                        bool                     _selecting = false; // the left button held (the window manager's)
                        uint8_t                  _clicks = 0;
                        uint64_t                 _pressUs = 0;
                        TerminalSelection::Point _pressAt;
                        Thread                   _worker;
                        Semaphore                _work{1, 0};
                        std::atomic<uint8_t>     _jobs{0};
                        std::atomic<bool>        _workerStarted{false};
                        std::atomic<bool>        _quit{false};
                        std::atomic<bool>        _pasteCancel{false};     // Ctrl-C typed
                        bool                     _allowWrite = true;      // [Clipboard] AllowWrite
                        bool                     _allowRead = false;      // [Clipboard] AllowRead
                        String                   _osc52{MemSpace::Psram}; // being set (the lock)
                        uint8_t                  _osc52Targets = 0;       // Target bits; 0: dropped
                        bool                     _osc52Ready = false;     // set: to copy, unlocked
                        std::atomic<uint32_t>    _menuAt{0};              // the right press: x << 16 | y
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
                size_t positional = _context.args.size();
                for (size_t i = 1; i < _context.args.size(); ++i) {
                        if (_context.args[i] != "-e") continue;
                        positional = i;
                        _command = Span<const StringView>(_context.args.data() + i + 1, _context.args.size() - i - 1);
                        break;
                }
                if (positional == 3) {
                        rect.x = static_cast<uint16_t>(std::atoi(String::fromView(_context.args[1]).value().cStr()));
                        rect.y = static_cast<uint16_t>(std::atoi(String::fromView(_context.args[2]).value().cStr()));
                } else if (positional != 1 || (positional < _context.args.size() && _command.isEmpty())) {
                        say("usage: terminal [X Y] [-e COMMAND [ARG...]]\n");
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
                readConfig();
                _colors = _configured;
                (void)_wm->watchResources(_client, resourceChanged, this);
                for (const char *key : {"text", "background", "cursor"}) {
                        char name[24], value[WindowManager::MaxResourceValue + 1];
                        std::snprintf(name, sizeof(name), "terminal-%s", key);
                        if (_wm->resource(StringView(name), Span<char>(value, sizeof(value))).isOk())
                                (void)_colors.set(StringView(key), StringView(value));
                }
                _cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Internal, sizeof(TextCell) * MaxColumns));
                auto [pty, perr] = Pty::open(MemSpace::Psram);
                Error err = _cells == nullptr ? Error(Error::NoMem) : perr;
                if (err.isOk()) {
                        _pty = pty;
                        std::snprintf(_name, sizeof(_name), "term%u", pty->number());
                        Mutex::Locker lock(_lock, WaitForever);
                        _screen.setReplySink(reply, this);
                        _screen.setClipboardSink(clipboardOsc, this);
                        err = _screen.resize(Columns, Rows);
                        if (err.isOk()) (void)_screen.setHistory(HistoryLines); // none without the memory
                        _selection.setColumns(Columns);
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
                        config.mouse = mouse;
                        config.client = _client;
                        config.kind = "terminal";
                        config.cascade = positional == 1; // no X Y: placed by the window manager
                        config.dump = dump;
                        config.reference = reference;
                        config.arg = this;
                        auto [opened, oerr] = _wm->open(config);
                        err = oerr;
                        id = opened;
                }
                if (err.isOk()) {
                        _window.store(id, std::memory_order_release);
                        // Its own icon (built into it) on its title bar and in the window lists.
                        char path[48];
                        std::snprintf(path, sizeof(path), "/bin/%.*s", int(_context.program.size()),
                                      _context.program.data());
                        const Icon *icon = _icons.program(StringView(path), 16);
                        if (icon != nullptr) (void)_wm->setIcon(id, icon);
                        const Icon *large = _icons.program(StringView(path), 32); // Alt+Tab's
                        if (large != nullptr) (void)_wm->setLargeIcon(id, large);
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
                                // Text under the selection changed: it goes (as xterm's).
                                if (_selection.isActive()) {
                                        const int64_t added = _screen.historyAdded();
                                        for (uint16_t r = 0; r < _screen.rows(); ++r) {
                                                if (!_screen.isRowDirty(r) || !_selection.covers(added + r, added + r))
                                                        continue;
                                                _selection.clear();
                                                _all.store(true, std::memory_order_relaxed);
                                                break;
                                        }
                                }
                        }
                        if (_osc52Ready) setFromProgram();
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
                stopWorker();
                // The window closes with the connection (if the window manager still runs).
                if (!_gone.load(std::memory_order_acquire)) _wm->disconnect(_client);
                _pty->release();
                return 0;
        }

        // The colors' file: what it sets over the defaults; a line it can't use reported.
        void TerminalApp::readConfig() {
                char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxConfigBytes));
                if (text == nullptr) return;
                size_t             got = 0;
                const Vfs::Context vc{_context.cwd, nullptr};
                auto [file, err] = Vfs::open(StringView(ConfigPath), Vfs::Read, vc);
                while (err.isOk() && got < MaxConfigBytes) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxConfigBytes - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                IniReader reader(StringView(text, got));
                for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                        if (item.kind == IniReader::Kind::Group) continue;
                        const bool entry = item.kind == IniReader::Kind::Entry;
                        const bool colors = entry && item.name.equalsIgnoreCase("Colors");
                        if (entry && item.name.equalsIgnoreCase("Clipboard")) {
                                bool *to = item.key.equalsIgnoreCase("AllowWrite")  ? &_allowWrite
                                           : item.key.equalsIgnoreCase("AllowRead") ? &_allowRead
                                                                                    : nullptr;
                                if (to != nullptr && IniReader::toBool(item.value, *to).isOk()) continue;
                        } else if (entry && !colors) {
                                continue;
                        } else if (colors && _configured.set(item.key, item.value).isOk()) {
                                continue;
                        }
                        char line[112];
                        std::snprintf(line, sizeof(line), "terminal: %s: line %lu: %s\n", ConfigPath,
                                      static_cast<unsigned long>(item.line),
                                      !entry   ? "not a [group] or a key=value line"
                                      : colors ? "not a color (RRGGBB) or a color's name"
                                               : "not AllowWrite or AllowRead set to true or false");
                        say(line);
                }
                Memory::free(text);
                return;
        }

        // A resource set (`win color terminal-text RRGGBB`): one of its colors, or the file's again
        // when removed. The window manager redraws the window next.
        void TerminalApp::resourceChanged(void *arg, StringView name, StringView value) {
                auto *self = static_cast<TerminalApp *>(arg);
                if (!name.startsWith(ResourcePrefix)) return;
                const StringView key = name.mid(ResourcePrefix.size());
                uint32_t        *to = self->_colors.find(key);
                if (to == nullptr) return;
                if (value.isEmpty() || self->_colors.set(key, value).isError()) *to = *self->_configured.find(key);
                return;
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
                        String found;
                        if (!self->_command.isEmpty()) {
                                // A name without a slash: found on $PATH, as the shell would.
                                request.path = self->_command[0];
                                request.args = self->_command;
                                StringView path;
                                for (const StringView &v : self->_context.environment)
                                        if (v.startsWith(StringView("PATH="))) path = v.mid(5);
                                if (!request.path.contains('/')) {
                                        const Vfs::Context vc{self->_context.cwd, nullptr};
                                        err = Vfs::findProgram(request.path, path, vc, found);
                                        request.path = found.view();
                                }
                        }
                        request.environment = self->_context.environment;
                        request.fds = &fds;
                        request.cwd = self->_context.cwd;
                        request.console = &console;
                        request.session = true;
                        // No interrupt: the terminal waits for its shell even while it ends (the
                        // hangup ends the shell), so the pty outlives it.
                        if (err.isOk()) {
                                auto [status, rerr] = self->_context.launcher->run(request);
                                (void)status;
                                err = rerr;
                        }
                }
                if (err.isError()) {
                        PtyConsole &out = self->_pty->console();
                        if (self->_command.isEmpty()) {
                                (void)out.write(StringView("terminal: can't start the shell: "), 100);
                        } else {
                                (void)out.write(StringView("terminal: "), 100);
                                (void)out.write(self->_command[0], 100);
                                (void)out.write(StringView(": "), 100);
                        }
                        (void)out.write(StringView(err.desc()), 100);
                        (void)out.write(StringView("\r\n"), 100);
                        // Shown until the window is closed (the hangup ends the reads).
                        if (!self->_command.isEmpty()) return;
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
                if (self->clipboardKey(event)) return;
                bool alternate = false;
                int  rows = 0;
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
                if (n == 1 && bytes[0] == '\x03') _pasteCancel.store(true, std::memory_order_relaxed); // Ctrl-C
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
                _all.store(true, std::memory_order_relaxed);
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
                        self->_selection.clear(); // its places no longer hold
                        self->_selection.setColumns(columns);
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
                const TerminalColors &colors = _colors;
                uint16_t             columns = 0, rows = 0;
                size_t               offset = 0;
                bool                  all = _all.exchange(false, std::memory_order_relaxed);
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
                                uint16_t            from = 0, to = 0;
                                (void)_selection.columns(TerminalView::lineOfViewRow(_screen, offset, r), from, to);
                                n = TerminalView::cells(line, length, columns, colors,
                                                        cursorHere ? _screen.cursorColumn() : -1, _cells, MaxColumns,
                                                        from, to);
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
                const uint32_t back = colors.background;
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

        void TerminalApp::redrawAll() {
                _all.store(true, std::memory_order_relaxed);
                update();
                return;
        }

        // Where (@p x, @p y) of the client area is: a cell, or the boundary nearest (characters).
        TerminalSelection::Point TerminalApp::pointAt(int x, int y, bool cell) {
                const int rows = _screen.rows(), columns = _screen.columns();
                int       r = y < 0 ? 0 : y / CellHeight;
                if (r >= rows) r = rows - 1;
                int c = cell ? x / CellWidth : (x + CellWidth / 2) / CellWidth;
                if (c < 0) c = 0;
                if (c > (cell ? columns - 1 : columns)) c = cell ? columns - 1 : columns;
                const size_t offset = _offset.load(std::memory_order_relaxed);
                return TerminalSelection::Point{TerminalView::lineOfViewRow(_screen, offset, uint16_t(r)), uint16_t(c)};
        }

        const TerminalCell *TerminalApp::selectionLine(void *arg, int64_t line, uint16_t &length) {
                return TerminalView::line(static_cast<TerminalApp *>(arg)->_screen, line, length);
        }

        // The mouse over the cells: selecting with the left button, pasting with the middle one.
        void TerminalApp::mouse(void *arg, const WindowManager::MouseEvent &event) {
                using Type = WindowManager::MouseEvent::Type;
                auto *self = static_cast<TerminalApp *>(arg);
                if (event.type == Type::Press && event.button == PointerEvent::Middle) {
                        self->queue(PastePrimary);
                        return;
                }
                if (event.type == Type::Press && event.button == PointerEvent::Right) {
                        // The menu opens from the worker: this callback mustn't call the window manager.
                        const uint16_t x = event.x < 0 ? 0 : uint16_t(event.x), y = event.y < 0 ? 0 : uint16_t(event.y);
                        self->_menuAt.store(uint32_t(x) << 16 | y, std::memory_order_relaxed);
                        self->queue(ShowMenu);
                        return;
                }
                if (event.type == Type::Press && event.button == PointerEvent::Left) {
                        const uint64_t now = System::uptimeUs();
                        const bool     block = (event.modifiers & KeyEvent::Ctrl) != 0;
                        const bool     extend = (event.modifiers & KeyEvent::Shift) != 0;
                        {
                                Mutex::Locker                  lock(self->_lock, WaitForever);
                                const TerminalSelection::Point cell = self->pointAt(event.x, event.y, true);
                                // Clicks in a row on one cell: characters, words, lines, characters...
                                self->_clicks = now - self->_pressUs < MultiClickUs && cell == self->_pressAt
                                                        ? uint8_t(self->_clicks % 3 + 1)
                                                        : uint8_t(1);
                                self->_pressUs = now;
                                self->_pressAt = cell;
                                const auto unit = self->_clicks == 1   ? TerminalSelection::Unit::Character
                                                  : self->_clicks == 2 ? TerminalSelection::Unit::Word
                                                                       : TerminalSelection::Unit::Line;
                                const bool chars = unit == TerminalSelection::Unit::Character;
                                const TerminalSelection::Point at =
                                        chars ? self->pointAt(event.x, event.y, false) : cell;
                                if (extend && self->_selection.isActive())
                                        self->_selection.extend(at, selectionLine, self);
                                else
                                        self->_selection.begin(at, unit, block && chars, selectionLine, self);
                        }
                        self->_selecting = true;
                        self->redrawAll();
                        return;
                }
                if (!self->_selecting) return;
                if (event.type == Type::Motion && (event.buttons & PointerEvent::Left) != 0) {
                        // Past the top or the bottom: the view scrolls a line a move.
                        int rows = 0;
                        {
                                Mutex::Locker lock(self->_lock, WaitForever);
                                rows = self->_screen.rows();
                        }
                        if (event.y < 0) self->scrollBy(1);
                        if (event.y >= rows * CellHeight) self->scrollBy(-1);
                        {
                                Mutex::Locker lock(self->_lock, WaitForever);
                                const bool    chars = self->_selection.unit() == TerminalSelection::Unit::Character;
                                const TerminalSelection::Point to = self->pointAt(event.x, event.y, !chars);
                                self->_selection.extend(to, selectionLine, self);
                        }
                        self->redrawAll();
                        return;
                }
                if (event.type == Type::Release && event.button == PointerEvent::Left) {
                        self->_selecting = false;
                        bool selected = false;
                        {
                                Mutex::Locker lock(self->_lock, WaitForever);
                                selected = !self->_selection.isEmpty();
                        }
                        if (selected) self->queue(CopyPrimary); // the selection is the primary one
                }
                return;
        }

        // The terminal's own keys: Ctrl+Shift+C copies, Ctrl+Shift+V and Shift+Insert paste. They
        // send nothing to the program (they sent nothing before either).
        bool TerminalApp::clipboardKey(const KeyEvent &event) {
                constexpr uint8_t C = HidKey::A + ('c' - 'a'), V = HidKey::A + ('v' - 'a');
                const bool        ctrlShift = event.hasCtrl() && event.hasShift() && !event.hasAlt();
                uint8_t           job = 0;
                if (ctrlShift && event.usage == C) job = CopyClipboard;
                if (ctrlShift && event.usage == V) job = PasteClipboard;
                if (event.usage == HidKey::Insert && event.hasShift() && !event.hasCtrl() && !event.hasAlt())
                        job = PasteClipboard;
                if (job == 0) return false;
                if (event.action == KeyEvent::Action::Press) queue(job);
                return true;
        }

        // Asks the worker for @p jobs, starting it the first time.
        void TerminalApp::queue(uint8_t jobs) {
                _jobs.fetch_or(jobs, std::memory_order_acq_rel);
                if (!_workerStarted.exchange(true, std::memory_order_acq_rel)) {
                        Thread::Config config;
                        config.name = "termclip";
                        config.stackBytes = WorkerStackBytes;
                        config.priority = CONFIG_BMDS_SHELL_PRIORITY;
                        if (_worker.start(config, workerMain, this).isError()) {
                                _workerStarted.store(false, std::memory_order_release);
                                _jobs.store(0, std::memory_order_relaxed);
                                return;
                        }
                }
                _work.release();
                return;
        }

        void TerminalApp::stopWorker() {
                if (!_workerStarted.load(std::memory_order_acquire)) return;
                _quit.store(true, std::memory_order_release);
                _pasteCancel.store(true, std::memory_order_relaxed);
                _work.release();
                _worker.join();
                return;
        }

        void TerminalApp::workerMain(void *arg) {
                auto *self = static_cast<TerminalApp *>(arg);
                for (;;) {
                        (void)self->_work.acquire(WaitForever);
                        if (self->_quit.load(std::memory_order_acquire)) return;
                        const uint8_t jobs = self->_jobs.exchange(0, std::memory_order_acq_rel);
                        if ((jobs & CopyPrimary) != 0) self->copySelection(Clipboard::Selection::Primary);
                        if ((jobs & CopyClipboard) != 0) self->copySelection(Clipboard::Selection::Clipboard);
                        if ((jobs & PasteClipboard) != 0) self->paste(Clipboard::Selection::Clipboard);
                        if ((jobs & PastePrimary) != 0) self->paste(Clipboard::Selection::Primary);
                        if ((jobs & ReplyClipboard) != 0) self->replyClipboard(Clipboard::Selection::Clipboard);
                        if ((jobs & ReplyPrimary) != 0) self->replyClipboard(Clipboard::Selection::Primary);
                        if ((jobs & ShowMenu) != 0) self->showMenu();
                }
        }

        // The selection's text onto @p to (none: nothing).
        void TerminalApp::copySelection(Clipboard::Selection to) {
                auto [clipboard, cerr] = Clipboard::clipboard(0);
                if (cerr.isError()) return;
                struct Text {
                                String text{MemSpace::Psram};
                                size_t limit;
                };
                Text  gathered{String(MemSpace::Psram), clipboard->limit()};
                Error err;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        if (_selection.isEmpty()) return;
                        err = _selection.text(
                                selectionLine, this,
                                [](void *arg, StringView piece) {
                                        auto *t = static_cast<Text *>(arg);
                                        if (t->text.size() + piece.size() > t->limit) return Error(Error::NoSpace);
                                        return t->text.append(piece);
                                },
                                &gathered);
                }
                if (err.isError() || gathered.text.isEmpty()) return;
                const StringView types[] = {ClipboardType::Text};
                const int32_t    pid = _context.process != nullptr ? _context.process->processId() : 0;
                (void)clipboard->copy(
                        to, Span<const StringView>(types),
                        ByteSpan(reinterpret_cast<const uint8_t *>(gathered.text.data()), gathered.text.size()),
                        "terminal", pid);
                return;
        }

        // @p from's text typed into the program: LF (and CR LF) as CR, as Enter sends; inside
        // ESC [200~ and ESC [201~ when the program asked (mode 2004), without ESCs, so the text
        // can't end the paste early.
        void TerminalApp::paste(Clipboard::Selection from) {
                auto [clipboard, cerr] = Clipboard::clipboard(0);
                if (cerr.isError()) return;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipboardReader));
                if (mem == nullptr) return;
                auto *reader = ::new (mem) ClipboardReader(*clipboard);
                if (reader->open(from, ClipboardType::Text).isOk()) {
                        bool bracketed = false;
                        {
                                Mutex::Locker lock(_lock, WaitForever);
                                bracketed = _screen.isBracketedPaste();
                        }
                        if (_offset.load(std::memory_order_relaxed) != 0) scrollBy(-int(HistoryLines));
                        _pasteCancel.store(false, std::memory_order_relaxed);
                        auto put = [this](const uint8_t *data, size_t n) {
                                return n == 0 || _pty->input(ByteSpan(data, n), PasteTimeoutMs).isOk();
                        };
                        bool ok = !bracketed ||
                                  put(reinterpret_cast<const uint8_t *>(PasteStart.data()), PasteStart.size());
                        bool cr = false; // the last byte was a CR: an LF after it is dropped
                        while (ok && !_pasteCancel.load(std::memory_order_relaxed)) {
                                uint8_t in[256], out[256];
                                auto [n, rerr] = reader->read(WritableByteSpan(in, sizeof(in)), PasteTimeoutMs);
                                if (rerr.isError()) break;
                                size_t m = 0;
                                for (size_t i = 0; i < n; ++i) {
                                        const uint8_t b = in[i];
                                        const bool    lf = b == '\n';
                                        if (!(lf && cr) && !(bracketed && b == 0x1b)) out[m++] = lf ? '\r' : b;
                                        cr = b == '\r';
                                }
                                ok = put(out, m);
                        }
                        if (bracketed && ok)
                                (void)put(reinterpret_cast<const uint8_t *>(PasteEnd.data()), PasteEnd.size());
                }
                reader->~ClipboardReader();
                Memory::free(reader);
                return;
        }

        // The right press's menu: Copy (with a selection), Paste (with text to paste), Select All.
        void TerminalApp::showMenu() {
                const uint8_t window = _window.load(std::memory_order_acquire);
                if (window == 0 || _gone.load(std::memory_order_acquire)) return;
                bool selected = false;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        selected = !_selection.isEmpty();
                }
                bool pastable = false;
                auto [clipboard, cerr] = Clipboard::clipboard(0);
                if (cerr.isOk()) {
                        Clipboard::Description d;
                        clipboard->describe(Clipboard::Selection::Clipboard, d);
                        StringView offered[Clipboard::MaxTypes];
                        for (size_t i = 0; i < d.count; ++i) offered[i] = StringView(d.types[i].type);
                        const ClipboardConversions *table = clipboard->conversions();
                        pastable = d.serial != 0 && table != nullptr &&
                                   table->find(Span<const StringView>(offered, d.count), ClipboardType::Text).isOk();
                }
                const WindowManager::MenuItem items[] = {
                        {"&Copy", "Ctrl+Shift+C", uint8_t(selected ? 0 : MenuLayout::Disabled)},
                        {"&Paste", "Ctrl+Shift+V", uint8_t(pastable ? 0 : MenuLayout::Disabled)},
                        {"", "", MenuLayout::Separator},
                        {"Select &All", "", 0},
                };
                const uint32_t at = _menuAt.load(std::memory_order_relaxed);
                (void)_wm->showMenu(window, Span<const WindowManager::MenuItem>(items), uint16_t(at >> 16),
                                    uint16_t(at & 0xffff), menuChosen, this);
                return;
        }

        // A menu item chosen (on the window manager's task, its lock released).
        void TerminalApp::menuChosen(void *arg, size_t index) {
                auto *self = static_cast<TerminalApp *>(arg);
                if (index == MenuCopy) self->queue(CopyClipboard);
                if (index == MenuPaste) self->queue(PasteClipboard);
                if (index == MenuSelectAll) self->selectAll();
                return;
        }

        // Every line, the history's first to the screen's last with text (or the cursor's), as lines;
        // the primary selection then, as a drag's.
        void TerminalApp::selectAll() {
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        const int64_t added = _screen.historyAdded();
                        const int64_t first = added - int64_t(_screen.historyCount());
                        uint16_t      last = _screen.cursorRow();
                        for (uint16_t r = _screen.rows(); r-- > last;) {
                                const TerminalCell *row = _screen.row(r);
                                bool                text = false;
                                for (uint16_t c = 0; c < _screen.columns() && !text; ++c)
                                        text = row[c].codepoint != ' ' && row[c].codepoint != 0;
                                if (text) {
                                        last = r;
                                        break;
                                }
                        }
                        _selection.begin(TerminalSelection::Point{first, 0}, TerminalSelection::Unit::Line, false,
                                         selectionLine, this);
                        _selection.extend(TerminalSelection::Point{added + last, 0}, selectionLine, this);
                }
                queue(CopyPrimary);
                redrawAll();
                return;
        }

        // OSC 52 from the program, decoded as it arrives (on the output's thread, the lock held): its
        // bytes gathered up to the clipboard's limit, then copied once the lock is let go.
        void TerminalApp::clipboardOsc(void *context, TerminalScreen::ClipboardEvent event, StringView targets,
                                       ByteSpan data) {
                using Event = TerminalScreen::ClipboardEvent;
                auto   *self = static_cast<TerminalApp *>(context);
                uint8_t bits = 0;
                for (const char t : targets) {
                        if (t == 'c' || t == 's') bits |= ToClipboard; // xterm's "select" is the clipboard here
                        if (t == 'p') bits |= ToPrimary;
                }
                if (targets.isEmpty()) bits = ToClipboard; // xterm's default, "s0"
                switch (event) {
                        case Event::Begin:
                                self->_osc52.clear();
                                self->_osc52Targets = self->_allowWrite ? bits : 0;
                                break;
                        case Event::Data: {
                                if (self->_osc52Targets == 0) break;
                                auto [clipboard, err] = Clipboard::clipboard(0);
                                const size_t     limit = err.isOk() ? clipboard->limit() : 0;
                                const StringView piece(reinterpret_cast<const char *>(data.data()), data.size());
                                if (self->_osc52.size() + data.size() > limit || self->_osc52.append(piece).isError()) {
                                        self->_osc52Targets = 0; // too much: dropped, as a failed copy
                                        self->_osc52.clear();
                                }
                                break;
                        }
                        case Event::End: self->_osc52Ready = self->_osc52Targets != 0; break;
                        case Event::Cancel:
                                self->_osc52Targets = 0;
                                self->_osc52.clear();
                                break;
                        case Event::Query:
                                if (!self->_allowRead) break; // anything printed could ask: off unless allowed
                                self->queue(uint8_t(((bits & ToClipboard) != 0 ? ReplyClipboard : 0) |
                                                    ((bits & ToPrimary) != 0 ? ReplyPrimary : 0)));
                                break;
                }
                return;
        }

        // What a program set (OSC 52) onto its selections; emptied when it set nothing.
        void TerminalApp::setFromProgram() {
                String  text(MemSpace::Psram);
                uint8_t targets = 0;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        text = std::move(_osc52);
                        _osc52 = String(MemSpace::Psram);
                        targets = _osc52Targets;
                        _osc52Targets = 0;
                        _osc52Ready = false;
                }
                auto [clipboard, err] = Clipboard::clipboard(0);
                if (err.isError()) return;
                const StringView types[] = {ClipboardType::Text};
                const int32_t    pid = _context.process != nullptr ? _context.process->processId() : 0;
                const ByteSpan   bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size());
                for (const Clipboard::Selection s : {Clipboard::Selection::Clipboard, Clipboard::Selection::Primary}) {
                        const uint8_t bit = s == Clipboard::Selection::Clipboard ? ToClipboard : ToPrimary;
                        if ((targets & bit) == 0) continue;
                        if (text.isEmpty())
                                (void)clipboard->clear(s, 0);
                        else
                                (void)clipboard->copy(s, Span<const StringView>(types), bytes, "terminal", pid);
                }
                return;
        }

        // OSC 52's question answered (when allowed): `ESC ] 52 ; c ; BASE64 BEL`, the text encoded as
        // it is read, typed in as a paste is.
        void TerminalApp::replyClipboard(Clipboard::Selection from) {
                auto [clipboard, cerr] = Clipboard::clipboard(0);
                if (cerr.isError()) return;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipboardReader));
                if (mem == nullptr) return;
                auto      *reader = ::new (mem) ClipboardReader(*clipboard);
                const bool any = reader->open(from, ClipboardType::Text).isOk();
                auto       put = [this](const char *data, size_t n) {
                        return _pty->input(ByteSpan(reinterpret_cast<const uint8_t *>(data), n), PasteTimeoutMs).isOk();
                };
                bool          ok = put(from == Clipboard::Selection::Primary ? "\x1b]52;p;" : "\x1b]52;c;", 7);
                Base64Encoder encoder;
                while (ok && any) {
                        uint8_t in[192];
                        char    out[Base64Encoder::maxOutput(sizeof(in))];
                        auto [n, rerr] = reader->read(WritableByteSpan(in, sizeof(in)), PasteTimeoutMs);
                        if (rerr.isError()) break;
                        ok = put(out, encoder.put(ByteSpan(in, n), out));
                }
                char tail[Base64Encoder::MaxFinish];
                if (ok) ok = put(tail, encoder.finish(tail));
                if (ok) (void)put("\x07", 1);
                reader->~ClipboardReader();
                Memory::free(reader);
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
                                    const WindowPalette &) {
                auto          *self = static_cast<TerminalApp *>(arg);
                const uint32_t back = self->_colors.background;
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
                        uint16_t            from = 0, to = 0;
                        (void)self->_selection.columns(TerminalView::lineOfViewRow(screen, offset, r), from, to);
                        const size_t cells = TerminalView::cells(line, length, screen.columns(), self->_colors, cursor,
                                                                 self->_cells, MaxColumns, from, to);
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

BMDS_PROGRAM_ICONS(terminal, terminalMain, 16 * 1024, TerminalIcons)

BMDS_NAMESPACE_END
