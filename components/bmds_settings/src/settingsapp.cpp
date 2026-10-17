/**
 * @file      settingsapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/settings, the control panel (devplan/controlpanel.md): `settings [GROUP]` opens it (at
 * GROUP's page); `settings -u` lists the keys no page places. One window: it registers the service
 * `settings`, and a second start sends it the group to show. Its command line and its service's
 * messages are interfaces once shipped: append only.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/guiapplication.h>
#include <bmds/memory.h>
#include <bmds/sysconf.h>

#include "settingsicons.h"
#include "settingstable.h"
#include "settingswindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Service("settings");

        struct App {
                        FileHandle     *out = nullptr;
                        FileHandle     *err = nullptr;
                        SettingsWindow *window = nullptr;

                        static void write(FileHandle *to, const char *text) {
                                if (to != nullptr)
                                        (void)to->write(
                                                ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                WaitForever);
                        }
        };

        // The service's message: a group's id (empty or unknown: raised as it is).
        void serviceMessage(void *arg, ByteSpan message) {
                auto *self = static_cast<App *>(arg);
                if (self->window == nullptr) return;
                self->window->raise(SettingsTable::findPage(
                        StringView(reinterpret_cast<const char *>(message.data()), message.size())));
                return;
        }

        void usage(const App &a) {
                App::write(a.err, "usage: settings [GROUP] | settings -u\n  GROUP:");
                for (const SettingsTable::Page &p : SettingsTable::pages()) {
                        App::write(a.err, " ");
                        App::write(a.err, p.id);
                }
                App::write(a.err, "\n");
                return;
        }

        // `-u`: the keys no page places (All Settings shows them, but a built-in one wants a row).
        int unplaced(const App &a) {
                size_t      count = 0, total = 0;
                SysConf::Id after = SysConf::NoId;
                for (;;) {
                        const Result<SysConf::Id> next = SysConf::next(StringView(), after);
                        if (next.isError()) break;
                        after = next.value();
                        ++total;
                        const StringView key = SysConf::keyOf(after);
                        if (SettingsTable::place(key).page >= 0) continue;
                        SysConf::Info info;
                        const bool    runtime = SysConf::info(after, info).isOk() && info.runtime;
                        char          line[96];
                        std::snprintf(line, sizeof(line), "%.*s%s\n", int(key.size()), key.data(),
                                      runtime ? " (declared at run time)" : "");
                        App::write(a.out, line);
                        ++count;
                }
                char line[64];
                std::snprintf(line, sizeof(line), "%zu of %zu keys on no page\n", count, total);
                App::write(a.out, line);
                return 0;
        }

        void windowClosed(void *arg, GuiWindow &window) {
                (void)window;
                static_cast<GuiApplication *>(arg)->quit(0);
                return;
        }

        int settingsMain(const BuiltinProgram::Context &context) {
                App a;
                a.out = context.fds != nullptr ? context.fds->handle(FdTable::Stdout) : nullptr;
                a.err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                int page = -1;
                if (context.args.size() > 2) {
                        usage(a);
                        return 2;
                }
                if (context.args.size() == 2) {
                        const StringView arg(context.args[1]);
                        if (arg == StringView("-u")) return unplaced(a);
                        page = SettingsTable::findPage(arg);
                        if (page < 0) {
                                usage(a);
                                return 2;
                        }
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                int   status = 1;
                if (app->connect(0).isError()) {
                        App::write(a.err, "settings: the window system isn't running ('win start')\n");
                } else if (Error e = app->registerService(Service, serviceMessage, &a); e == Error::Exists) {
                        const char *id = page >= 0 ? SettingsTable::pages()[size_t(page)].id : "";
                        const Error sent = app->windowManager().sendToService(
                                Service, ByteSpan(reinterpret_cast<const uint8_t *>(id), std::strlen(id)));
                        if (sent.isError()) App::write(a.err, "settings: the one running can't be reached\n");
                        status = sent.isOk() ? 0 : 1;
                } else {
                        if (e.isError())
                                App::write(a.err, "settings: its service can't be registered: a second may start\n");
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(SettingsWindow));
                        if (wmem != nullptr) {
                                GuiWindow::Config config;
                                config.rect = Rect{180, 70, 800, 560};
                                config.title = "Settings";
                                config.kind = "settings";
                                auto *window = ::new (wmem) SettingsWindow(*app, config, context);
                                Error err;
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        err = window->build();
                                        if (err.isOk()) {
                                                window->setClosed(windowClosed, app);
                                                err = window->show();
                                        }
                                        if (err.isOk()) {
                                                if (page >= 0) window->showPage(size_t(page));
                                                a.window = window;
                                        }
                                }
                                if (err.isOk())
                                        status = app->exec();
                                else
                                        App::write(a.err, "settings: can't open its window\n");
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        a.window = nullptr;
                                        window->~SettingsWindow();
                                }
                                Memory::free(wmem);
                        }
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM_ICONS(settings, settingsMain, 16 * 1024, SettingsIcons)

BMDS_NAMESPACE_END
