/**
 * @file      imageviewapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/imageview, the image viewer (devplan/images.md §4): `imageview [FILE...]`. Each run is a
 * window of its own. Its command line is an interface once shipped: append only.
 */

#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/guiapplication.h>
#include <bmds/images.h>
#include <bmds/imagescaler.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

#include "imageviewicons.h"
#include "imagewindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t MinWidth = 480;
        constexpr uint16_t MinHeight = 360;

        void say(const BuiltinProgram::Context &context, const char *text) {
                FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                if (err != nullptr)
                        (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                         WaitForever);
                return;
        }

        void windowClosed(void *arg, GuiWindow &) {
                static_cast<GuiApplication *>(arg)->quit(0);
                return;
        }

        // The window: sized to the first picture (its header read), within most of the work area.
        Rect placeFor(GuiApplication &app, StringView first) {
                const Rect        work = app.windowManager().workArea();
                uint32_t          w = 800, h = 600;
                Images::FileBytes file;
                if (!first.isEmpty() && file.read(first, Vfs::Context{}).isOk()) {
                        auto [info, err] = Images::info(file.bytes());
                        if (err.isOk()) {
                                const ImageScaler::Size up =
                                        ImageScaler::oriented(info.width, info.height, info.orientation);
                                // The picture, its toolbar and status bar, the frame: about 80 rows more.
                                const ImageScaler::Size f = ImageScaler::fit(up.width, up.height, work.width * 9 / 10,
                                                                             work.height * 9 / 10 - 80, false);
                                w = f.width + 16;
                                h = f.height + 80;
                        }
                }
                if (w < MinWidth) w = MinWidth;
                if (h < MinHeight) h = MinHeight;
                if (w > work.width) w = work.width;
                if (h > work.height) h = work.height;
                return Rect{uint16_t((work.x + (work.width - w) / 2) & ~3u), uint16_t(work.y + (work.height - h) / 2),
                            uint16_t(w), uint16_t(h)};
        }

        int imageviewMain(const BuiltinProgram::Context &context) {
                // The paths, made absolute (the window's worker has no current directory).
                List<String> paths(MemSpace::Psram);
                for (size_t i = 1; i < context.args.size(); ++i) {
                        auto [full, err] = Vfs::normalize(StringView(context.args[i]), context.cwd);
                        if (err.isError()) {
                                say(context, "imageview: a path too long\n");
                                return 2;
                        }
                        (void)paths.append(std::move(full));
                }
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                app->setApplicationName("Image Viewer");
                int status = 1;
                if (app->connect(0).isError()) {
                        say(context, "imageview: the window system isn't running ('win start')\n");
                } else if (void *wmem = Memory::alloc(MemSpace::Psram, sizeof(ImageWindow)); wmem != nullptr) {
                        GuiWindow::Config config;
                        config.rect = placeFor(*app, paths.isEmpty() ? StringView() : paths[0].view());
                        config.title = "Image Viewer";
                        config.kind = "imageview";
                        auto *window = ::new (wmem) ImageWindow(*app, config);
                        Error err;
                        {
                                Mutex::Locker lock(app->lock(), WaitForever);
                                err = window->build();
                                if (err.isOk()) {
                                        window->setClosed(windowClosed, app);
                                        err = window->show();
                                }
                                if (err.isOk()) {
                                        StringView views[16];
                                        size_t     n = 0;
                                        for (size_t i = 0; i < paths.size() && n < 16; ++i)
                                                views[n++] = paths[i].view();
                                        window->open(Span<const StringView>(views, n));
                                }
                        }
                        if (err.isOk())
                                status = app->exec();
                        else
                                say(context, "imageview: can't open its window\n");
                        {
                                Mutex::Locker lock(app->lock(), WaitForever);
                                window->~ImageWindow();
                        }
                        Memory::free(wmem);
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM_ICONS(imageview, imageviewMain, 16 * 1024, ImageviewIcons)

BMDS_NAMESPACE_END
