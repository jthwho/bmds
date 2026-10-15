/**
 * @file      appsservice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's applications (devplan/apps.md §4, §11): an Apps over the `apps/path` setting,
 * /apps following it, and the `apps` command. Target only.
 */

#include <new>
#include <bmds/apps.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/sysconf.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char PathKey[] = "apps/path";

        BMDS_SYSCONF(appsPath, PathKey, Config::Type::String, "/sd0/p0/apps",
                     "Where installed applications' NAME.app folders are: folders separated by ;, the first "
                     "of a command's name winning",
                     0, 0, nullptr, nullptr)

        // The service and what watches its setting (Psram, made once).
        struct System {
                        Apps             apps;
                        SysConf::Watcher watcher;
        };

        constinit System *systemApps = nullptr;

        Error rootsFromSettings(void *, String &out) {
                return SysConf::get(StringView(PathKey), out);
        }

        // A scan changed what is installed: the log says what.
        void logProblem(void *, const Apps::Problem &problem) {
                bmdsWarn("apps: %s: %s", problem.folder.cStr(), problem.why);
                return;
        }

        void installedChanged(void *arg) {
                auto *apps = static_cast<Apps *>(arg);
                bmdsInfo("apps: %u installed", unsigned(apps->count()));
                apps->visitProblems(logProblem, nullptr);
                return;
        }

        void settingChanged(void *arg) {
                static_cast<Apps *>(arg)->rescan(); // must not block: the scan is the thread's
                return;
        }

        // ---- The `apps` command

        void printApp(void *arg, const Apps::App &app) {
                auto &ctx = *static_cast<CommandContext *>(arg);
                ctx.printf("%-16s %-16s %s\n", app.command, app.id, app.program.cStr());
                return;
        }

        void printProblem(void *arg, const Apps::Problem &problem) {
                auto &ctx = *static_cast<CommandContext *>(arg);
                ctx.printf("%s: %s\n", problem.folder.cStr(), problem.why);
                return;
        }

        int runApps(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                Apps                   *apps = Apps::system();
                if (apps == nullptr) {
                        ctx.eprintf("apps: not running\n");
                        return Shell::ExitFailure;
                }
                if (args.size() == 2 && StringView(args[1]) == "rescan") {
                        apps->rescan();
                        return Shell::ExitSuccess;
                }
                if (args.size() != 1) return ctx.usage();
                ctx.printf("%-16s %-16s %s\n", "Command", "Id", "Program");
                apps->visit(printApp, &ctx);
                apps->visitProblems(printProblem, &ctx);
                return Shell::ExitSuccess;
        }

} // namespace

Apps *Apps::system() {
        return systemApps != nullptr ? &systemApps->apps : nullptr;
}

Error Apps::init() {
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(System));
        if (memory == nullptr) return Error::NoMem;
        auto  *system = ::new (memory) System();
        Config config;
        config.roots = rootsFromSettings;
        config.notify = installedChanged;
        config.notifyArg = &system->apps;
        config.links = true;
        Error err = system->apps.start(config);
        if (err.isOk()) err = system->watcher.start(StringView(PathKey), settingChanged, &system->apps);
        if (err.isError()) {
                bmdsWarn("apps: %s", err.desc());
                system->~System();
                Memory::free(memory);
                return Error::Ok; // no applications, the rest boots
        }
        systemApps = system;
        return Error::Ok;
}

BMDS_SHELL_COMMAND(apps, runApps, "[rescan]",
                   "List the installed applications (apps/path's NAME.app folders, each a command in /apps) "
                   "and the folders that can't be used; rescan looks again")

BMDS_NAMESPACE_END
