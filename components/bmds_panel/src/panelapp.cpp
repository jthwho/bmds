/**
 * @file      panelapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel, /bin/panel (devplan/gui.md §4c): a strip along the top of the
 * screen with a launcher, the windows and a clock, and the desktop behind
 * every window, whose right click opens the launcher's menu. An ordinary
 * program on the GUI toolkit: its items and their settings come from a
 * configuration file, its launcher's entries from Desktop Entry files.
 *
 *   panel [-c FILE]   FILE instead of /boot/config/panel.conf
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/clock.h>
#include <bmds/desktopentry.h>
#include <bmds/fdtable.h>
#include <bmds/font.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/inireader.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/programlauncher.h>
#include <bmds/string.h>
#include <bmds/vfs.h>
#include <bmds/widget.h>

#include "panelicons.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;
        using Info = WindowManager::Info;

        constexpr const char *ConfigPath = "/boot/config/panel.conf";
        constexpr const char *DefaultApplications = "/boot/applications;/sd0/p0/share/applications";
        constexpr size_t      MaxConfigBytes = 8192;
        constexpr size_t      MaxEntryBytes = 4096;
        constexpr size_t      MaxEntries = 48;
        constexpr size_t      MaxSettings = 64;
        constexpr size_t      MaxItems = 8;
        constexpr size_t      MaxCategories = 7;
        // A row of buttons in a raised bar: the bevel, a pixel, the buttons, a pixel, the bevel.
        constexpr uint16_t MarginX = 4; // the sides: a 4-pixel column
        constexpr uint16_t MarginY = 3;
        constexpr uint16_t Height = GuiStyle::ButtonHeight + 2 * MarginY;
        constexpr uint16_t Spacing = 4;

        // The configuration used when the file can't be read: the same as the one installed
        // (boot/config/panel.conf).
        constexpr char DefaultConfig[] = "[Panel]\n"
                                         "Edge=top\n"
                                         "Items=launcher;windows;clock\n";

        // The main categories (freedesktop.org's), in the launcher's order; the rest are Other.
        constexpr const char *Categories[MaxCategories - 1] = {
                "System", "Utility", "Development", "Graphics", "AudioVideo", "Game",
        };

        struct Entry {
                        DesktopEntry entry;
                        char         file[48] = {}; // its file's name: the first of a name wins
        };

        // A key=value of the configuration (views into its text).
        struct Setting {
                        StringView group;
                        StringView key;
                        StringView value;
        };

        class PanelApp;

        // The desktop: the desktop's color, and the launcher's menu on a right click.
        class DesktopWindow final : public GuiWindow {
                public:
                        DesktopWindow(GuiApplication &app, const Config &config, PanelApp &panel)
                            : GuiWindow(app, config), _panel(panel) {}

                        void paint(Painter &painter) override {
                                (void)painter.fill(geometry(), painter.palette().color(Role::Desktop));
                                return;
                        }

                        void mousePressed(const WindowManager::MouseEvent &event) override;

                protected:
                        void drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                           const WindowPalette &palette) override {
                                const uint32_t c = palette.color(Role::Desktop);
                                for (uint16_t y = 0; y < height; ++y) {
                                        uint8_t *p = pixels + size_t(y) * stride;
                                        for (uint16_t x = 0; x < width; ++x, p += 3) {
                                                p[0] = static_cast<uint8_t>(c);
                                                p[1] = static_cast<uint8_t>(c >> 8);
                                                p[2] = static_cast<uint8_t>(c >> 16);
                                        }
                                }
                                return;
                        }

                private:
                        PanelApp &_panel;
        };

        // A window's button: they share the room equally.
        class WindowButton final : public Button {
                public:
                        using Button::Button;
                        GuiSize sizeHint() const override { return GuiSize{32, GuiStyle::ButtonHeight}; }

                        // Window @p id's icon, copied (the window's owner keeps its own); none if it has none.
                        void takeIcon(WindowManager &wm, uint8_t id) {
                                uint32_t pixels[Side * Side];
                                auto [icon, err] = wm.copyIcon(id, Span<uint32_t>(pixels));
                                const bool   has = err.isOk() && icon.width <= Side && icon.height <= Side;
                                const size_t n = has ? size_t(icon.width) * icon.height : 0;
                                if (has == _icon.isValid() &&
                                    (!has || (icon.width == _icon.width && icon.height == _icon.height &&
                                              std::memcmp(_pixels, pixels, n * 4) == 0)))
                                        return; // the same
                                if (has) {
                                        std::memcpy(_pixels, pixels, n * 4);
                                        _icon = Icon{icon.width, icon.height, _pixels};
                                } else {
                                        _icon = Icon();
                                }
                                setIcon(nullptr); // drawn again
                                setIcon(has ? &_icon : nullptr);
                                return;
                        }

                private:
                        static constexpr uint16_t Side = GuiStyle::IconSize;
                        Icon                      _icon;
                        uint32_t                  _pixels[Side * Side] = {};
        };

        // The windows, a button each, in the order they opened.
        class WindowList final : public BoxLayout {
                public:
                        WindowList(Widget *parent, PanelApp &panel)
                            : BoxLayout(parent, Direction::Horizontal), _panel(panel) {
                                setSpacing(Spacing);
                                setStretch(1);
                        }

                        void setMaxWidth(uint16_t width) { _maxWidth = width; }

                        void refresh(Span<const Info> windows);

                private:
                        static void clicked(void *arg, Button &button);
                        static void pressed(void *arg, Button &button, uint8_t which, uint16_t x, uint16_t y);

                        PanelApp &_panel;
                        uint16_t  _maxWidth = 200;
        };

        // The time, redrawn as it changes (each minute, or second if shown).
        class ClockLabel final : public Label {
                public:
                        ClockLabel(Widget *parent, GuiApplication &app) : Label(parent), _app(app) {
                                setAlign(Align::Center);
                                setFrame(Bevel::Style::Status); // sunk into the bar, as the classic trays
                        }

                        ~ClockLabel() override { _app.stopTimer(_timer); }

                        Error   configure(StringView format, StringView zone);
                        void    tick();
                        GuiSize sizeHint() const override { return GuiSize{_width, GuiStyle::ButtonHeight}; }

                private:
                        static void timeout(void *arg) {
                                static_cast<ClockLabel *>(arg)->tick();
                                return;
                        }

                        GuiApplication &_app;
                        char            _format[32] = "%a %d %b %H:%M";
                        TimeZone        _zone;
                        bool            _hasZone = false;
                        bool            _seconds = false;
                        uint16_t        _width = 0;
                        int             _timer = -1;
        };

        class PanelApp {
                public:
                        explicit PanelApp(const BuiltinProgram::Context &context) : _context(context) {}
                        ~PanelApp();

                        int run();

                        void            showLauncher(const Rect &anchor);
                        GuiApplication &application() { return _app; }

                private:
                        static void windowsChanged(void *arg);
                        static void launcherPressed(void *arg, Button &button, uint8_t which, uint16_t x, uint16_t y);
                        static void triggered(void *arg, int item);
                        static void dismissed(void *arg, Menu &menu);
                        static void launcherKey(void *arg);

                        void        say(const char *a, const char *b = "", const char *c = "", const char *d = "");
                        Error       readConfig(StringView path);
                        StringView  setting(StringView group, StringView key, StringView fallback) const;
                        void        loadEntries(StringView directories);
                        bool        loadDirectory(StringView directory);
                        void        freeLauncher();
                        void        reloadLauncher();
                        void        buildMenu(size_t submenuAfter);
                        Error       buildItems(Widget *row);
                        Error       openWindows();
                        void        launch(const Entry &entry);
                        const Icon *entryIcon(const Entry &entry);
                        Error       programPath(StringView command, String &out) const;

                        const BuiltinProgram::Context &_context;
                        GuiApplication                 _app;
                        GuiWindow                     *_panel = nullptr;
                        DesktopWindow                 *_desktop = nullptr;
                        BoxLayout                     *_row = nullptr;
                        WindowList                    *_windows = nullptr;
                        Menu                          *_menu = nullptr;
                        Button                        *_launcher = nullptr;       // pressed in while its menu is open
                        Button                        *_launcherButton = nullptr; // the launcher item's
                        char                           _applications[256] = {};   // [Launcher] Applications
                        char                           _launcherIcon[64] = {};    // its Icon
                        size_t                         _submenuAfter = 12;
                        bool                           _incomplete = false; // a directory of entries wasn't there
                        Menu                          *_submenus[MaxCategories] = {};
                        char                          *_configText = nullptr;
                        Setting                        _settings[MaxSettings];
                        size_t                         _settingCount = 0;
                        Entry                         *_entries = nullptr;
                        size_t                         _entryCount = 0;
                        Rect                           _screen;
                        bool                           _bottom = false;
        };

        // ---- Desktop, window list, clock

        void DesktopWindow::mousePressed(const WindowManager::MouseEvent &event) {
                if (event.button != PointerEvent::Right) return;
                const Rect s = screenRect();
                _panel.showLauncher(Rect{uint16_t(s.x + event.x), uint16_t(s.y + event.y), 0, 0});
                return;
        }

        void WindowList::refresh(Span<const Info> windows) {
                // Buttons of windows gone, destroyed; the others brought up to date.
                for (Widget *w = firstChild(); w != nullptr;) {
                        Widget *next = w->next();
                        auto   *b = static_cast<Button *>(w);
                        bool    found = false;
                        for (const Info &info : windows) found = found || info.id == b->data();
                        if (!found) b->destroy();
                        w = next;
                }
                for (const Info &info : windows) {
                        if (!WindowStack::isApplication(info.level)) continue;
                        Button *button = nullptr;
                        for (Widget *w = firstChild(); w != nullptr && button == nullptr; w = w->next())
                                if (static_cast<Button *>(w)->data() == info.id) button = static_cast<Button *>(w);
                        if (button == nullptr) {
                                button = Widget::create<WindowButton>(this);
                                if (button == nullptr) continue;
                                button->setData(info.id);
                                button->setStretch(1);
                                button->setMaximumWidth(_maxWidth);
                                button->setClicked(clicked, &_panel);
                                button->setPressed(pressed, &_panel);
                        }
                        char text[WindowManager::MaxTitle + 3];
                        std::snprintf(text, sizeof(text), info.minimized ? "[%s]" : "%s",
                                      info.title[0] != '\0' ? info.title : "(untitled)");
                        button->setText(StringView(text));
                        button->setChecked(info.focused);
                        static_cast<WindowButton *>(button)->takeIcon(_panel.application().windowManager(), info.id);
                }
                relayout();
                return;
        }

        // A click raises the window, or minimizes it when it has the focus.
        void WindowList::clicked(void *arg, Button &button) {
                WindowManager &wm = static_cast<PanelApp *>(arg)->application().windowManager();
                const auto     id = static_cast<uint8_t>(button.data());
                if (button.isChecked()) {
                        (void)wm.minimize(id);
                } else {
                        (void)wm.raise(id);
                }
                return;
        }

        // A right press: the window's menu, where the pointer is.
        void WindowList::pressed(void *arg, Button &button, uint8_t which, uint16_t x, uint16_t y) {
                if (which != PointerEvent::Right) return;
                WindowManager &wm = static_cast<PanelApp *>(arg)->application().windowManager();
                (void)wm.showWindowMenu(static_cast<uint8_t>(button.data()), x, y);
                return;
        }

        Error ClockLabel::configure(StringView format, StringView zone) {
                if (!format.isEmpty()) {
                        if (format.size() >= sizeof(_format)) return Error::BufferTooSmall;
                        std::memcpy(_format, format.data(), format.size());
                        _format[format.size()] = '\0';
                }
                if (!zone.isEmpty()) {
                        Result<TimeZone> rules = TimeZone::isName(zone) ? Vfs::zoneRules(zone) : TimeZone::parse(zone);
                        if (rules.error().isError()) return rules.error();
                        _zone = rules.value();
                        _hasZone = true;
                }
                const StringView f(_format);
                _seconds = f.contains(StringView("%S")) || f.contains(StringView("%T")) ||
                           f.contains(StringView("%s")) || f.contains(StringView("%r"));
                // Wide enough for any time: a sample's width and a little more.
                tick();
                _width = static_cast<uint16_t>((Fonts::unscii16.textWidth(text()) + 4 * Fonts::unscii16.width() + 3) &
                                               ~3u);
                return Error();
        }

        void ClockLabel::tick() {
                const Timestamp now = Clock::now();
                const LocalTime local = _hasZone ? _zone.localTime(now) : Clock::localTime(now);
                String          text;
                if (formatTime(text, StringView(_format), local).isOk()) setText(text.view());
                // The next change: the next second or minute's start.
                const int64_t ms = now.ns / 1'000'000;
                const int64_t period = _seconds ? 1000 : 60'000;
                const int64_t wait = period - ((ms % period) + period) % period + 5;
                _app.stopTimer(_timer);
                auto [timer, err] = _app.startTimer(static_cast<uint32_t>(wait), false, timeout, this);
                _timer = err.isOk() ? timer : -1;
                return;
        }

        // ---- The panel

        // The launcher's menus and entries, gone.
        void PanelApp::freeLauncher() {
                for (Menu *&m : _submenus) {
                        if (m == nullptr) continue;
                        m->~Menu();
                        Memory::free(m);
                        m = nullptr;
                }
                if (_menu != nullptr) {
                        _menu->~Menu();
                        Memory::free(_menu);
                        _menu = nullptr;
                }
                Memory::free(_entries);
                _entries = nullptr;
                _entryCount = 0;
                return;
        }

        // The entries read again (a directory wasn't there: the card mounted since?).
        void PanelApp::reloadLauncher() {
                freeLauncher();
                loadEntries(StringView(_applications));
                buildMenu(_submenuAfter);
                if (_launcherButton != nullptr)
                        _launcherButton->setIcon(_app.icons().find(StringView(_launcherIcon), GuiStyle::IconSize));
                return;
        }

        PanelApp::~PanelApp() {
                // The windows (and their widgets) before the application they belong to.
                freeLauncher();
                if (_desktop != nullptr) {
                        _desktop->~DesktopWindow();
                        Memory::free(_desktop);
                }
                if (_panel != nullptr) {
                        _panel->~GuiWindow();
                        Memory::free(_panel);
                }
                Memory::free(_configText);
        }

        void PanelApp::say(const char *a, const char *b, const char *c, const char *d) {
                FileHandle *err = _context.fds != nullptr ? _context.fds->handle(FdTable::Stderr) : nullptr;
                if (err == nullptr) return;
                for (const char *text : {a, b, c, d})
                        (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                         WaitForever);
                return;
        }

        // The configuration's settings, from @p path (or the built-in default without one).
        Error PanelApp::readConfig(StringView path) {
                _configText = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxConfigBytes));
                if (_configText == nullptr) return Error::NoMem;
                size_t             got = 0;
                const Vfs::Context vc{_context.cwd, nullptr};
                auto [file, err] = Vfs::open(path, Vfs::Read, vc);
                while (err.isOk() && got < MaxConfigBytes) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(_configText) + got, MaxConfigBytes - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                if (err.isError()) {
                        char p[64];
                        std::snprintf(p, sizeof(p), "%.*s", int(path.size()), path.data());
                        say("panel: ", p, ": ",
                            err == Error::NotExist ? "not found: the default layout\n"
                                                   : "can't read it: the default layout\n");
                        got = sizeof(DefaultConfig) - 1;
                        std::memcpy(_configText, DefaultConfig, got);
                }
                IniReader reader(StringView(_configText, got));
                for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                        if (item.kind == IniReader::Kind::Invalid) {
                                char line[16];
                                std::snprintf(line, sizeof(line), "%lu", static_cast<unsigned long>(item.line));
                                say("panel: line ", line, ": not a [group] or a key=value line\n");
                                continue;
                        }
                        if (item.kind != IniReader::Kind::Entry) continue;
                        if (_settingCount == MaxSettings) {
                                say("panel: too many settings: the rest left out\n");
                                break;
                        }
                        _settings[_settingCount++] = Setting{item.name, item.key, item.value};
                }
                return Error();
        }

        StringView PanelApp::setting(StringView group, StringView key, StringView fallback) const {
                for (size_t i = 0; i < _settingCount; ++i)
                        if (_settings[i].group.equalsIgnoreCase(group) && _settings[i].key == key)
                                return _settings[i].value;
                return fallback;
        }

        // Directory @p directory's entries added; false if it isn't there (no card yet: no error).
        bool PanelApp::loadDirectory(StringView directory) {
                const Vfs::Context vc{_context.cwd, nullptr};
                auto [dir, err] = Vfs::openDirectory(directory, vc);
                if (err.isError()) return false;
                char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxEntryBytes));
                if (text == nullptr) return true;
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr.isError()) break;
                        const StringView name = info.name.view();
                        if (info.type != FileType::File || !name.endsWith(StringView(".desktop")) ||
                            name.size() >= sizeof(Entry::file))
                                continue;
                        // The first directory's file of a name wins (Hidden=true there hides it).
                        bool seen = false;
                        for (size_t i = 0; i < _entryCount; ++i) seen = seen || StringView(_entries[i].file) == name;
                        if (seen || _entryCount == MaxEntries) continue;
                        String path;
                        if (path.append(directory).isError() || path.append('/').isError() ||
                            path.append(name).isError())
                                continue;
                        auto [file, oerr] = Vfs::open(path.view(), Vfs::Read, vc);
                        size_t got = 0;
                        while (oerr.isOk() && got < MaxEntryBytes) {
                                auto [n, rerr] = file->read(
                                        WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxEntryBytes - got),
                                        WaitForever);
                                if (rerr.isError() || n == 0) break;
                                got += n;
                        }
                        if (oerr.isError()) continue;
                        auto [entry, perr] = DesktopEntry::parse(StringView(text, got));
                        Entry &e = _entries[_entryCount];
                        std::memcpy(e.file, name.data(), name.size());
                        e.file[name.size()] = '\0';
                        if (perr.isError()) {
                                if (perr != Error::NotSupported)
                                        say("panel: ", path.cStr(), ": not a launcher entry\n");
                                continue;
                        }
                        e.entry = entry;
                        ++_entryCount; // a hidden one too: it hides the later ones of its name
                }
                Memory::free(text);
                return true;
        }

        void PanelApp::loadEntries(StringView directories) {
                _entries = static_cast<Entry *>(Memory::alloc(MemSpace::Psram, sizeof(Entry) * MaxEntries));
                if (_entries == nullptr) return;
                for (size_t i = 0; i < MaxEntries; ++i) ::new (&_entries[i]) Entry();
                _incomplete = false;
                for (StringView d = IniReader::nextListItem(directories); !d.isEmpty();
                     d = IniReader::nextListItem(directories))
                        if (!loadDirectory(d)) _incomplete = true; // read again when the launcher opens
                // Hidden and not-shown entries out; the rest by name.
                size_t n = 0;
                for (size_t i = 0; i < _entryCount; ++i)
                        if (!_entries[i].entry.hidden && !_entries[i].entry.noDisplay) _entries[n++] = _entries[i];
                _entryCount = n;
                for (size_t i = 1; i < _entryCount; ++i) {
                        const Entry e = _entries[i];
                        size_t      k = i;
                        for (; k > 0 && std::strcmp(_entries[k - 1].entry.name, e.entry.name) > 0; --k)
                                _entries[k] = _entries[k - 1];
                        _entries[k] = e;
                }
                // Kept in as little as they need.
                auto *kept = static_cast<Entry *>(
                        Memory::alloc(MemSpace::Psram, sizeof(Entry) * (_entryCount > 0 ? _entryCount : 1)));
                if (kept != nullptr) {
                        std::memcpy(static_cast<void *>(kept), _entries, sizeof(Entry) * _entryCount);
                        Memory::free(_entries);
                        _entries = kept;
                }
                return;
        }

        // The launcher's menu: the entries by name; past @p submenuAfter of them, in submenus
        // by their main category.
        void PanelApp::buildMenu(size_t submenuAfter) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                if (mem == nullptr) return;
                _menu = ::new (mem) Menu(_app);
                _menu->setTriggered(triggered, this);
                _menu->setDismissed(dismissed, this);
                if (_entryCount <= submenuAfter) {
                        for (size_t i = 0; i < _entryCount; ++i)
                                (void)_menu->addItem(StringView(_entries[i].entry.name), int(i), 0, StringView(),
                                                     entryIcon(_entries[i]));
                } else {
                        for (size_t c = 0; c < MaxCategories; ++c) {
                                const StringView category =
                                        c + 1 < MaxCategories ? StringView(Categories[c]) : StringView("Other");
                                Menu *sub = nullptr;
                                for (size_t i = 0; i < _entryCount; ++i) {
                                        size_t which = MaxCategories - 1;
                                        for (size_t k = 0; k + 1 < MaxCategories && which == MaxCategories - 1; ++k)
                                                if (DesktopEntry::hasCategory(_entries[i].entry.categories,
                                                                              Categories[k]))
                                                        which = k;
                                        if (which != c) continue;
                                        if (sub == nullptr) {
                                                void *m = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                                                if (m == nullptr) break;
                                                sub = ::new (m) Menu(_app);
                                                _submenus[c] = sub;
                                                (void)_menu->addSubmenu(category, *sub);
                                        }
                                        (void)sub->addItem(StringView(_entries[i].entry.name), int(i), 0, StringView(),
                                                           entryIcon(_entries[i]));
                                }
                        }
                }
                if (_menu->count() == 0) (void)_menu->addItem("(no applications)", -1, MenuLayout::Disabled);
                return;
        }

        void PanelApp::showLauncher(const Rect &anchor) {
                if (_incomplete && (_menu == nullptr || !_menu->isShown())) reloadLauncher();
                if (_menu != nullptr) (void)_menu->popup(anchor);
                return;
        }

        void PanelApp::launcherPressed(void *arg, Button &button, uint8_t which, uint16_t, uint16_t) {
                auto *self = static_cast<PanelApp *>(arg);
                if (which != PointerEvent::Left) return;
                const Rect b = button.geometry();
                const Rect s = self->_panel->screenRect();
                self->showLauncher(Rect{uint16_t(s.x + b.x), uint16_t(s.y + b.y), b.width, b.height});
                // Pressed in until the menu closes, as the classic Start buttons.
                if (self->_menu != nullptr && self->_menu->isShown()) {
                        self->_launcher = &button;
                        button.setChecked(true);
                }
                return;
        }

        // The Windows key: the launcher's menu at its button (or the screen's top-left corner), its
        // first item highlighted for the keys.
        void PanelApp::launcherKey(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_menu != nullptr && self->_menu->isShown()) {
                        self->_menu->dismiss(); // the key again closes it
                        return;
                }
                Rect anchor{self->_screen.x, self->_screen.y, 0, 0};
                if (self->_launcherButton != nullptr && self->_panel != nullptr) {
                        const Rect b = self->_launcherButton->geometry();
                        const Rect s = self->_panel->screenRect();
                        anchor = Rect{uint16_t(s.x + b.x), uint16_t(s.y + b.y), b.width, b.height};
                }
                self->showLauncher(anchor);
                if (self->_menu != nullptr && self->_menu->isShown()) {
                        self->_menu->highlightFirst();
                        if (self->_launcherButton != nullptr) {
                                self->_launcher = self->_launcherButton;
                                self->_launcher->setChecked(true);
                        }
                }
                return;
        }

        void PanelApp::dismissed(void *arg, Menu &) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_launcher != nullptr) self->_launcher->setChecked(false);
                self->_launcher = nullptr;
                return;
        }

        void PanelApp::triggered(void *arg, int item) {
                auto *self = static_cast<PanelApp *>(arg);
                if (item >= 0 && size_t(item) < self->_entryCount) self->launch(self->_entries[item]);
                return;
        }

        // Entry @p entry's command started (in a terminal if it asks), not waited for.
        void PanelApp::launch(const Entry &entry) {
                if (_context.launcher == nullptr) return;
                char         buffer[DesktopEntry::MaxExec + 1];
                StringView   args[DesktopEntry::MaxArgs + 2];
                const size_t front = entry.entry.terminal ? 2 : 0;
                auto [n, err] = DesktopEntry::splitExec(StringView(entry.entry.exec), buffer, sizeof(buffer),
                                                        Span<StringView>(args + front, DesktopEntry::MaxArgs));
                if (err.isError()) {
                        say("panel: ", entry.entry.name, ": its Exec line can't be used\n");
                        return;
                }
                if (entry.entry.terminal) {
                        args[0] = "terminal";
                        args[1] = "-e";
                }
                const size_t count = n + front;
                String       found;
                err = programPath(args[0], found);
                const StringView path = found.view();
                if (err.isOk()) {
                        ProgramLauncher::Request request;
                        request.path = path;
                        request.args = Span<const StringView>(args, count);
                        request.environment = _context.environment;
                        request.fds = _context.fds;
                        request.cwd = _context.cwd;
                        request.console = _context.console;
                        request.session = true; // not the panel's terminal's: its signals aren't the program's
                        auto [pid, serr] = _context.launcher->start(request);
                        err = serr;
                        if (err.isOk() && _context.process != nullptr) _context.process->disownChild(pid);
                }
                if (err.isError())
                        say("panel: ", entry.entry.name, ": ",
                            err == Error::NotExist ? "not found\n" : "can't start it\n");
                return;
        }

        // Where @p command (a name on $PATH, or a path) is.
        Error PanelApp::programPath(StringView command, String &out) const {
                out.clear();
                if (command.contains('/')) return out.append(command);
                StringView searchPath;
                for (const StringView &v : _context.environment)
                        if (v.startsWith(StringView("PATH="))) searchPath = v.mid(5);
                const Vfs::Context vc{_context.cwd, nullptr};
                return Vfs::findProgram(command, searchPath, vc, out);
        }

        // Entry @p entry's icon: its Icon=, else its program's own, else a generic one.
        const Icon *PanelApp::entryIcon(const Entry &entry) {
                char       buffer[DesktopEntry::MaxExec + 1];
                StringView args[DesktopEntry::MaxArgs];
                String     path;
                auto [n, err] = DesktopEntry::splitExec(StringView(entry.entry.exec), buffer, sizeof(buffer),
                                                        Span<StringView>(args));
                if (err.isOk() && n > 0) (void)programPath(args[0], path);
                return _app.icons().forProgram(StringView(entry.entry.icon), path.view(), GuiStyle::IconSize);
        }

        void PanelApp::windowsChanged(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_windows == nullptr) return;
                Info         infos[WindowStack::MaxWindows];
                const size_t n = self->_app.windowManager().list(Span<Info>(infos));
                self->_windows->refresh(Span<const Info>(infos, n));
                return;
        }

        // The items of [Panel] Items, each from its section, in @p row.
        Error PanelApp::buildItems(Widget *row) {
                StringView items = setting("Panel", "Items", "launcher;windows;clock");
                size_t     made = 0;
                for (StringView name = IniReader::nextListItem(items); !name.isEmpty() && made < MaxItems;
                     name = IniReader::nextListItem(items), ++made) {
                        char lower[16] = {};
                        for (size_t i = 0; i < name.size() && i + 1 < sizeof(lower); ++i)
                                lower[i] = StringView::toLower(name[i]);
                        const StringView type = setting(name, "Type", StringView(lower));
                        if (type == "launcher") {
                                auto *b = Widget::create<Button>(row, setting(name, "Label", "Apps"));
                                if (b == nullptr) return Error::NoMem;
                                const StringView icon = setting(name, "Icon", "start-here");
                                std::snprintf(_launcherIcon, sizeof(_launcherIcon), "%.*s", int(icon.size()),
                                              icon.data());
                                b->setIcon(_app.icons().find(StringView(_launcherIcon), GuiStyle::IconSize));
                                _launcherButton = b;
                                b->setPressed(launcherPressed, this);
                        } else if (type == "windows") {
                                _windows = Widget::create<WindowList>(row, *this);
                                if (_windows == nullptr) return Error::NoMem;
                                const StringView most = setting(name, "MaxWidth", "200");
                                char             digits[8] = {};
                                std::snprintf(digits, sizeof(digits), "%.*s", int(most.size()), most.data());
                                const int w = std::atoi(digits);
                                if (w > 0) _windows->setMaxWidth(static_cast<uint16_t>(w));
                        } else if (type == "clock") {
                                auto *c = Widget::create<ClockLabel>(row, _app);
                                if (c == nullptr) return Error::NoMem;
                                if (c->configure(setting(name, "Format", ""), setting(name, "Zone", "")).isError())
                                        say("panel: [", lower, "]: a Format or Zone that can't be used\n");
                        } else if (type == "spacer") {
                                auto *s = Widget::create<Label>(row);
                                if (s == nullptr) return Error::NoMem;
                                const StringView width = setting(name, "Width", "");
                                char             digits[8] = {};
                                std::snprintf(digits, sizeof(digits), "%.*s", int(width.size()), width.data());
                                const int w = std::atoi(digits);
                                if (w > 0) {
                                        s->setMaximumWidth(static_cast<uint16_t>(w));
                                        s->setStretch(1); // up to its width
                                } else {
                                        s->setStretch(1);
                                }
                        } else {
                                char t[24];
                                std::snprintf(t, sizeof(t), "%.*s", int(type.size()), type.data());
                                say("panel: an item of a kind not known: ", t, "\n");
                        }
                }
                return Error();
        }

        int PanelApp::run() {
                StringView config(ConfigPath);
                if (_context.args.size() == 3 && _context.args[1] == "-c") {
                        config = _context.args[2];
                } else if (_context.args.size() != 1) {
                        say("usage: panel [-c FILE]\n");
                        return 2;
                }
                Error err = _app.connect(0);
                if (err.isError()) {
                        say(err == Error::InvalidState ? "panel: the window system isn't running ('win start')\n"
                                                       : "panel: can't connect to the window manager\n");
                        return 1;
                }
                WindowManager &wm = _app.windowManager();
                {
                        Info         infos[WindowStack::MaxWindows];
                        const size_t n = wm.list(Span<Info>(infos));
                        for (size_t i = 0; i < n; ++i) {
                                if (StringView(infos[i].kind) == "panel") {
                                        say("panel: one is running already\n");
                                        return 1;
                                }
                        }
                }
                err = readConfig(config);
                if (err.isError()) {
                        say("panel: out of memory\n");
                        return 1;
                }
                _screen = wm.screen();
                _bottom = setting("Panel", "Edge", "top") == "bottom";
                // The icon theme (a directory under /sd0/p0/share/icons; flat-color unless set).
                const StringView theme = setting("Panel", "IconTheme", "");
                if (!theme.isEmpty()) _app.icons().setTheme(theme);
                const StringView apps = setting("Launcher", "Applications", DefaultApplications);
                std::snprintf(_applications, sizeof(_applications), "%.*s", int(apps.size()), apps.data());
                const StringView after = setting("Launcher", "Submenus", "12");
                char             digits[8] = {};
                std::snprintf(digits, sizeof(digits), "%.*s", int(after.size()), after.data());
                _submenuAfter = static_cast<size_t>(std::atoi(digits));
                loadEntries(StringView(_applications));
                buildMenu(_submenuAfter);

                {
                        Mutex::Locker lock(_app.lock(), WaitForever);
                        err = openWindows();
                }
                if (err.isError()) {
                        say("panel: ", err.desc(), "\n");
                        return 1;
                }
                return _app.exec();
        }

        // The desktop and the panel opened, the window list filled. The application's lock held.
        Error PanelApp::openWindows() {
                bool desktop = true;
                (void)IniReader::toBool(setting("Desktop", "Menu", "true"), desktop);
                if (desktop) {
                        GuiWindow::Config dc;
                        dc.rect = _screen;
                        dc.title = "Desktop";
                        dc.decorations = WindowFrame::None;
                        dc.level = WindowManager::Level::Desktop;
                        dc.focusable = false;
                        dc.kind = "desktop";
                        dc.reference = true; // win check compares it
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(DesktopWindow));
                        if (mem != nullptr) {
                                _desktop = ::new (mem) DesktopWindow(_app, dc, *this);
                                if (_desktop->show().isError()) say("panel: no desktop window\n");
                        }
                }
                GuiWindow::Config pc;
                pc.rect = Rect{0, static_cast<uint16_t>(_bottom ? _screen.height - Height : 0), _screen.width, Height};
                pc.title = "Panel";
                pc.decorations = WindowFrame::None;
                pc.level = WindowManager::Level::Panel;
                pc.reserve = Height;
                pc.focusable = false;
                pc.kind = "panel";
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(GuiWindow));
                if (mem == nullptr) return Error::NoMem;
                _panel = ::new (mem) GuiWindow(_app, pc);
                _row = Widget::create<BoxLayout>(_panel, BoxLayout::Direction::Horizontal);
                Error err = _row == nullptr ? Error(Error::NoMem) : buildItems(_row);
                if (err.isOk()) {
                        _row->setSpacing(Spacing);
                        _row->setMargins(MarginX, MarginY, MarginX, MarginY);
                        _row->setFrame(Bevel::Style::Window);
                        _panel->setCentral(_row);
                        err = _panel->show();
                }
                if (err.isOk()) err = _app.setWindowsChanged(windowsChanged, this);
                // The Windows keys (left and right, tapped alone) open the launcher.
                if (err.isOk()) {
                        constexpr uint8_t LeftGui = 0xe3, RightGui = 0xe7;
                        for (const uint8_t key : {LeftGui, RightGui})
                                if (_app.grabKey(key, 0, launcherKey, this).isError())
                                        say("panel: the Windows key is taken: no launcher on it\n");
                }
                if (err.isOk()) windowsChanged(this);
                // The configuration is in the widgets now.
                _settingCount = 0;
                Memory::free(_configText);
                _configText = nullptr;
                return err;
        }

        int panelMain(const BuiltinProgram::Context &context) {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(PanelApp));
                if (memory == nullptr) return 1;
                auto     *app = ::new (memory) PanelApp(context);
                const int status = app->run();
                app->~PanelApp();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM_ICONS(panel, panelMain, 16 * 1024, PanelIcons)

BMDS_NAMESPACE_END
