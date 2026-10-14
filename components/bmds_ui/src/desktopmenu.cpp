/**
 * @file      desktopmenu.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The window manager's desktop menu, from /cfg/config/wm.conf, and the
 * programs it starts (devplan/gui.md §4o).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <sdkconfig.h>
#include <bmds/command.h>
#include <bmds/desktopentry.h>
#include <bmds/fdtable.h>
#include <bmds/inireader.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/programlauncher.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>
#include <bmds/windowmanager.h>

#include "desktopmenu.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *ConfigPath = "/cfg/config/wm.conf";
        constexpr size_t      MaxConfigBytes = 4096;
        constexpr size_t      MaxEnvironment = 48;
        constexpr size_t      MaxEnvironmentBytes = 4096;
        constexpr size_t      MaxCwd = 255;
        constexpr size_t      LaunchStackBytes = 8192;

        // What the menu's launches need, kept from `win start` (PSRAM, made once, reused).
        struct Menu {
                        char             labels[WindowManager::MaxDesktopItems][WindowManager::MaxDesktopLabel + 1];
                        char             commands[WindowManager::MaxDesktopItems][DesktopEntry::MaxExec + 1];
                        size_t           count = 0;
                        char             keyCommand[DesktopEntry::MaxExec + 1] = {}; // [Keys] Terminal
                        char             environmentText[MaxEnvironmentBytes];
                        StringView       environment[MaxEnvironment];
                        size_t           environmentCount = 0;
                        char             cwd[MaxCwd + 1] = {};
                        FdTable          fds; // all three on /dev/null
                        ProgramLauncher *launcher = nullptr;
                        // The thread starting a choice's program: it detaches itself at once (a
                        // thread's own call), so the next choice can start another; the lock makes
                        // the two calls one at a time.
                        Mutex  launchLock;
                        Thread launchThread;
        };

        Menu *menu = nullptr;

        // Item @p arg's command started (on a thread of its own: the window manager's task has
        // little stack, and loading a program may take more).
        void launch(void *arg) {
                const size_t index = reinterpret_cast<uintptr_t>(arg);
                {
                        Mutex::Locker lock(menu->launchLock, WaitForever);
                        menu->launchThread.detach();
                }
                const bool key = index == WindowManager::DesktopKey; // Ctrl+Shift+Windows
                if (menu->launcher == nullptr || (!key && index >= menu->count)) return;
                const char *label = key ? "Ctrl+Shift+Windows" : menu->labels[index];
                const char *command = key ? menu->keyCommand : menu->commands[index];
                char        buffer[DesktopEntry::MaxExec + 1];
                StringView  args[DesktopEntry::MaxArgs];
                auto [n, err] =
                        DesktopEntry::splitExec(StringView(command), buffer, sizeof(buffer), Span<StringView>(args));
                String path;
                if (err.isOk() && n == 0) err = Error::Invalid;
                if (err.isOk()) {
                        StringView searchPath;
                        for (size_t i = 0; i < menu->environmentCount; ++i)
                                if (menu->environment[i].startsWith(StringView("PATH=")))
                                        searchPath = menu->environment[i].mid(5);
                        const Vfs::Context vc{StringView(menu->cwd), nullptr};
                        err = args[0].contains('/') ? path.append(args[0])
                                                    : Vfs::findProgram(args[0], searchPath, vc, path);
                }
                if (err.isOk()) {
                        ProgramLauncher::Request request;
                        request.path = path.view();
                        request.args = Span<const StringView>(args, n);
                        request.environment = Span<const StringView>(menu->environment, menu->environmentCount);
                        request.fds = &menu->fds;
                        request.cwd = StringView(menu->cwd);
                        request.session = true; // its own, without a terminal: its window is where it speaks
                        auto [pid, serr] = menu->launcher->start(request);
                        (void)pid;
                        err = serr;
                }
                if (err.isError())
                        bmdsWarn("win: the desktop menu's %s: %s: %s", label, command,
                                 err == Error::NotExist ? "not found" : err.desc());
                return;
        }

        // Told on the window manager's task (unlocked): a thread does the rest.
        void chosen(void *, size_t index) {
                if (menu == nullptr) return;
                Mutex::Locker lock(menu->launchLock, WaitForever);
                if (menu->launchThread.isStarted()) {
                        bmdsWarn("win: the desktop menu: a program is still being started");
                        return;
                }
                Thread::Config config;
                config.name = "wmlaunch";
                config.stackBytes = LaunchStackBytes;
                config.priority = CONFIG_BMDS_SHELL_PRIORITY;
                if (menu->launchThread.start(config, launch, reinterpret_cast<void *>(uintptr_t(index))).isError())
                        bmdsWarn("win: the desktop menu: no thread to start a program");
                return;
        }

        // [Menu]'s Label=command lines, in order; Terminal=terminal without the file.
        void readItems(Menu &m) {
                m.count = 0;
                char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxConfigBytes));
                if (text == nullptr) return;
                size_t             got = 0;
                const Vfs::Context vc;
                auto [file, err] = Vfs::open(StringView(ConfigPath), Vfs::Read, vc);
                while (err.isOk() && got < MaxConfigBytes) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxConfigBytes - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                if (err.isError()) {
                        static constexpr char Default[] = "[Menu]\nTerminal=terminal\n[Keys]\nTerminal=terminal\n";
                        got = sizeof(Default) - 1;
                        std::memcpy(text, Default, got);
                }
                std::snprintf(m.keyCommand, sizeof(m.keyCommand), "terminal"); // unless [Keys] says
                IniReader reader(StringView(text, got));
                for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                        if (item.kind == IniReader::Kind::Entry && item.name.equalsIgnoreCase("Keys") &&
                            item.key == "Terminal" && item.value.size() <= DesktopEntry::MaxExec) {
                                std::snprintf(m.keyCommand, sizeof(m.keyCommand), "%.*s", int(item.value.size()),
                                              item.value.data());
                                continue;
                        }
                        if (item.kind != IniReader::Kind::Entry || !item.name.equalsIgnoreCase("Menu")) continue;
                        if (m.count == WindowManager::MaxDesktopItems || item.value.size() > DesktopEntry::MaxExec)
                                continue;
                        std::snprintf(m.labels[m.count], sizeof(m.labels[0]), "%.*s", int(item.key.size()),
                                      item.key.data());
                        std::snprintf(m.commands[m.count], sizeof(m.commands[0]), "%.*s", int(item.value.size()),
                                      item.value.data());
                        ++m.count;
                }
                Memory::free(text);
                return;
        }

        // The command's exported variables, copied as NAME=value.
        void copyEnvironment(Menu &m, CommandContext &ctx) {
                m.environmentCount = 0;
                size_t used = 0;
                for (const Variables::Entry &e : ctx.executor().state().vars.entries()) {
                        if ((e.flags & Variables::Exported) == 0 || (e.flags & Variables::IsSet) == 0) continue;
                        const StringView name = e.name.view(), value = e.value.view();
                        const size_t     bytes = name.size() + 1 + value.size();
                        if (m.environmentCount == MaxEnvironment || used + bytes > MaxEnvironmentBytes) break;
                        char *at = m.environmentText + used;
                        std::memcpy(at, name.data(), name.size());
                        at[name.size()] = '=';
                        std::memcpy(at + name.size() + 1, value.data(), value.size());
                        m.environment[m.environmentCount++] = StringView(at, bytes);
                        used += bytes;
                }
                return;
        }

} // namespace

Error installDesktopMenu(WindowManager &wm, CommandContext &ctx) {
        if (menu == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                if (mem == nullptr) return Error::NoMem;
                menu = ::new (mem) Menu();
                const Vfs::Context vc;
                auto [null, err] = Vfs::open(StringView("/dev/null"), Vfs::Read | Vfs::Write, vc);
                if (err.isOk())
                        for (int fd : {FdTable::Stdin, FdTable::Stdout, FdTable::Stderr}) (void)menu->fds.set(fd, null);
        }
        readItems(*menu);
        copyEnvironment(*menu, ctx);
        std::snprintf(menu->cwd, sizeof(menu->cwd), "%.*s", int(ctx.cwd().size()), ctx.cwd().data());
        menu->launcher = ctx.shell().launcher();
        StringView labels[WindowManager::MaxDesktopItems];
        for (size_t i = 0; i < menu->count; ++i) labels[i] = StringView(menu->labels[i]);
        return wm.setDesktopMenu(Span<const StringView>(labels, menu->count), chosen, nullptr);
}

BMDS_NAMESPACE_END
