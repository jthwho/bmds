/**
 * @file      sysconfhome.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `home` parameter: the VFS's /home follows it (devplan/sysconf.md §7).
 * When the filesystem holding it goes (a card pulled), `home` becomes the
 * default, and when it comes back, `home` is what it was again.
 */

#include <atomic>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/thread.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>
#ifdef ESP_PLATFORM
#include <bmds/log.h>
#endif

#include "sysconfhome.h"

BMDS_NAMESPACE_BEGIN

namespace SysConf::Home {

        namespace {

                // `home` must be an existing directory (on a mounted filesystem, so a card's).
                Error checkHome(StringView value) {
                        Vfs::Context context;
                        auto [info, err] = Vfs::stat(value, context);
                        if (err.isError()) return err;
                        return info.isDirectory() ? Error() : Error(Error::NotDir);
                }

                // A new home gets the default home's Desktop: its files, once.
                void prepare(StringView home) {
                        if (home == StringView(DefaultPath)) return;
                        Vfs::Context context;
                        String       desktop;
                        if (desktop.append(home).isError() || desktop.append("/Desktop").isError()) return;
                        if (Vfs::stat(desktop.view(), context).isOk()) return;
                        if (Vfs::mkdir(desktop.view(), context).isError()) return;
                        String from;
                        if (from.append(DefaultPath).isError() || from.append("/Desktop").isError()) return;
                        auto [dir, err] = Vfs::openDirectory(from.view(), context);
                        if (err.isError()) return;
                        for (auto entry = dir.next(); entry.isOk(); entry = dir.next()) {
                                if (entry.value().type != FileType::File) continue;
                                String a, b;
                                if (a.append(from.view()).isError() || a.append('/').isError() ||
                                    a.append(entry.value().name.view()).isError() ||
                                    b.append(desktop.view()).isError() || b.append('/').isError() ||
                                    b.append(entry.value().name.view()).isError())
                                        return;
                                (void)Vfs::copyFile(a.view(), b.view(), context);
                        }
                        return;
                }

                // A home that went with its filesystem, to have back when it returns ("" none), and
                // whether the change being told of is this module's own. The thread's, but `apply()`
                // clears it when somebody else sets `home` meanwhile.
                constinit StaticObject<String> missed;
                constinit std::atomic<bool>    ownChange{false};

                // The registry told of a change (or start()): /home follows.
                void apply(void *) {
                        if (!ownChange.load(std::memory_order_acquire) && missed.isCreated()) missed->clear();
                        String home;
                        if (SysConf::get("home", home).isError()) return;
                        prepare(home.view());
                        (void)Vfs::setHome(home.view(), StringView(DefaultPath));
                        return;
                }

#ifdef ESP_PLATFORM
                void report(StringView leads, bool configured) {
                        bmdsInfo("SysConf: /home leads to %.*s%s", int(leads.size()), leads.data(),
                                 configured ? "" : " (home isn't mounted)");
                        return;
                }
#endif

                constinit StaticObject<Watcher> watcher;

                bool isDirectory(StringView path) {
                        Vfs::Context context;
                        auto [info, err] = Vfs::stat(path, context);
                        return err.isOk() && info.isDirectory();
                }

                void setOwn(StringView value, bool toDefault) {
                        ownChange.store(true, std::memory_order_release);
                        (void)(toDefault ? SysConf::setDefault("home") : SysConf::set("home", value));
                        ownChange.store(false, std::memory_order_release);
                        return;
                }

                // After each mount and unmount: a home whose filesystem went becomes the default
                // (kept to come back to), and comes back when its directory is there again.
                void followMounts() {
                        String home;
                        if (SysConf::get("home", home).isError()) return;
                        if (home.view() != StringView(DefaultPath) && !isDirectory(home.view())) {
#ifdef ESP_PLATFORM
                                bmdsInfo("SysConf: home %s isn't there: %s until it is", home.cStr(), DefaultPath);
#endif
                                (void)missed->assign(home.view());
                                setOwn(StringView(), true);
                                return;
                        }
                        if (missed->isEmpty() || home.view() != StringView(DefaultPath) || !isDirectory(missed->view()))
                                return;
#ifdef ESP_PLATFORM
                        bmdsInfo("SysConf: home %s is back", missed->cStr());
#endif
                        const String back = std::move(*missed);
                        missed->clear();
                        setOwn(back.view(), false);
                        return;
                }

                constinit StaticObject<Thread> follower;

                void followerMain(void *) {
                        Semaphore       changed(1, 0);
                        WaitList::Entry entry(Vfs::mountWaiters(), changed);
                        for (;;) {
                                followMounts();
                                (void)changed.acquire(WaitForever);
                        }
                }

        } // namespace

        BMDS_SYSCONF(home, "home", Config::Type::Path, "/flash/home", "The user's home directory: where /home leads", 0,
                     0, nullptr, checkHome)

        const Config::Param &param() {
                return bmdsConf_home;
        }

        Error start() {
#ifdef ESP_PLATFORM
                Vfs::setHomeReport(report);
#endif
                Vfs::Context context;
                const Error  made = Vfs::mkdir(StringView(DefaultPath), context);
                if (made.isError() && made != Error::Exists) return made;
                if (!watcher.isCreated()) watcher.create();
                if (!missed.isCreated()) missed.create(MemSpace::Psram);
                apply(nullptr);
                Error err = watcher->start("home", apply, nullptr);
                if (err.isError() || follower.isCreated()) return err;
                // The mounts followed on a thread of its own: a mount's waiters are told with the
                // storage lock held, and following them uses the VFS.
                Thread::Config config;
                config.name = "home";
                config.stackBytes = 4096;
                return follower.create().start(config, followerMain, nullptr);
        }

        void stop() {
                if (watcher.isCreated()) watcher->stop();
                (void)Vfs::setHome(StringView(), StringView());
                return;
        }

} // namespace SysConf::Home

BMDS_NAMESPACE_END
