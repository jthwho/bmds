/**
 * @file      panelapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel, /bin/panel (devplan/gui.md §4c): a strip along the top of the
 * screen with a launcher, the windows and a clock, and the desktop behind
 * every window, with icons (§4m), whose right click opens the launcher's
 * menu. An ordinary
 * program on the GUI toolkit: its items and their settings come from the
 * system configuration's panel/... (devplan/sysconf.md §8), read when it
 * starts; its launcher's entries from Desktop Entry files; the desktop is
 * /home/Desktop. Its folder windows (devplan/filebrowser.md §4h) open on the
 * desktop's folders, from its `files` service, and from the `files` command.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/clipboard.h>
#include <bmds/clock.h>
#include <bmds/desktopentry.h>
#include <bmds/fdtable.h>
#include <bmds/fileformat.h>
#include <bmds/fileops.h>
#include <bmds/filenames.h>
#include <bmds/foldersettings.h>
#include <bmds/folderwatches.h>
#include <bmds/font.h>
#include <bmds/guiapplication.h>
#include <bmds/guiclipboard.h>
#include <bmds/guiwindow.h>
#include <bmds/inireader.h>
#include <bmds/lineedit.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/menulayout.h>
#include <bmds/messagebox.h>
#include <bmds/mimeapps.h>
#include <bmds/popupedit.h>
#include <bmds/programlauncher.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>
#include <bmds/widget.h>

#include "desktopwindow.h"
#include "fileicons.h"
#include "folderactions.h"
#include "folderwindow.h"
#include "panelentries.h"
#include "panelicons.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;
        using Info = WindowManager::Info;

        constexpr const char *DefaultDesktop = "/home/Desktop";
        constexpr const char *ConfigDir = "/home/.config";                    // the user's settings
        constexpr const char *PlacesPath = "/home/.config/desktop.conf";      // where the icons were put
        constexpr const char *UserMimeApps = "/home/.config/mimeapps.list";   // what opens files: the user's,
        constexpr const char *DefaultMimeApps = "/boot/config/mimeapps.list"; // else the system's
        constexpr const char *FoldersPath = "/home/.config/folders.conf";     // how each folder was shown

        // The panel's settings: the system configuration's (devplan/sysconf.md §8).
        BMDS_SYSCONF(panelEdge, "panel/edge", Config::Type::Enum, "top", "The screen edge the panel is on", 0, 0,
                     "top|bottom", nullptr)
        BMDS_SYSCONF(panelItems, "panel/items", Config::Type::String, "launcher;windows;clock",
                     "The panel's items in order: launcher, windows, clock, spacer (; between)", 0, 128, nullptr,
                     nullptr)
        BMDS_SYSCONF(panelIconTheme, "panel/icon-theme", Config::Type::String, "flat-color",
                     "The icon theme (a directory of /sd0/p0/share/icons)", 0, 64, nullptr, nullptr)
        BMDS_SYSCONF(panelLauncherLabel, "panel/launcher/label", Config::Type::String, "Apps",
                     "The launcher button's label", 0, 32, nullptr, nullptr)
        BMDS_SYSCONF(panelLauncherIcon, "panel/launcher/icon", Config::Type::String, "start-here",
                     "The launcher button's icon (a theme's name)", 0, 64, nullptr, nullptr)
        BMDS_SYSCONF(panelLauncherApplications, "panel/launcher/applications", Config::Type::Paths,
                     "/boot/applications;/sd0/p0/share/applications",
                     "The directories of Desktop Entry files the launcher lists", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(panelLauncherSubmenus, "panel/launcher/submenus", Config::Type::Int, "12",
                     "Entries past which the launcher groups them by category (0: never)", 0, 1000, nullptr, nullptr)
        BMDS_SYSCONF(panelWindowsMaxWidth, "panel/windows/max-width", Config::Type::Int, "200",
                     "The widest a window list button gets, in pixels", 16, 2000, nullptr, nullptr)
        BMDS_SYSCONF(panelClockFormat, "panel/clock/format", Config::Type::String, "%a %d %b %H:%M",
                     "The clock's format (strftime's %-fields)", 0, 64, nullptr, nullptr)
        BMDS_SYSCONF(panelClockZone, "panel/clock/zone", Config::Type::String, "",
                     "The clock's time zone (empty: the kernel's)", 0, 64, nullptr, nullptr)
        BMDS_SYSCONF(panelSpacerWidth, "panel/spacer/width", Config::Type::Int, "0",
                     "A spacer's width in pixels (0: what is left)", 0, 2000, nullptr, nullptr)
        BMDS_SYSCONF(panelDesktopMenu, "panel/desktop/menu", Config::Type::Bool, "true",
                     "A right click on the desktop opens the launcher's menu", 0, 0, nullptr, nullptr)
        // The folder windows' (devplan/filebrowser.md §4h, FB16).
        BMDS_SYSCONF(filesShowHidden, "files/show-hidden", Config::Type::Bool, "false",
                     "Folder windows show hidden files (a leading '.', or the hidden attribute)", 0, 0, nullptr,
                     nullptr)
        BMDS_SYSCONF(filesDefaultView, "files/default-view", Config::Type::Enum, "icons",
                     "The view of a folder not shown before", 0, 0, "icons|details", nullptr)

        constexpr StringView  DesktopSuffix(".desktop");                // an icon's file, its name before it
        constexpr size_t      MaxPlacesBytes = 4096;
        constexpr size_t      MaxFoldersBytes = 64 * 1024; // folders.conf read at most
        constexpr uint32_t    FoldersSaveMs = 1000;        // a change written after
        constexpr uint16_t    FolderWidth = 640;           // a folder window's
        constexpr uint16_t    FolderHeight = 440;
        constexpr uint16_t    FolderStep = 24;   // each window this much below and right of the last
        constexpr uint32_t    SaveDelayMs = 500; // after a drop: drawn first, then written
        constexpr size_t      MaxEntries = 48;
        constexpr uint32_t    WatchDelayMs = 200; // changes from elsewhere, taken together after
        constexpr size_t      MaxItems = 8;
        constexpr size_t      MaxCategories = 7;
        // A row of buttons in a raised bar: the bevel, a pixel, the buttons, a pixel, the bevel.
        constexpr uint16_t MarginX = 4; // the sides: a 4-pixel column
        constexpr uint16_t MarginY = 3;
        constexpr uint16_t Height = GuiStyle::ButtonHeight + 2 * MarginY;
        constexpr uint16_t Spacing = 4;

        // The main categories (freedesktop.org's), in the launcher's order; the rest are Other.
        constexpr const char *Categories[MaxCategories - 1] = {
                "System", "Utility", "Development", "Graphics", "AudioVideo", "Game",
        };

        using Entry = PanelEntry;

        // The desktop menu's own choices (the launcher's entries, its Applications submenu, are 0
        // and up: a submenu's choice is told to its root's function).
        enum DesktopChoice : int {
                ChoosePaste = -2,
                ChooseNewFolder = -3,
                ChooseNewText = -4,
                ChooseNewFile = -5,
        };

        // @p text with each & doubled, for a menu that takes & as its key's mark (gui.md §4r).
        StringView literal(StringView text, char (&out)[2 * DesktopEntry::MaxName + 1]) {
                size_t n = 0;
                for (const char c : text) {
                        if (n + 2 >= sizeof(out)) break;
                        if (c == '&') out[n++] = '&';
                        out[n++] = c;
                }
                out[n] = '\0';
                return StringView(out, n);
        }

        class PanelApp;

        // The panel's window: Tab moves through its buttons (gui.md §4r); Left and Right do too,
        // and Esc goes back to the desktop.
        class PanelWindow final : public GuiWindow {
                public:
                        using Escaped = void (*)(void *arg);

                        PanelWindow(GuiApplication &app, const Config &config, Escaped fn, void *arg)
                            : GuiWindow(app, config), _escaped(fn), _escapedArg(arg) {}

                        bool keyPressed(const KeyEvent &event) override {
                                if (event.hasCtrl() || event.hasAlt()) return false;
                                if (event.usage == HidKey::Left || event.usage == HidKey::Right)
                                        return focusNext(event.usage == HidKey::Right);
                                if (event.usage == HidKey::Escape && _escaped != nullptr) {
                                        _escaped(_escapedArg);
                                        return true;
                                }
                                return false;
                        }

                private:
                        Escaped _escaped;
                        void   *_escapedArg;
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

        class PanelApp final : public FolderWindowHost {
                public:
                        explicit PanelApp(const BuiltinProgram::Context &context)
                            : _context(context), _fileIcons(_app.icons()), _actions(_app, context, *this) {
                                _fileIcons.setAsked(iconsAsked, this);
                        }
                        ~PanelApp();

                        // The folder windows' host (devplan/filebrowser.md §4h).
                        void                 openFolder(StringView path, StringView select) override;
                        void                 folderChanged(StringView folder) override;
                        void                 cutChanged() override;
                        FolderActions       &folderActions() override { return _actions; }
                        FileIcons           &fileIcons() override { return _fileIcons; }
                        FolderWatches       &folderWatches() override { return _watches; }
                        FolderMenus         &folderMenus() override { return *_folderMenus; }
                        FolderSettings      &folderSettings() override { return _folderSettings; }
                        void                 folderSettingsChanged() override;
                        void                 folderClosed(FolderWindow &window) override;
                        bool                 showHidden() const override { return _showHidden; }
                        FolderSettings::View defaultView() const override { return _defaultView; }

                        int run();

                        void            showLauncher(const Rect &anchor);
                        GuiApplication &application() { return _app; }

                private:
                        static void windowsChanged(void *arg);
                        static void launcherPressed(void *arg, Button &button, uint8_t which, uint16_t x, uint16_t y);
                        static void triggered(void *arg, int item);
                        static void dismissed(void *arg, Menu &menu);
                        static void launcherKey(void *arg);
                        static void desktopMenu(void *arg, uint16_t x, uint16_t y);
                        static void  desktopOpened(void *arg, size_t item);
                        static void desktopMoved(void *arg);
                        static void  windowClosed(void *arg, GuiWindow &window);
                        static void  iconMenu(void *arg, size_t item, uint16_t x, uint16_t y);
                        static void  desktopChosen(void *arg, int choice);
                        static void  renameAsked(void *arg, size_t item);
                        static void  renamed(void *arg, bool accepted, StringView text);
                        static void  renameRetry(void *arg, MessageBox::Answer answer);
                        static bool  desktopKey(void *arg, const KeyEvent &event, int selected);
                        static void  desktopTabOut(void *arg, GuiWindow &window, bool forward);
                        static void  panelTabOut(void *arg, GuiWindow &window, bool forward);
                        static void  panelEscaped(void *arg);
                        void         focusDesktop();
                        void         startRename(size_t item, StringView text);
                        void         createNew(int choice);
                        static void  iconChosen(void *arg, int choice);
                        static void savePlaces(void *arg);
                        static void badEntry(void *arg, const char *path);
                        static void  filesMessage(void *arg, ByteSpan message);
                        static void  filesSettingsChanged(void *arg, SysConf::Id id);
                        static void  reapFolders(void *arg);
                        static void  iconsAsked(void *arg);
                        static void  iconsDue(void *arg);
                        static void  watchNotify(void *arg);
                        static void  watchesDue(void *arg);
                        static void  watchesTick(void *arg);
                        static void  watchChanged(void *arg, StringView path, uint8_t changes);
                        static void  saveFolders(void *arg);
                        void         readFilesSettings();
                        void         loadFolders();
                        void         closeFolders();

                        void        say(const char *a, const char *b = "", const char *c = "", const char *d = "");
                        StringView  setting(const Config::Param &param);
                        static void settingsChanged(void *arg, SysConf::Id id);
                        static void settingsDue(void *arg);
                        void        applySettings();
                        void        loadEntries(StringView directories);
                        bool        loadDesktop();
                        void        readPlaces(Span<IconGrid::Cell> wanted);
                        bool        findDesktopIcons();
                        const Icon *desktopIcon(const Entry &entry);
                        void        freeLauncher();
                        void        reloadLauncher();
                        void        buildMenu(size_t submenuAfter);
                        Error       buildItems(Widget *row);
                        Error       openWindows();
                        void        open(const Entry &entry);
                        const Icon *fileIcon(StringView type);
                        StringView  shownName(const Entry &entry) const;
                        const Icon *entryIcon(const Entry &entry, uint16_t side);

                        const BuiltinProgram::Context &_context;
                        GuiApplication                 _app;
                        PanelWindow                   *_panel = nullptr;
                        DesktopWindow                 *_desktop = nullptr;
                        char                           _desktopDirs[256] = {}; // the desktop (/home/Desktop)
                        Entry                         *_desktopEntries = nullptr;
                        size_t                         _desktopCount = 0;
                        bool                           _desktopIncomplete = false;
                        int                            _saveTimer = -1; // places to write
                        BoxLayout                     *_row = nullptr;
                        WindowList                    *_windows = nullptr;
                        Menu                          *_menu = nullptr;
                        Menu                          *_iconMenu = nullptr;    // an icon's: Open, Cut, Copy, Delete
                        Menu    *_desktopMenu = nullptr; // the desktop's: Applications, New, Paste
                        Menu    *_newMenu = nullptr;     // its New submenu
                        uint16_t _menuX = 0;             // where it was opened
                        uint16_t _menuY = 0;
                        PopupEdit                     *_nameEdit = nullptr;    // an icon's name being edited
                        size_t                         _renaming = 0;          // the icon it is
                        char                           _renameTried[sizeof(PanelEntry::file)] = {}; // to edit again
                        size_t                         _iconItem = 0;             // the icon it is for
                        Button                        *_launcher = nullptr;       // pressed in while its menu is open
                        Button                        *_launcherButton = nullptr; // the launcher item's
                        char                           _applications[256] = {};   // panel/launcher/applications
                        char                           _launcherIcon[64] = {};    // its Icon
                        size_t                         _submenuAfter = 12;
                        bool                           _incomplete = false; // a directory of entries wasn't there
                        Menu                          *_submenus[MaxCategories] = {};
                        String                         _setting; // setting()'s last value
                        char                           _theme[64] = {};          // panel/icon-theme, as applied
                        bool                           _settingsPending = false; // applySettings() posted
                        Entry                         *_entries = nullptr;
                        size_t                         _entryCount = 0;
                        Rect                           _screen;
                        bool                           _bottom = false;
                        FileIcons                      _fileIcons;
                        FolderActions                  _actions;
                        FolderWatches                  _watches{watchNotify, this}; // the views' and desktop's
                        std::atomic<bool>              _watchPosted{false};         // watchesDue() on its way
                        std::atomic<bool>              _iconsPosted{false};         // iconsDue() on its way
                        int                            _watchTimer = -1;            // the changes to take
                        bool                           _mountsTold = false;         // by this take
                        FolderSettings                 _folderSettings;
                        FolderMenus                   *_folderMenus = nullptr; // PSRAM, the first window's
                        List<FolderWindow *>           _folderWindows{MemSpace::Psram};
                        int                            _foldersTimer = -1; // folders.conf to write
                        bool                           _reapPosted = false;
                        bool                           _quietDesktop = false; // a rename's: reloaded after
                        bool                           _showHidden = false;
                        FolderSettings::View           _defaultView = FolderSettings::View::Icons;
                        unsigned                       _cascade = 0;
        };

        // ---- Desktop, window list, clock

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
                        // The title's & doubled: a button takes & as its key's mark (gui.md §4r).
                        char        text[2 * WindowManager::MaxTitle + 3];
                        size_t      n = 0;
                        const char *title = info.title[0] != '\0' ? info.title : "(untitled)";
                        if (info.minimized) text[n++] = '[';
                        for (const char *c = title; *c != '\0' && n + 3 < sizeof(text); ++c) {
                                if (*c == '&') text[n++] = '&';
                                text[n++] = *c;
                        }
                        if (info.minimized) text[n++] = ']';
                        button->setText(StringView(text, n));
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
                closeFolders();
                freeLauncher();
                if (_watchTimer >= 0) _app.stopTimer(_watchTimer);
                if (_saveTimer >= 0) {
                        _app.stopTimer(_saveTimer);
                        savePlaces(this); // a move not written yet
                }
                if (_iconMenu != nullptr) {
                        _iconMenu->~Menu();
                        Memory::free(_iconMenu);
                }
                if (_desktopMenu != nullptr) {
                        _desktopMenu->~Menu();
                        Memory::free(_desktopMenu);
                }
                if (_newMenu != nullptr) {
                        _newMenu->~Menu();
                        Memory::free(_newMenu);
                }
                if (_nameEdit != nullptr) {
                        _nameEdit->~PopupEdit();
                        Memory::free(_nameEdit);
                }
                if (_desktop != nullptr) {
                        _desktop->~DesktopWindow();
                        Memory::free(_desktop);
                }
                Memory::free(_desktopEntries);
                if (_panel != nullptr) {
                        _panel->~PanelWindow();
                        Memory::free(_panel);
                }
        }

        void PanelApp::say(const char *a, const char *b, const char *c, const char *d) {
                FileHandle *err = _context.fds != nullptr ? _context.fds->handle(FdTable::Stderr) : nullptr;
                if (err == nullptr) return;
                for (const char *text : {a, b, c, d})
                        (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                         WaitForever);
                return;
        }

        // A setting's value now (its default if it can't be read), valid until the next call.
        StringView PanelApp::setting(const Config::Param &param) {
                if (SysConf::get(param.key, _setting).isError()) return param.defaultValue;
                return _setting.view();
        }

        // An Int setting's value now.
        int32_t intSetting(const Config::Param &param) {
                const Result<int32_t> v = SysConf::getInt(param.key);
                return v.isOk() ? v.value() : Config::toInt(param.defaultValue);
        }

        void PanelApp::badEntry(void *arg, const char *path) {
                static_cast<PanelApp *>(arg)->say("panel: ", path, ": not a launcher entry\n");
                return;
        }

        void PanelApp::loadEntries(StringView directories) {
                _entries = static_cast<Entry *>(Memory::alloc(MemSpace::Psram, sizeof(Entry) * MaxEntries));
                if (_entries == nullptr) return;
                for (size_t i = 0; i < MaxEntries; ++i) ::new (&_entries[i]) Entry();
                _incomplete = false;
                const Vfs::Context vc{_context.cwd, nullptr};
                for (StringView d = IniReader::nextListItem(directories); !d.isEmpty();
                     d = IniReader::nextListItem(directories))
                        if (!readEntryDirectory(d, vc, _entries, _entryCount, MaxEntries, badEntry, this))
                                _incomplete = true; // read again when the launcher opens
                keepShownEntries(_entries, _entryCount);
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
                        for (size_t i = 0; i < _entryCount; ++i) {
                                char name[2 * DesktopEntry::MaxName + 1];
                                (void)_menu->addItem(literal(StringView(_entries[i].entry.name), name), int(i), 0,
                                                     StringView(), entryIcon(_entries[i], GuiStyle::IconSize));
                        }
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
                                        char name[2 * DesktopEntry::MaxName + 1];
                                        (void)sub->addItem(literal(StringView(_entries[i].entry.name), name), int(i), 0,
                                                           StringView(), entryIcon(_entries[i], GuiStyle::IconSize));
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

        // The left button, or Space or Enter (0: the first item highlighted for the keys).
        void PanelApp::launcherPressed(void *arg, Button &button, uint8_t which, uint16_t, uint16_t) {
                auto *self = static_cast<PanelApp *>(arg);
                if (which != PointerEvent::Left && which != 0) return;
                const Rect b = button.geometry();
                const Rect s = self->_panel->screenRect();
                self->showLauncher(Rect{uint16_t(s.x + b.x), uint16_t(s.y + b.y), b.width, b.height});
                // Pressed in until the menu closes, as the classic Start buttons.
                if (self->_menu != nullptr && self->_menu->isShown()) {
                        self->_launcher = &button;
                        button.setChecked(true);
                        if (which == 0) self->_menu->highlightFirst();
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
                if (item >= 0 && size_t(item) < self->_entryCount) self->_actions.launch(self->_entries[item].entry);
                return;
        }

        // A desktop item opened: a launcher started, a folder in a window, another file by its type.
        void PanelApp::open(const Entry &entry) {
                if (entry.isLauncher()) {
                        _actions.launch(entry.entry);
                } else if (StringView(entry.type) == FileFormat::Directory) {
                        openFolder(StringView(entry.path), StringView());
                } else {
                        _actions.openFile(StringView(entry.path), StringView(entry.type));
                }
                return;
        }

        // A file's icon (devplan/filetypes.md T10): its type's, the generic one, else `unknown`.
        const Icon *PanelApp::fileIcon(StringView type) {
                char        name[FileFormat::MaxType + 1];
                const Icon *icon =
                        _app.icons().find(FileFormat::iconName(type, Span<char>(name)), DesktopWindow::IconSide);
                if (icon == nullptr) icon = _app.icons().find(FileFormat::genericIcon(type), DesktopWindow::IconSide);
                if (icon == nullptr) icon = _app.icons().find("unknown", DesktopWindow::IconSide);
                return icon;
        }

        // An item's name on the desktop: a launcher's file without .desktop, another file's whole.
        StringView PanelApp::shownName(const Entry &entry) const {
                const StringView file(entry.file);
                return entry.isLauncher() && file.endsWith(DesktopSuffix)
                               ? file.left(file.size() - DesktopSuffix.size())
                               : file;
        }

        // Entry @p entry's icon: its Icon=, else its program's own, else a generic one.
        const Icon *PanelApp::entryIcon(const Entry &entry, uint16_t side) {
                char       buffer[DesktopEntry::MaxExec + 1];
                StringView args[DesktopEntry::MaxArgs];
                String     path;
                auto [n, err] = DesktopEntry::splitExec(StringView(entry.entry.exec), buffer, sizeof(buffer),
                                                        Span<StringView>(args));
                if (err.isOk() && n > 0) (void)_actions.programPath(args[0], path);
                return _app.icons().forProgram(StringView(entry.entry.icon), path.view(), side);
        }

        // ---- The desktop's icons

        // A right press on the desktop: Applications (the launcher's menu), New (a folder or a file,
        // devplan/files.md F10), then Paste, enabled when the clipboard holds files (F7).
        void PanelApp::desktopMenu(void *arg, uint16_t x, uint16_t y) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_incomplete && (self->_menu == nullptr || !self->_menu->isShown())) self->reloadLauncher();
                if (self->_desktopMenu == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                        if (mem == nullptr) return;
                        self->_desktopMenu = ::new (mem) Menu(self->_app);
                        self->_desktopMenu->setTriggered(desktopChosen, self);
                }
                if (self->_newMenu == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                        if (mem == nullptr) return;
                        self->_newMenu = ::new (mem) Menu(self->_app);
                        (void)self->_newMenu->addItem("&Folder", ChooseNewFolder, 0, StringView(),
                                                      self->_app.icons().find("folder", GuiStyle::IconSize));
                        (void)self->_newMenu->addItem("&Text Document", ChooseNewText, 0, StringView(),
                                                      self->_app.icons().find("text-x-generic", GuiStyle::IconSize));
                        (void)self->_newMenu->addItem("&Empty File", ChooseNewFile, 0, StringView(),
                                                      self->_app.icons().find("unknown", GuiStyle::IconSize));
                }
                self->_menuX = x;
                self->_menuY = y;
                Menu         &menu = *self->_desktopMenu;
                const bool    files = self->_actions.canPaste();
                menu.clear();
                if (self->_menu != nullptr) {
                        (void)menu.addSubmenu("&Applications", *self->_menu);
                        (void)menu.addSeparator();
                }
                (void)menu.addSubmenu("&New", *self->_newMenu);
                (void)menu.addSeparator();
                (void)menu.addItem("&Paste", ChoosePaste, files ? 0 : MenuLayout::Disabled);
                (void)menu.popup(Rect{x, y, 0, 0});
                return;
        }

        void PanelApp::desktopChosen(void *arg, int choice) {
                auto *self = static_cast<PanelApp *>(arg);
                if (choice >= 0) {
                        triggered(self, choice); // a launcher entry, from the Applications submenu
                        return;
                }
                if (choice == ChooseNewFolder || choice == ChooseNewText || choice == ChooseNewFile) {
                        self->createNew(choice);
                        return;
                }
                if (choice != ChoosePaste) return;
                // Into the desktop's folder (made if it isn't there): the desktop told when done.
                StringView       dirs(self->_desktopDirs);
                const StringView directory = IniReader::nextListItem(dirs);
                if (directory.isEmpty()) return;
                (void)Vfs::mkdir(directory, Vfs::Context{self->_context.cwd, nullptr});
                self->_actions.paste(directory);
                return;
        }

        // New > Folder, Text Document or Empty File: made in the desktop's first directory under the
        // first free name ("New Folder", then "New Folder (2)"...), put where the menu was opened
        // if that cell is free, then named in place.
        void PanelApp::createNew(int choice) {
                StringView       dirs(_desktopDirs);
                const StringView directory = IniReader::nextListItem(dirs);
                if (directory.isEmpty()) return;
                const FileOps::NewKind kind = choice == ChooseNewFolder ? FileOps::NewKind::Folder
                                              : choice == ChooseNewText ? FileOps::NewKind::TextDocument
                                                                        : FileOps::NewKind::EmptyFile;
                // Made, and the desktop read again (told by the actions).
                auto [path, err] = _actions.makeNew(directory, kind);
                if (err.isError()) return;
                char             name[sizeof(PanelEntry::file)];
                const StringView made = FileOps::baseName(path.view());
                std::snprintf(name, sizeof(name), "%.*s", int(made.size()), made.data());
                for (size_t i = 0; i < _desktopCount; ++i) {
                        if (std::strcmp(_desktopEntries[i].file, name) != 0) continue;
                        if (_desktop->placeItem(i, _menuX, _menuY)) {
                                if (_saveTimer >= 0) {
                                        _app.stopTimer(_saveTimer);
                                        _saveTimer = -1;
                                }
                                savePlaces(this);
                        }
                        _desktop->setSelected(int(i));
                        startRename(i, shownName(_desktopEntries[i]));
                        break;
                }
                return;
        }

        void PanelApp::desktopOpened(void *arg, size_t item) {
                auto *self = static_cast<PanelApp *>(arg);
                if (item < self->_desktopCount) self->open(self->_desktopEntries[item]);
                return;
        }

        // The entries of [Desktop] Icons as the desktop's items, in the work area, with the
        // icons found; false if a directory or an icon wasn't (the card, mounted later).
        bool PanelApp::loadDesktop() {
                if (_desktop == nullptr) return true;
                // The selection kept by its file's name.
                char      selectedFile[sizeof(PanelEntry::file)] = {};
                const int was = _desktop->selected();
                if (was >= 0 && size_t(was) < _desktopCount)
                        std::memcpy(selectedFile, _desktopEntries[was].file, sizeof(selectedFile));
                Memory::free(_desktopEntries);
                _desktopEntries = nullptr;
                _desktopCount = 0;
                _desktopIncomplete = false;
                // As many entries as the directories hold (no limit: devplan/filebrowser.md §4g).
                const Vfs::Context vc{_context.cwd, nullptr};
                size_t             room = 0;
                StringView         dirs(_desktopDirs);
                for (StringView d = IniReader::nextListItem(dirs); !d.isEmpty(); d = IniReader::nextListItem(dirs)) {
                        auto [dir, derr] = Vfs::openDirectory(d, vc);
                        while (derr.isOk() && dir.next().isOk()) ++room;
                }
                if (room == 0) room = 1;
                _desktopEntries = static_cast<Entry *>(Memory::alloc(MemSpace::Psram, sizeof(Entry) * room));
                if (_desktopEntries == nullptr) return true;
                for (size_t i = 0; i < room; ++i) ::new (&_desktopEntries[i]) Entry();
                dirs = StringView(_desktopDirs);
                for (StringView d = IniReader::nextListItem(dirs); !d.isEmpty(); d = IniReader::nextListItem(dirs))
                        if (!readEntryDirectory(d, vc, _desktopEntries, _desktopCount, room, badEntry, this, true))
                                _desktopIncomplete = true;
                keepShownEntries(_desktopEntries, _desktopCount);
                List<StringView>     names(MemSpace::Psram);
                List<IconGrid::Cell> wanted(MemSpace::Psram);
                if (names.reserve(_desktopCount).isError() || wanted.reserve(_desktopCount).isError()) {
                        say("panel: no memory for the desktop's icons\n");
                        return true;
                }
                // An icon is named by its file (devplan/gui.md §4q): copies and renames stay in step.
                for (size_t i = 0; i < _desktopCount; ++i) {
                        (void)names.append(shownName(_desktopEntries[i]));
                        (void)wanted.append(IconGrid::Cell());
                }
                const Rect work = _app.windowManager().workArea();
                const Rect s = _desktop->screenRect();
                const Rect area{uint16_t(work.x - s.x), uint16_t(work.y - s.y), work.width, work.height};
                readPlaces(wanted.span());
                if (_desktop->setItems(names.span(), wanted.span(), area).isError())
                        say("panel: no memory for the desktop's icons\n");
                for (size_t i = 0; i < _desktopCount && selectedFile[0] != '\0'; ++i)
                        if (std::strcmp(_desktopEntries[i].file, selectedFile) == 0) _desktop->setSelected(int(i));
                // A name being edited stays hidden under its editor (the icons looked for again
                // reload the desktop while it is open).
                if (_nameEdit != nullptr && _nameEdit->isShown() && _renaming < _desktopCount)
                        _desktop->setEditing(int(_renaming));
                return findDesktopIcons();
        }

        // Where the desktop's entries were put (desktop.conf's [Icons], FILE=COLUMN,ROW), in @p wanted.
        void PanelApp::readPlaces(Span<IconGrid::Cell> wanted) {
                for (IconGrid::Cell &c : wanted) c = IconGrid::Cell();
                char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxPlacesBytes));
                if (text == nullptr) return;
                size_t             got = 0;
                const Vfs::Context vc{_context.cwd, nullptr};
                auto [file, err] = Vfs::open(StringView(PlacesPath), Vfs::Read, vc);
                while (err.isOk() && got < MaxPlacesBytes) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxPlacesBytes - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                IniReader reader(StringView(text, got));
                for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                        if (item.kind != IniReader::Kind::Entry || !item.name.equalsIgnoreCase("Icons")) continue;
                        const size_t comma = item.value.indexOf(',');
                        if (comma == StringView::NPos) continue;
                        char column[8] = {}, row[8] = {};
                        std::snprintf(column, sizeof(column), "%.*s", int(comma), item.value.data());
                        std::snprintf(row, sizeof(row), "%.*s", int(item.value.size() - comma - 1),
                                      item.value.data() + comma + 1);
                        const int c = std::atoi(column), r = std::atoi(row);
                        if (c < 0 || r < 0 || c >= IconGrid::None || r >= IconGrid::None) continue;
                        for (size_t i = 0; i < wanted.size(); ++i)
                                if (StringView(_desktopEntries[i].file) == item.key)
                                        wanted[i] = IconGrid::Cell{uint16_t(c), uint16_t(r)};
                }
                Memory::free(text);
                return;
        }

        // An icon moved: the places written soon (once the drop is drawn).
        // A right press on a desktop icon: its menu (made the first time).
        void PanelApp::iconMenu(void *arg, size_t item, uint16_t x, uint16_t y) {
                auto *self = static_cast<PanelApp *>(arg);
                if (item >= self->_desktopCount) return;
                if (self->_iconMenu == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                        if (mem == nullptr) return;
                        self->_iconMenu = ::new (mem) Menu(self->_app);
                        self->_iconMenu->setTriggered(iconChosen, self);
                        (void)self->_iconMenu->addItem("&Open", 0);
                        (void)self->_iconMenu->addItem("Cu&t", 4);
                        (void)self->_iconMenu->addItem("&Copy", 1);
                        (void)self->_iconMenu->addItem("Re&name", 3);
                        (void)self->_iconMenu->addSeparator();
                        (void)self->_iconMenu->addItem("&Delete", 2);
                }
                self->_iconItem = item;
                (void)self->_iconMenu->popup(Rect{x, y, 0, 0});
                return;
        }

        // Open starts it; Cut and Copy put its file on the clipboard (devplan/clipboard.md §4f,
        // filebrowser.md §4j), rendered when asked for; Delete trashes it, 5 (Shift+Delete) deletes it
        // for good once asked.
        void PanelApp::iconChosen(void *arg, int choice) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_iconItem >= self->_desktopCount) return;
                if (choice == 3) {
                        renameAsked(self, self->_iconItem);
                        return;
                }
                // Open, Cut, Copy and Delete act on every icon selected (the one asked for is).
                if (choice == 0) {
                        for (int i = self->_desktop->nextSelected(0); i >= 0;
                             i = self->_desktop->nextSelected(size_t(i) + 1))
                                if (size_t(i) < self->_desktopCount) self->open(self->_desktopEntries[i]);
                        return;
                }
                // The others: the selected icons' paths, a line each.
                String paths(MemSpace::Psram);
                for (int i = self->_desktop->nextSelected(0); i >= 0; i = self->_desktop->nextSelected(size_t(i) + 1)) {
                        const PanelEntry &e = self->_desktopEntries[i];
                        if (e.path[0] == '\0') continue;
                        if (!paths.isEmpty()) (void)paths.append('\n');
                        (void)paths.append(StringView(e.path));
                }
                if (paths.isEmpty()) return;
                if (choice == 2) {
                        // To their filesystems' trash (devplan/files.md F8): `trash -r` brings them back.
                        self->_actions.trash(paths.view());
                } else if (choice == 1) {
                        self->_actions.copy(paths.view());
                } else if (choice == 4) {
                        self->_actions.cut(paths.view());
                } else if (choice == 5) {
                        self->_actions.removeForGood(paths.view());
                }
                return;
        }

        // An icon to rename (its menu, a slow second click, a long press): its name edited in place.
        void PanelApp::renameAsked(void *arg, size_t item) {
                auto *self = static_cast<PanelApp *>(arg);
                if (item >= self->_desktopCount) return;
                self->startRename(item, self->shownName(self->_desktopEntries[item]));
                return;
        }

        // The editor over icon @p item's name, @p text in it: drawn as the name is, in its place,
        // the name hidden under it (devplan/gui.md §4r).
        void PanelApp::startRename(size_t item, StringView text) {
                if (_desktop == nullptr) return;
                if (_nameEdit == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(PopupEdit));
                        if (mem == nullptr) return;
                        _nameEdit = ::new (mem) PopupEdit(_app);
                }
                _renaming = item;
                if (_nameEdit->open(_desktop->nameRect(item), text, PopupEdit::Look::IconName, renamed, this)
                            .isError()) {
                        say("panel: can't edit the name\n");
                        return;
                }
                // A file's extension stays: only the name before it selected (devplan/filetypes.md T10).
                const Entry &e = _desktopEntries[item];
                LineEdit    *edit = _nameEdit->edit();
                edit->setMaxLength(sizeof(PanelEntry::file) - 1 - (e.isLauncher() ? DesktopSuffix.size() : 0));
                const size_t dot = text.lastIndexOf('.');
                if (!e.isLauncher() && dot != StringView::NPos && dot > 0) edit->setSelection(0, dot);
                _desktop->setEditing(int(item));
                return;
        }

        // The name edited: checked (a message, then editing again, if it can't be), then the file
        // renamed, its place on the grid kept.
        void PanelApp::renamed(void *arg, bool accepted, StringView text) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_desktop != nullptr) self->_desktop->setEditing(-1);
                if (!accepted || self->_renaming >= self->_desktopCount) return;
                PanelEntry      &e = self->_desktopEntries[self->_renaming];
                const StringView suffix = e.isLauncher() ? DesktopSuffix : StringView(); // a launcher's .desktop kept
                const StringView name = text.trimmed();
                String           to(MemSpace::Psram);
                char             message[200];
                // The desktop read again below, once its place is written under the new name.
                self->_quietDesktop = true;
                const bool done = self->_actions.rename(StringView(e.path), name, suffix, sizeof(e.file) - 1, to,
                                                        Span<char>(message, sizeof(message)));
                self->_quietDesktop = false;
                const char *problem = !done && message[0] != '\0' ? message : nullptr;
                if (!done && problem == nullptr) return; // the same name
                if (problem != nullptr) {
                        // Told, then back to editing what was typed.
                        const size_t n = name.size() < sizeof(self->_renameTried) - 1 ? name.size()
                                                                                      : sizeof(self->_renameTried) - 1;
                        std::memcpy(self->_renameTried, name.data(), n);
                        self->_renameTried[n] = '\0';
                        if (MessageBox::show(self->_app, MessageBox::Kind::Error, "Rename", StringView(problem),
                                             MessageBox::Buttons::Ok, renameRetry, self)
                                    .isError())
                                self->say("panel: ", problem, "\n");
                        return;
                }
                // The entry renamed in place, its place written under its new name, then shown again.
                const StringView newPath = to.view();
                const StringView newFile = newPath.mid(newPath.lastIndexOf('/') + 1);
                std::memcpy(e.file, newFile.data(), newFile.size());
                e.file[newFile.size()] = '\0';
                if (newPath.size() < sizeof(e.path)) {
                        std::memcpy(e.path, newPath.data(), newPath.size());
                        e.path[newPath.size()] = '\0';
                }
                if (self->_saveTimer >= 0) {
                        self->_app.stopTimer(self->_saveTimer);
                        self->_saveTimer = -1;
                }
                savePlaces(self);
                (void)self->loadDesktop();
                return;
        }

        void PanelApp::renameRetry(void *arg, MessageBox::Answer answer) {
                (void)answer;
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_renaming < self->_desktopCount)
                        self->startRename(self->_renaming, StringView(self->_renameTried));
                return;
        }

        // A key the desktop leaves (gui.md §4r): Delete trashes the selected icons (Shift+Delete
        // deletes them for good, asking), Ctrl+X cuts them, Ctrl+C copies them, Ctrl+V pastes.
        bool PanelApp::desktopKey(void *arg, const KeyEvent &event, int selected) {
                auto             *self = static_cast<PanelApp *>(arg);
                constexpr uint8_t C = HidKey::A + ('c' - 'a'), V = HidKey::A + ('v' - 'a'), X = HidKey::A + ('x' - 'a');
                const bool        ctrl = event.hasCtrl() && !event.hasAlt();
                int               choice = -1;
                if (event.usage == HidKey::Delete && !event.hasCtrl() && !event.hasAlt())
                        choice = event.hasShift() ? 5 : 2;
                if (ctrl && event.usage == C) choice = 1;
                if (ctrl && event.usage == X) choice = 4;
                if (choice >= 0) {
                        if (selected < 0) return true;
                        self->_iconItem = size_t(selected);
                        iconChosen(self, choice);
                        return true;
                }
                if (ctrl && event.usage == V) {
                        desktopChosen(self, ChoosePaste);
                        return true;
                }
                return false;
        }

        // The desktop's keys, its selection (or its first icon) shown.
        void PanelApp::focusDesktop() {
                if (_desktop == nullptr) return;
                if (_desktop->selected() < 0 && _desktop->itemCount() > 0) _desktop->setSelected(0);
                _desktop->activate();
                return;
        }

        // Tab past the desktop: the panel's first button, Shift+Tab its last.
        void PanelApp::desktopTabOut(void *arg, GuiWindow &, bool forward) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_panel == nullptr) return;
                (void)self->_panel->focusFirst(!forward);
                self->_panel->activate();
                return;
        }

        // Tab past the panel's ends: the desktop.
        void PanelApp::panelTabOut(void *arg, GuiWindow &, bool) {
                static_cast<PanelApp *>(arg)->focusDesktop();
                return;
        }

        void PanelApp::panelEscaped(void *arg) {
                static_cast<PanelApp *>(arg)->focusDesktop();
                return;
        }

        // The window manager closed the panel or the desktop (`win close`): the panel ends, the other
        // window closing with it, rather than run on half there.
        void PanelApp::windowClosed(void *arg, GuiWindow &window) {
                (void)window;
                static_cast<PanelApp *>(arg)->_app.quit(0);
                return;
        }

        void PanelApp::desktopMoved(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_saveTimer >= 0) return;
                auto [timer, err] = self->_app.startTimer(SaveDelayMs, false, savePlaces, self);
                if (err.isOk()) self->_saveTimer = timer;
                return;
        }

        // Every icon's place, into desktop.conf.
        void PanelApp::savePlaces(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                self->_saveTimer = -1;
                if (self->_desktop == nullptr) return;
                String text;
                Error  err = text.append(StringView("# Where the desktop's icons were put: written by the panel.\n"
                                                     "[Icons]\n"));
                for (size_t i = 0; i < self->_desktop->itemCount() && err.isOk(); ++i) {
                        const IconGrid::Cell c = self->_desktop->itemCell(i);
                        char                 line[80];
                        std::snprintf(line, sizeof(line), "%s=%u,%u\n", self->_desktopEntries[i].file,
                                      unsigned(c.column), unsigned(c.row));
                        err = text.append(StringView(line));
                }
                const Vfs::Context vc{self->_context.cwd, nullptr};
                if (err.isOk()) {
                        (void)Vfs::mkdir(StringView(ConfigDir), vc); // the user's settings' directory, once
                        auto [file, oerr] =
                                Vfs::open(StringView(PlacesPath), Vfs::Write | Vfs::Create | Vfs::Truncate, vc);
                        err = oerr;
                        if (err.isOk()) {
                                auto [n, werr] = file->write(
                                        ByteSpan(reinterpret_cast<const uint8_t *>(text.cStr()), text.size()),
                                        WaitForever);
                                err = werr.isError() ? werr : (n != text.size() ? Error(Error::IoError) : Error());
                        }
                }
                if (err.isError()) {
                        char line[96];
                        std::snprintf(line, sizeof(line), "panel: %s: can't write it: %s\n", PlacesPath, err.desc());
                        self->say(line);
                }
                return;
        }

        // The icons the desktop's items lack, looked for; true when none is missing.
        bool PanelApp::findDesktopIcons() {
                bool all = !_desktopIncomplete;
                for (size_t i = 0; i < _desktop->itemCount(); ++i) {
                        if (_desktop->hasItemIcon(i)) continue;
                        const Entry &e = _desktopEntries[i];
                        const Icon  *icon = desktopIcon(e);
                        if (icon != nullptr) {
                                // A copy (the theme's cache may drop it), dimmed if cut.
                                _desktop->setItemIcon(i, icon, _actions.copied().isCut(StringView(e.path)));
                        } else {
                                all = false;
                        }
                }
                return all;
        }

        const Icon *PanelApp::desktopIcon(const Entry &entry) {
                return entry.isLauncher() ? entryIcon(entry, DesktopWindow::IconSide)
                                          : fileIcon(StringView(entry.type));
        }

        // The files cut changed: the windows redraw their icons; the desktop's are copied again,
        // dimmed or not (devplan/filebrowser.md §4j).
        void PanelApp::cutChanged() {
                for (FolderWindow *w : _folderWindows) w->redrawIcons();
                if (_desktop == nullptr) return;
                for (size_t i = 0; i < _desktop->itemCount() && i < _desktopCount; ++i) {
                        const Entry &e = _desktopEntries[i];
                        const bool   dim = _actions.copied().isCut(StringView(e.path));
                        if (!_desktop->hasItemIcon(i) || dim == _desktop->isItemDimmed(i)) continue;
                        const Icon *icon = desktopIcon(e);
                        if (icon != nullptr) _desktop->setItemIcon(i, icon, dim);
                }
                return;
        }

        void PanelApp::windowsChanged(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_windows == nullptr) return;
                // Without the memory for the list, as it was (the next change tries again).
                auto [infos, err] = self->_app.windowManager().windows();
                if (err.isOk()) self->_windows->refresh(infos.span());
                return;
        }

        // The items of panel/items (launcher, windows, clock, spacer), in order, in @p row.
        Error PanelApp::buildItems(Widget *row) {
                char             list[128];
                const StringView value = setting(bmdsConf_panelItems);
                std::snprintf(list, sizeof(list), "%.*s", int(value.size()), value.data());
                StringView items(list);
                size_t     made = 0;
                for (StringView name = IniReader::nextListItem(items); !name.isEmpty() && made < MaxItems;
                     name = IniReader::nextListItem(items), ++made) {
                        char lower[16] = {};
                        for (size_t i = 0; i < name.size() && i + 1 < sizeof(lower); ++i)
                                lower[i] = StringView::toLower(name[i]);
                        const StringView type(lower);
                        if (type == "launcher") {
                                auto *b = Widget::create<Button>(row, setting(bmdsConf_panelLauncherLabel));
                                if (b == nullptr) return Error::NoMem;
                                const StringView icon = setting(bmdsConf_panelLauncherIcon);
                                std::snprintf(_launcherIcon, sizeof(_launcherIcon), "%.*s", int(icon.size()),
                                              icon.data());
                                b->setIcon(_app.icons().find(StringView(_launcherIcon), GuiStyle::IconSize));
                                _launcherButton = b;
                                b->setPressed(launcherPressed, this);
                        } else if (type == "windows") {
                                _windows = Widget::create<WindowList>(row, *this);
                                if (_windows == nullptr) return Error::NoMem;
                                const int32_t w = intSetting(bmdsConf_panelWindowsMaxWidth);
                                if (w > 0) _windows->setMaxWidth(static_cast<uint16_t>(w));
                        } else if (type == "clock") {
                                auto *c = Widget::create<ClockLabel>(row, _app);
                                if (c == nullptr) return Error::NoMem;
                                char             format[64];
                                const StringView f = setting(bmdsConf_panelClockFormat);
                                std::snprintf(format, sizeof(format), "%.*s", int(f.size()), f.data());
                                if (c->configure(StringView(format), setting(bmdsConf_panelClockZone)).isError())
                                        say("panel: panel/clock/format or panel/clock/zone can't be used\n");
                        } else if (type == "spacer") {
                                auto *s = Widget::create<Label>(row);
                                if (s == nullptr) return Error::NoMem;
                                const int32_t w = intSetting(bmdsConf_panelSpacerWidth);
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
                if (_context.args.size() != 1) {
                        say("usage: panel (its settings are sysconf's panel/...)\n");
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
                        auto [infos, lerr] = wm.windows();
                        if (lerr.isError()) {
                                say("panel: ", lerr.desc(), "\n");
                                return 1;
                        }
                        for (const Info &info : infos) {
                                if (StringView(info.kind) == "panel") {
                                        say("panel: one is running already\n");
                                        return 1;
                                }
                        }
                }
                _screen = wm.screen();
                _bottom = setting(bmdsConf_panelEdge) == "bottom";
                // The icon theme (a directory under /sd0/p0/share/icons).
                const StringView theme = setting(bmdsConf_panelIconTheme);
                std::snprintf(_theme, sizeof(_theme), "%.*s", int(theme.size()), theme.data());
                if (_theme[0] != '\0') _app.icons().setTheme(StringView(_theme));
                const StringView apps = setting(bmdsConf_panelLauncherApplications);
                std::snprintf(_applications, sizeof(_applications), "%.*s", int(apps.size()), apps.data());
                _submenuAfter = static_cast<size_t>(intSetting(bmdsConf_panelLauncherSubmenus));
                loadEntries(StringView(_applications));
                buildMenu(_submenuAfter);
                _actions.setApplications(StringView(_applications));
                readFilesSettings();
                loadFolders();

                {
                        Mutex::Locker lock(_app.lock(), WaitForever);
                        err = openWindows();
                }
                if (err.isError()) {
                        say("panel: ", err.desc(), "\n");
                        return 1;
                }
                // Its settings followed while it runs (devplan/sysconf.md §12.3).
                if (_app.watchConfig("panel", settingsChanged, this).isError())
                        say("panel: its settings take effect when it starts again\n");
                if (_app.watchConfig("files", filesSettingsChanged, this).isError())
                        say("panel: files/... take effect when it starts again\n");
                // `files` opens folder windows here (devplan/filebrowser.md FB6).
                if (_app.registerService("files", filesMessage, this).isError())
                        say("panel: the files service is taken: 'files' won't open windows here\n");
                return _app.exec();
        }

        // The desktop and the panel opened, the window list filled. The application's lock held.
        Error PanelApp::openWindows() {
                const Result<bool> asked = SysConf::getBool(bmdsConf_panelDesktopMenu.key);
                const bool         menu = asked.isOk() ? asked.value() : true;
                // The desktop is the user's (/home/Desktop); what opens files the user's, else the system's.
                const StringView icons(DefaultDesktop);
                std::snprintf(_desktopDirs, sizeof(_desktopDirs), "%.*s", int(icons.size()), icons.data());
                const Vfs::Context vc{_context.cwd, nullptr};
                const StringView   mimeApps = Vfs::stat(StringView(UserMimeApps), vc).isOk()
                                                      ? StringView(UserMimeApps)
                                                      : StringView(DefaultMimeApps);
                _actions.setMimeApps(mimeApps);
                if (menu || !icons.isEmpty()) {
                        GuiWindow::Config dc;
                        dc.rect = _screen;
                        dc.title = "Desktop";
                        dc.decorations = WindowFrame::None;
                        dc.level = WindowManager::Level::Desktop; // the keys when pressed (§4r)
                        dc.kind = "desktop";
                        dc.reference = true; // win check compares it
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(DesktopWindow));
                        if (mem != nullptr) {
                                _desktop = ::new (mem) DesktopWindow(_app, dc);
                                if (menu) _desktop->setMenuAsked(desktopMenu, this);
                                _desktop->setOpened(desktopOpened, this);
                                _desktop->setMoved(desktopMoved, this);
                                _desktop->setClosed(windowClosed, this);
                                _desktop->setItemMenuAsked(iconMenu, this);
                                _desktop->setRenameAsked(renameAsked, this);
                                _desktop->setKeyTyped(desktopKey, this);
                                _desktop->setTabOut(desktopTabOut, this);
                                if (_desktop->show().isError()) say("panel: no desktop window\n");
                        }
                }
                GuiWindow::Config pc;
                pc.rect = Rect{0, static_cast<uint16_t>(_bottom ? _screen.height - Height : 0), _screen.width, Height};
                pc.title = "Panel";
                pc.decorations = WindowFrame::None;
                pc.level = WindowManager::Level::Panel;
                pc.reserve = Height;
                pc.focusOnPress = false; // the keys only by Tab: a press on a window's button keeps its focus
                pc.kind = "panel";
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(PanelWindow));
                if (mem == nullptr) return Error::NoMem;
                _panel = ::new (mem) PanelWindow(_app, pc, panelEscaped, this);
                _panel->setClosed(windowClosed, this);
                _panel->setTabOut(panelTabOut, this);
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
                // The desktop's icons, in the work area the panel left. Its directories are watched:
                // changes from elsewhere read it again, and a card mounted later (devplan/filebrowser.md §4i).
                if (err.isOk()) {
                        StringView dirs(_desktopDirs);
                        for (StringView d = IniReader::nextListItem(dirs); !d.isEmpty();
                             d = IniReader::nextListItem(dirs))
                                (void)_watches.add(d);
                        (void)loadDesktop();
                }
                return err;
        }

        // A panel/... parameter changed (the loop's thread): the settings applied once for the changes
        // that came together, after them.
        void PanelApp::settingsChanged(void *arg, SysConf::Id) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_settingsPending) return;
                self->_settingsPending = self->_app.post(settingsDue, self).isOk();
                if (!self->_settingsPending) self->applySettings(); // the queue is full: now
                return;
        }

        void PanelApp::settingsDue(void *arg) {
                static_cast<PanelApp *>(arg)->applySettings();
                return;
        }

        // Every setting applied again: the edge, the icon theme, the launcher's entries, the row of
        // items and the desktop's menu. Rare, so all of it rather than by key.
        void PanelApp::applySettings() {
                _settingsPending = false;
                // Menus open on the old settings close: their items are made again.
                for (Menu *m : {_menu, _desktopMenu, _iconMenu, _newMenu})
                        if (m != nullptr && m->isShown()) m->dismiss();
                _launcher = nullptr;
                const bool bottom = setting(bmdsConf_panelEdge) == "bottom";
                const bool moved = bottom != _bottom;
                if (moved) {
                        _bottom = bottom;
                        (void)_panel->move(0, static_cast<uint16_t>(bottom ? _screen.height - Height : 0));
                }
                // A new theme drops the old one's icons: whatever holds them is made again.
                const StringView theme = setting(bmdsConf_panelIconTheme);
                const bool       themed = theme != StringView(_theme);
                if (themed) {
                        std::snprintf(_theme, sizeof(_theme), "%.*s", int(theme.size()), theme.data());
                        // The type icons' copies go first, then the theme's: the windows take new ones.
                        _fileIcons.clear();
                        _app.icons().setTheme(StringView(_theme));
                        for (FolderWindow *w : _folderWindows) w->settingsChanged();
                        if (_newMenu != nullptr) {
                                _newMenu->~Menu();
                                Memory::free(_newMenu);
                                _newMenu = nullptr;
                        }
                }
                const StringView apps = setting(bmdsConf_panelLauncherApplications);
                std::snprintf(_applications, sizeof(_applications), "%.*s", int(apps.size()), apps.data());
                _actions.setApplications(StringView(_applications));
                _submenuAfter = static_cast<size_t>(intSetting(bmdsConf_panelLauncherSubmenus));
                reloadLauncher();
                // The row: its items made again from panel/items and theirs.
                while (Widget *w = _row->firstChild()) w->destroy();
                _launcherButton = nullptr;
                _windows = nullptr;
                if (buildItems(_row).isError()) say("panel: out of memory for its items\n");
                _row->relayout();
                windowsChanged(this);
                if (_desktop != nullptr) {
                        const Result<bool> asked = SysConf::getBool(bmdsConf_panelDesktopMenu.key);
                        _desktop->setMenuAsked(!asked.isOk() || asked.value() ? desktopMenu : nullptr, this);
                        // In the new work area, with the new theme's icons.
                        if (moved || themed) (void)loadDesktop();
                }
                return;
        }

        // ---- Folder windows (devplan/filebrowser.md §4h)

        // A window on @p path (@p select selected once read), cascaded from the last one opened.
        void PanelApp::openFolder(StringView path, StringView select) {
                if (_folderMenus == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(FolderMenus));
                        if (mem == nullptr) return;
                        _folderMenus = ::new (mem) FolderMenus(_app);
                }
                if (_folderWindows.reserve(_folderWindows.size() + 1).isError()) return;
                const Rect        work = _app.windowManager().workArea();
                const unsigned    step = _cascade++ % 8;
                GuiWindow::Config config;
                config.rect = Rect{uint16_t(work.x + 48 + step * FolderStep), uint16_t(work.y + 24 + step * FolderStep),
                                   FolderWidth < work.width ? FolderWidth : work.width,
                                   FolderHeight < work.height ? FolderHeight : work.height};
                config.title = "Files";
                config.kind = "files";
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(FolderWindow));
                if (mem == nullptr) return;
                auto *w = ::new (mem) FolderWindow(_app, config, *this);
                (void)_folderWindows.append(w); // reserved
                Error err = w->build();
                if (err.isOk()) err = w->go(path, select);
                if (err.isOk()) err = w->show();
                if (err.isError()) {
                        if (err == Error::NoMem || err == Error::Full)
                                _actions.tell("Files", "No room for another window.");
                        folderClosed(*w);
                        return;
                }
                w->activate();
                return;
        }

        void PanelApp::folderClosed(FolderWindow &window) {
                if (window.isShown()) window.close();
                if (!_reapPosted) _reapPosted = _app.post(reapFolders, this).isOk();
                return;
        }

        // The windows closed, destroyed (after the event that closed them).
        void PanelApp::reapFolders(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                self->_reapPosted = false;
                for (size_t i = self->_folderWindows.size(); i-- > 0;) {
                        FolderWindow *w = self->_folderWindows[i];
                        if (w->isShown()) continue;
                        self->_folderWindows.removeAt(i);
                        w->~FolderWindow();
                        Memory::free(w);
                }
                return;
        }

        void PanelApp::closeFolders() {
                for (FolderWindow *w : _folderWindows) {
                        w->~FolderWindow();
                        Memory::free(w);
                }
                _folderWindows.clear();
                if (_folderMenus != nullptr) {
                        _folderMenus->~FolderMenus();
                        Memory::free(_folderMenus);
                        _folderMenus = nullptr;
                }
                if (_foldersTimer >= 0) {
                        _app.stopTimer(_foldersTimer);
                        saveFolders(this); // a change not written yet
                }
                return;
        }

        // An action changed @p folder: the windows on it read it again at once, the trees showing it
        // too, and the desktop if it is the desktop's. Its watch's news dropped: read already.
        void PanelApp::folderChanged(StringView folder) {
                _watches.forget(folder);
                for (FolderWindow *w : _folderWindows) {
                        if (w->path() == folder) w->refresh();
                        w->treeChanged(folder);
                }
                StringView       dirs(_desktopDirs);
                const StringView desktop = IniReader::nextListItem(dirs);
                if (!_quietDesktop && folder == desktop) (void)loadDesktop();
                return;
        }

        // Icons asked for while drawing (the window manager's task): loaded on the loop, then drawn.
        void PanelApp::iconsAsked(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_iconsPosted.exchange(true, std::memory_order_acq_rel)) return;
                if (self->_app.post(iconsDue, self).isError())
                        self->_iconsPosted.store(false, std::memory_order_release);
                return;
        }

        void PanelApp::iconsDue(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                self->_iconsPosted.store(false, std::memory_order_release);
                if (!self->_fileIcons.loadAsked()) return;
                for (FolderWindow *w : self->_folderWindows) w->redrawIcons();
                return;
        }

        // A watched folder changed (any task, the storage lock held): the loop told, once until it
        // takes it. A post lost to a full queue: the next change posts again.
        void PanelApp::watchNotify(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                if (self->_watchPosted.exchange(true, std::memory_order_acq_rel)) return;
                if (self->_app.post(watchesDue, self).isError())
                        self->_watchPosted.store(false, std::memory_order_release);
                return;
        }

        // The changes taken a little later, all together: a copy of many files is one read.
        void PanelApp::watchesDue(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                self->_watchPosted.store(false, std::memory_order_release);
                if (self->_watchTimer >= 0) return;
                auto [timer, err] = self->_app.startTimer(WatchDelayMs, false, watchesTick, self);
                if (err.isError()) {
                        watchesTick(self); // no timer: now
                        return;
                }
                self->_watchTimer = timer;
                return;
        }

        void PanelApp::watchesTick(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                self->_watchTimer = -1;
                self->_mountsTold = false;
                self->_watches.take(watchChanged, self);
                if (self->_mountsTold) {
                        // A card in or out: icons not found may be there now, the desktop's too.
                        self->_fileIcons.forgetMissing();
                        (void)self->loadDesktop();
                }
                return;
        }

        void PanelApp::watchChanged(void *arg, StringView path, uint8_t changes) {
                auto *self = static_cast<PanelApp *>(arg);
                if ((changes & DirectoryWatch::Mounts) != 0) self->_mountsTold = true;
                for (FolderWindow *w : self->_folderWindows) w->watchedChanged(path, changes);
                if ((changes & DirectoryWatch::Mounts) != 0) return; // the desktop once, after them all
                StringView dirs(self->_desktopDirs);
                for (StringView d = IniReader::nextListItem(dirs); !d.isEmpty(); d = IniReader::nextListItem(dirs)) {
                        if (d != path) continue;
                        (void)self->loadDesktop();
                        break;
                }
                return;
        }

        // A message to the files service: absolute paths, NUL after each; a window for each (a
        // file's folder with the file selected).
        void PanelApp::filesMessage(void *arg, ByteSpan message) {
                auto      *self = static_cast<PanelApp *>(arg);
                StringView rest(reinterpret_cast<const char *>(message.data()), message.size());
                while (!rest.isEmpty()) {
                        const size_t     end = rest.indexOf('\0');
                        const StringView path = end == StringView::NPos ? rest : rest.left(end);
                        rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                        if (path.startsWith('/')) self->openFolder(path, StringView());
                }
                return;
        }

        void PanelApp::readFilesSettings() {
                const Result<bool> hidden = SysConf::getBool(bmdsConf_filesShowHidden.key);
                _showHidden = hidden.isOk() && hidden.value();
                FolderSettings::View view = FolderSettings::View::Icons;
                (void)FolderSettings::parseView(setting(bmdsConf_filesDefaultView), view);
                _defaultView = view;
                return;
        }

        void PanelApp::filesSettingsChanged(void *arg, SysConf::Id) {
                auto *self = static_cast<PanelApp *>(arg);
                self->readFilesSettings();
                for (FolderWindow *w : self->_folderWindows) w->settingsChanged();
                return;
        }

        // folders.conf, read once: how each folder was shown.
        void PanelApp::loadFolders() {
                char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxFoldersBytes));
                if (text == nullptr) return;
                size_t             got = 0;
                const Vfs::Context vc{_context.cwd, nullptr};
                auto [file, err] = Vfs::open(StringView(FoldersPath), Vfs::Read, vc);
                while (err.isOk() && got < MaxFoldersBytes) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxFoldersBytes - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                if (_folderSettings.parse(StringView(text, got)).isError()) say("panel: no memory for folders.conf\n");
                Memory::free(text);
                return;
        }

        void PanelApp::folderSettingsChanged() {
                if (_foldersTimer >= 0) return; // written soon already
                auto [timer, err] = _app.startTimer(FoldersSaveMs, false, saveFolders, this);
                if (err.isOk()) _foldersTimer = timer;
                return;
        }

        void PanelApp::saveFolders(void *arg) {
                auto *self = static_cast<PanelApp *>(arg);
                self->_foldersTimer = -1;
                String text(MemSpace::Psram);
                Error  err = self->_folderSettings.write(text);
                if (err.isOk()) {
                        const Vfs::Context vc{self->_context.cwd, nullptr};
                        (void)Vfs::mkdir(StringView(ConfigDir), vc);
                        auto [file, oerr] =
                                Vfs::open(StringView(FoldersPath), Vfs::Write | Vfs::Create | Vfs::Truncate, vc);
                        err = oerr;
                        if (err.isOk()) {
                                auto [n, werr] = file->write(
                                        ByteSpan(reinterpret_cast<const uint8_t *>(text.cStr()), text.size()),
                                        WaitForever);
                                err = werr.isError() ? werr : (n != text.size() ? Error(Error::IoError) : Error());
                        }
                }
                if (err.isError()) {
                        char line[96];
                        std::snprintf(line, sizeof(line), "panel: %s: can't write it: %s\n", FoldersPath, err.desc());
                        self->say(line);
                }
                return;
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
