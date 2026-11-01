/**
 * @file      pkgsystem.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `pkg` in the firmware (devplan/packages.md §7, §9): its settings, the keys built in from keys/,
 * and the command.
 */

#include <ctime>
#include <sdkconfig.h>
#include <bmds/command.h>
#include <bmds/fetch.h>
#include <bmds/pkgcommand.h>
#include <bmds/shell.h>
#include <bmds/sys.h>
#include <bmds/sysconf.h>
#include <bmds/updatekeys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(pkgRoot, "pkg/root", Config::Type::Path, "/sd0/p0",
                     "Where packages install their files (bin, apps, share, etc)", 0, 255, nullptr, nullptr)
        BMDS_SYSCONF(pkgDb, "pkg/db", Config::Type::Path, "/sd0/p0/var/lib/pkg",
                     "The package database: what each installed package put where", 0, 255, nullptr, nullptr)
        BMDS_SYSCONF(pkgCache, "pkg/cache", Config::Type::Path, "/sd0/p0/var/cache/pkg",
                     "Where pkg keeps the sources' lists (pkg update reads them, pkg clean empties it)", 0, 255,
                     nullptr, nullptr)

        Error checkSources(StringView value) {
                String why;
                return Packages::checkSources(value, why);
        }

        BMDS_SYSCONF(pkgSources, "pkg/sources", Config::Type::String, "file:/sd0/p0/packages",
                     "Where packages come from, ';'-separated: file:/FOLDER (a folder of .bmpk files)", 0, 1023,
                     nullptr, checkSources)

        BMDS_SYSCONF(pkgKeys, "pkg/keys", Config::Type::Path, "/cfg/pkg/keys",
                     "Keys added to pkg's keyring (pkg key add), a file each", 0, 255, nullptr, nullptr)
        BMDS_SYSCONF(pkgDev, "pkg/dev", Config::Type::Enum, "allow",
                     "Packages signed by the dev key (anyone could have): allow, ask before installing, or refuse", 0,
                     0, "allow|ask|refuse", nullptr)

        // Seconds since 1970 for an index's date; 0 while the clock isn't set (it reads 1955 then).
        int64_t now() {
                const int64_t t = int64_t(std::time(nullptr));
                return t > 1577836800 ? t : 0; // 2020
        }

        int runPkg(CommandContext &ctx) {
                String root, db, cache, sources, keys, dev;
                if (SysConf::get(StringView("pkg/root"), root).isError() || root.isEmpty() ||
                    SysConf::get(StringView("pkg/db"), db).isError() || db.isEmpty() ||
                    SysConf::get(StringView("pkg/cache"), cache).isError() || cache.isEmpty()) {
                        ctx.eprintf("pkg: the settings pkg/root, pkg/db and pkg/cache must name folders\n");
                        return Shell::ExitFailure;
                }
                (void)SysConf::get(StringView("pkg/sources"), sources);
                (void)SysConf::get(StringView("pkg/keys"), keys);
                (void)SysConf::get(StringView("pkg/dev"), dev);
                Packages::Config config;
                config.root = root.view();
                config.db = db.view();
                config.keys = UpdateKeys::accepted();
                config.isDevKey = UpdateKeys::isDevKey;
                config.keyComment = UpdateKeys::comment;
                config.devKey = &UpdateKeys::devKey();
                config.keysDir = keys.view();
                config.dev = dev.view() == StringView("refuse") ? Packages::DevPolicy::Refuse
                             : dev.view() == StringView("ask")  ? Packages::DevPolicy::Ask
                                                                : Packages::DevPolicy::Allow;
                config.arch = CONFIG_IDF_TARGET;
                config.abi = BMDS_ABI_VERSION;
                config.cache = cache.view();
                config.sources = sources.view();
                config.now = now;
                config.fetch = &systemFetch != nullptr ? systemFetch : nullptr;
                return PkgCommand::run(ctx, config);
        }

} // namespace

BMDS_SHELL_COMMAND(pkg, runPkg, PkgCommand::Usage, PkgCommand::Help)

BMDS_NAMESPACE_END
