/**
 * @file      mplayerapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/mplayer, the media player (devplan/mediaplayer.md): `mplayer [-a | -p] [FILE...]`. One
 * player: it registers the service `mplayer`, and with one running the files go to it instead
 * (§6i): replacing its playlist; added with -a; with -p (the file browser's) each added if it isn't
 * there and the first played from its start. Its command line and its service's messages are
 * interfaces once shipped: append only.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/guiapplication.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/string.h>
#include <bmds/vfs.h>

#include "mplayericons.h"
#include "player.h"
#include "playerwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Service("mplayer");
        constexpr uint16_t   MinWidth = 320; // a size kept smaller than this: the default instead
        constexpr uint16_t   MinHeight = 240;

        /**
         * A message to the service: this header (`size` its own size: fields appended, never
         * changed), then absolute paths, each ending in a NUL.
         */
        struct ServiceHeader {
                        uint32_t size;
                        uint32_t request; // PlayerWindow::Request
        };

        struct App {
                        const BuiltinProgram::Context &context;
                        FileHandle                    *err = nullptr;
                        GuiApplication                *app = nullptr;
                        PlayerWindow                  *window = nullptr;

                        void say(const char *text) const {
                                if (err != nullptr)
                                        (void)err->write(
                                                ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                WaitForever);
                        }
        };

        // A message from another `mplayer`: its files, or only to be shown.
        void serviceMessage(void *arg, ByteSpan message) {
                auto         *self = static_cast<App *>(arg);
                ServiceHeader h{};
                if (message.size() < sizeof(h) || self->window == nullptr) return;
                std::memcpy(&h, message.data(), sizeof(h));
                if (h.size < sizeof(h) || h.size > message.size()) return;
                List<StringView> paths(MemSpace::Psram);
                const char      *text = reinterpret_cast<const char *>(message.data());
                size_t           at = h.size;
                while (at < message.size()) {
                        const size_t start = at;
                        while (at < message.size() && text[at] != '\0') ++at;
                        if (at > start && paths.append(StringView(text + start, at - start)).isError()) break;
                        ++at;
                }
                const auto request = static_cast<PlayerWindow::Request>(h.request);
                if (!paths.isEmpty() &&
                    (request == PlayerWindow::Request::Open || request == PlayerWindow::Request::Add ||
                     request == PlayerWindow::Request::Play))
                        self->window->addPaths(paths.span(), request);
                self->window->showPlayer(); // hidden: shown again
                return;
        }

        // Sends @p paths (absolute) to the player running. @return The exit status.
        int sendToRunning(App &a, Span<const StringView> paths, PlayerWindow::Request how) {
                WindowManager &wm = a.app->windowManager();
                String         message(MemSpace::Psram);
                const auto     begin = [&message](PlayerWindow::Request request) {
                        message.clear();
                        const ServiceHeader h{sizeof(ServiceHeader), uint32_t(request)};
                        return message.append(StringView(reinterpret_cast<const char *>(&h), sizeof(h)));
                };
                const auto send = [&wm, &message, &a]() {
                        const Error err = wm.sendToService(
                                Service, ByteSpan(reinterpret_cast<const uint8_t *>(message.data()), message.size()));
                        if (err.isOk()) return true;
                        a.say(err == Error::Full ? "mplayer: the player running is busy: try again\n"
                                                 : "mplayer: the player running can't be reached\n");
                        return false;
                };
                PlayerWindow::Request request = paths.size() == 0 ? PlayerWindow::Request::Show : how;
                if (begin(request).isError()) return 1;
                for (const StringView p : paths) {
                        if (p.size() + 1 + sizeof(ServiceHeader) > WindowManager::MaxServiceMessage) {
                                a.say("mplayer: a path too long to send\n");
                                return 1;
                        }
                        if (message.size() + p.size() + 1 > WindowManager::MaxServiceMessage) {
                                if (!send()) return 1;
                                request = PlayerWindow::Request::Add; // the rest after the first
                                if (begin(request).isError()) return 1;
                        }
                        if (message.append(p).isError() || message.append('\0').isError()) return 1;
                }
                return send() ? 0 : 1;
        }

        int mplayerMain(const BuiltinProgram::Context &context) {
                App a{context};
                a.err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                // Its files, absolute and there.
                auto             how = PlayerWindow::Request::Open; // -a: Add; -p: Play
                bool             bad = false;
                List<String>     owned(MemSpace::Psram);
                List<StringView> paths(MemSpace::Psram);
                for (size_t i = 1; i < context.args.size(); ++i) {
                        const StringView arg = context.args[i];
                        if ((arg == "-a" || arg == "-p") && owned.isEmpty() && how == PlayerWindow::Request::Open) {
                                how = arg == "-a" ? PlayerWindow::Request::Add : PlayerWindow::Request::Play;
                                continue;
                        }
                        if (arg.startsWith('-')) {
                                a.say("usage: mplayer [-a | -p] [FILE...]\n");
                                return 2;
                        }
                        auto [path, nerr] = Vfs::normalize(arg, context.cwd);
                        if (nerr.isOk()) nerr = Vfs::stat(path.view(), Vfs::Context{context.cwd, nullptr}).error();
                        if (nerr.isError()) {
                                char text[160];
                                std::snprintf(text, sizeof(text), "mplayer: %.*s: %s\n", int(arg.size()), arg.data(),
                                              nerr == Error::NotExist ? "not found" : nerr.desc());
                                a.say(text);
                                bad = true;
                                continue;
                        }
                        if (owned.append(std::move(path)).isError()) return 1;
                }
                for (const String &s : owned)
                        if (paths.append(s.view()).isError()) return 1;
                if (bad && paths.isEmpty()) return 1;

                void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                a.app = ::new (memory) GuiApplication();
                int status = 1;
                if (a.app->connect(0).isError()) {
                        a.say("mplayer: the window system isn't running ('win start')\n");
                } else if (Error e = a.app->registerService(Service, serviceMessage, &a); e == Error::Exists) {
                        status = sendToRunning(a, paths.span(), how);
                        if (bad) status = 1;
                } else {
                        if (e.isError()) a.say("mplayer: its service can't be registered: a second one may start\n");
                        void *pmem = Memory::alloc(MemSpace::Psram, sizeof(Player));
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(PlayerWindow));
                        if (pmem != nullptr && wmem != nullptr) {
                                auto          *player = ::new (pmem) Player();
                                PlayerSettings settings;
                                PlayerWindow::readSettings(context.cwd, settings);
                                GuiWindow::Config config;
                                config.rect = Rect{200, 120, 680, 480};
                                if (settings.width >= MinWidth && settings.height >= MinHeight) {
                                        config.rect.width = settings.width;
                                        config.rect.height = settings.height;
                                }
                                config.title = "Media Player";
                                config.kind = "mplayer";
                                auto *window = ::new (wmem) PlayerWindow(*a.app, config, *player, context.cwd);
                                Error err = player->start(PlayerWindow::reportWaiting, window, context.cwd);
                                if (err.isOk()) {
                                        Mutex::Locker lock(a.app->lock(), WaitForever);
                                        err = window->build();
                                        if (err.isOk()) err = window->show();
                                        if (err.isOk()) {
                                                a.window = window;
                                                window->restore(settings);
                                                if (!paths.isEmpty()) window->addPaths(paths.span(), how);
                                        }
                                }
                                if (err.isOk()) {
                                        status = a.app->exec();
                                        if (bad && status == 0) status = 1;
                                } else {
                                        a.say("mplayer: can't open its window\n");
                                }
                                if (a.window != nullptr) {
                                        Mutex::Locker lock(a.app->lock(), WaitForever);
                                        window->saveNow(); // where it was playing: the thread still running
                                }
                                player->quit(); // before the window it reports to goes
                                {
                                        Mutex::Locker lock(a.app->lock(), WaitForever);
                                        a.window = nullptr;
                                        window->~PlayerWindow();
                                }
                                player->~Player();
                        }
                        Memory::free(wmem);
                        Memory::free(pmem);
                }
                a.app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM_ICONS(mplayer, mplayerMain, 16 * 1024, MplayerIcons)

BMDS_NAMESPACE_END
