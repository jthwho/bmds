/**
 * @file      patchbayapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/patchbay, the patchbay (devplan/audio.md §24): `patchbay`. One window:
 * it registers the service `patchbay`, and a second start raises it instead.
 * Its command line and its service's messages are interfaces once shipped:
 * append only.
 */

#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/icontheme.h>
#include <bmds/memory.h>

#include "patchbayicons.h"
#include "patchsource.h"
#include "patchview.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Service("patchbay");
        constexpr StringView RaiseMessage("raise");

        struct App {
                        FileHandle *err = nullptr;
                        GuiWindow  *window = nullptr;
                        PatchView  *view = nullptr;

                        void say(const char *text) const {
                                if (err != nullptr)
                                        (void)err->write(
                                                ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                WaitForever);
                        }
        };

        // Any message raises it (for later ones).
        void serviceMessage(void *arg, ByteSpan) {
                auto *self = static_cast<App *>(arg);
                if (self->window == nullptr) return;
                if (!self->window->isShown()) (void)self->window->show();
                self->window->activate();
                if (self->view != nullptr) self->view->refresh();
                return;
        }

        void windowClosed(void *arg, GuiWindow &) {
                static_cast<GuiApplication *>(arg)->quit(0);
                return;
        }

        int patchbayMain(const BuiltinProgram::Context &context) {
                App a;
                a.err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                if (context.args.size() > 1) {
                        a.say("usage: patchbay\n");
                        return 2;
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                int   status = 1;
                if (app->connect(0).isError()) {
                        a.say("patchbay: the window system isn't running ('win start')\n");
                } else if (Error e = app->registerService(Service, serviceMessage, &a); e == Error::Exists) {
                        const Error sent = app->windowManager().sendToService(
                                Service,
                                ByteSpan(reinterpret_cast<const uint8_t *>(RaiseMessage.data()), RaiseMessage.size()));
                        if (sent.isError()) a.say("patchbay: the patchbay running can't be reached\n");
                        status = sent.isOk() ? 0 : 1;
                } else {
                        if (e.isError()) a.say("patchbay: its service can't be registered: a second one may start\n");
                        void *smem = Memory::alloc(MemSpace::Psram, sizeof(KernelPatchSource));
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(GuiWindow));
                        if (smem != nullptr && wmem != nullptr) {
                                auto             *source = ::new (smem) KernelPatchSource();
                                IconTheme         icons;
                                GuiWindow::Config config;
                                config.rect = Rect{200, 120, 720, 480};
                                config.title = "Patchbay";
                                config.kind = "patchbay";
                                auto *window = ::new (wmem) GuiWindow(*app, config);
                                Error err = Error::NoMem;
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        PatchView    *view = Widget::create<PatchView>(window, *app, *source);
                                        if (view != nullptr) {
                                                window->setCentral(view);
                                                window->setFocusWidget(view);
                                                // Its own icons (built in), on its title bar and in Alt+Tab.
                                                const Icon *icon = icons.program("/bin/patchbay", 16);
                                                if (icon != nullptr) window->setIcon(icon);
                                                const Icon *large = icons.program("/bin/patchbay", 32);
                                                if (large != nullptr) window->setLargeIcon(large);
                                                window->setClosed(windowClosed, app);
                                                err = window->show();
                                                if (err.isOk()) {
                                                        if (view->start().isError())
                                                                a.say("patchbay: changes elsewhere won't show\n");
                                                        a.window = window;
                                                        a.view = view;
                                                }
                                        }
                                }
                                if (err.isOk())
                                        status = app->exec();
                                else
                                        a.say("patchbay: can't open its window\n");
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        a.window = nullptr;
                                        a.view = nullptr;
                                        window->~GuiWindow(); // its widgets with it
                                }
                                source->~KernelPatchSource();
                        }
                        Memory::free(wmem);
                        Memory::free(smem);
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM_ICONS(patchbay, patchbayMain, 16 * 1024, PatchbayIcons)

BMDS_NAMESPACE_END
