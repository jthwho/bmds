/**
 * @file      editapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/edit, the text editor (devplan/editor.md §8): `edit [FILE...]`, each file in a tab (one
 * not there yet is new, made when saved). One window a run. Its command line is an interface
 * once shipped: append only.
 */

#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/guiapplication.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

#include "editicons.h"
#include "editwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        void say(const BuiltinProgram::Context &context, const char *text) {
                FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                if (err != nullptr)
                        (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                         WaitForever);
                return;
        }

        void windowClosed(void *, GuiWindow &) {
                return; // the window decides (EditWindow::closedByManager())
        }

        // Most of the work area, centred.
        Rect placeFor(GuiApplication &app) {
                const Rect     work = app.windowManager().workArea();
                const uint32_t w = work.width * 3 / 4 > 720u   ? 720u
                                   : work.width * 3 / 4 < 480u ? work.width
                                                               : work.width * 3 / 4;
                const uint32_t h = work.height * 5 / 6;
                return Rect{uint16_t((work.x + (work.width - w) / 2) & ~3u), uint16_t(work.y + (work.height - h) / 2),
                            uint16_t(w), uint16_t(h)};
        }

        int editMain(const BuiltinProgram::Context &context) {
                // The paths, made absolute (the window has no current directory of its own).
                List<String> paths(MemSpace::Psram);
                for (size_t i = 1; i < context.args.size(); ++i) {
                        auto [full, err] = Vfs::normalize(StringView(context.args[i]), context.cwd);
                        if (err.isError()) {
                                say(context, "edit: a path too long\n");
                                return 2;
                        }
                        (void)paths.append(std::move(full));
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                app->setApplicationName("Edit");
                int status = 1;
                if (app->connect(0).isError()) {
                        say(context, "edit: the window system isn't running ('win start')\n");
                } else if (void *wmem = Memory::alloc(MemSpace::Psram, sizeof(EditWindow)); wmem != nullptr) {
                        GuiWindow::Config config;
                        config.rect = placeFor(*app);
                        config.title = "Edit";
                        config.kind = "edit";
                        EditWindow::Startup startup;
                        startup.cwd = context.cwd;
                        startup.environment = context.environment;
                        startup.launcher = context.launcher;
                        startup.process = context.process;
                        auto *window = ::new (wmem) EditWindow(*app, config, startup);
                        Error err;
                        {
                                Mutex::Locker lock(app->lock(), WaitForever);
                                err = window->build();
                                if (err.isOk()) {
                                        window->setClosed(windowClosed, app);
                                        err = window->show();
                                }
                                if (err.isOk()) {
                                        if (paths.isEmpty()) window->newDocument();
                                        for (size_t i = 0; i < paths.size(); ++i) window->open(paths[i].view());
                                }
                        }
                        if (err.isOk())
                                status = app->exec();
                        else
                                say(context, "edit: can't open its window\n");
                        {
                                Mutex::Locker lock(app->lock(), WaitForever);
                                window->~EditWindow();
                        }
                        Memory::free(wmem);
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM_ICONS(edit, editMain, 16 * 1024, EditIcons)

BMDS_NAMESPACE_END
