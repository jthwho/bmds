/**
 * @file      sysmonapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/sysmon, the System Monitor (devplan/sysmon.md §3.5): `sysmon`. One monitor: it registers
 * the service `sysmon`, and a second start raises it instead. Its command line and its service's
 * messages are interfaces once shipped: append only.
 */

#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/guiapplication.h>
#include <bmds/memory.h>

#include "kernelsource.h"
#include "monitor.h"
#include "sysmonicons.h"
#include "sysmonwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Service("sysmon");

        // The service's messages: "raise" (any message not known raises it, for later ones), and
        // "overview" (raised at Overview: the tray meter's click while it alarms).
        constexpr StringView RaiseMessage("raise");
        constexpr StringView OverviewMessage("overview");

        struct App {
                        FileHandle   *err = nullptr;
                        SysmonWindow *window = nullptr;

                        void say(const char *text) const {
                                if (err != nullptr)
                                        (void)err->write(
                                                ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                WaitForever);
                        }
        };

        void serviceMessage(void *arg, ByteSpan message) {
                auto *self = static_cast<App *>(arg);
                if (self->window == nullptr) return;
                if (StringView(reinterpret_cast<const char *>(message.data()), message.size()) == OverviewMessage)
                        self->window->raiseOverview();
                else
                        self->window->raise();
                return;
        }

        void windowClosed(void *arg, GuiWindow &window) {
                (void)window;
                static_cast<GuiApplication *>(arg)->quit(0);
                return;
        }

        int sysmonMain(const BuiltinProgram::Context &context) {
                App a;
                a.err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                if (context.args.size() > 1) {
                        a.say("usage: sysmon\n");
                        return 2;
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                int   status = 1;
                if (app->connect(0).isError()) {
                        a.say("sysmon: the window system isn't running ('win start')\n");
                } else if (Error e = app->registerService(Service, serviceMessage, &a); e == Error::Exists) {
                        const Error sent = app->windowManager().sendToService(
                                Service,
                                ByteSpan(reinterpret_cast<const uint8_t *>(RaiseMessage.data()), RaiseMessage.size()));
                        if (sent.isError()) a.say("sysmon: the monitor running can't be reached\n");
                        status = sent.isOk() ? 0 : 1;
                } else {
                        if (e.isError()) a.say("sysmon: its service can't be registered: a second one may start\n");
                        void *smem = Memory::alloc(MemSpace::Psram, sizeof(KernelSource));
                        void *mmem = Memory::alloc(MemSpace::Psram, sizeof(Monitor));
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(SysmonWindow));
                        if (smem != nullptr && mmem != nullptr && wmem != nullptr) {
                                auto             *source = ::new (smem) KernelSource();
                                auto             *monitor = ::new (mmem) Monitor(*source);
                                GuiWindow::Config config;
                                config.rect = Rect{160, 80, 800, 560};
                                config.title = "System Monitor";
                                config.kind = "sysmon";
                                auto *window = ::new (wmem) SysmonWindow(*app, config, *monitor);
                                Error err;
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        err = window->build();
                                        if (err.isOk()) {
                                                window->setClosed(windowClosed, app);
                                                err = window->show();
                                        }
                                        if (err.isOk()) err = window->start();
                                        if (err.isOk()) a.window = window;
                                }
                                if (err.isOk())
                                        status = app->exec();
                                else
                                        a.say("sysmon: can't open its window\n");
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        a.window = nullptr;
                                        window->~SysmonWindow();
                                }
                                monitor->~Monitor();
                                source->~KernelSource(); // its watch ends
                        }
                        Memory::free(wmem);
                        Memory::free(mmem);
                        Memory::free(smem);
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM_ICONS(sysmon, sysmonMain, 16 * 1024, SysmonIcons)

BMDS_NAMESPACE_END
