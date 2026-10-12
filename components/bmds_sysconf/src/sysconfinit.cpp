/**
 * @file      sysconfinit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system configuration's boot stage (target only): the built-in
 * declarations from their linker section, the store, /home; the parameters
 * whose defaults are the build's (devplan/sysconf.md §8), and the log's (§13).
 */

#include <sdkconfig.h>
#include <bmds/clock.h>
#include <bmds/inireader.h>
#include <bmds/log.h>
#include <bmds/staticobject.h>
#include <bmds/password.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

#include "sysconfhome.h"
#include "sysconfstate.h"

extern "C" const bmds::Config::Param _bmds_conf_start[];
extern "C" const bmds::Config::Param _bmds_conf_end[];

BMDS_NAMESPACE_BEGIN

namespace {

        // clock/zone: the kernel's time zone (rules, or a name in the zone database).
        Error checkZone(StringView value) {
                return Clock::checkTimeZone(value);
        }

        BMDS_SYSCONF(clockZone, "clock/zone", Config::Type::String, CONFIG_BMDS_TIMEZONE,
                     "The kernel's time zone: POSIX TZ rules or a zone name (Europe/Berlin)", 0, 64, nullptr, checkZone)

        // clock/tzdata: the time zone database (devplan/time.md §9); it needn't exist yet (a card).
        BMDS_SYSCONF(clockTzdata, "clock/tzdata", Config::Type::Path, CONFIG_BMDS_ZONEINFO_PATH,
                     "The time zone database (tools/tzbundle's zoneinfo.bin), shown as /share/zoneinfo", 0, 0, nullptr,
                     nullptr)

        constinit StaticObject<SysConf::Watcher> clockWatcher;

        // The clock's parameters applied: at boot and on each change.
        void applyClock(void *) {
                String value;
                if (SysConf::get("clock/tzdata", value).isOk()) {
                        const Error err = Vfs::setZoneDatabase(value.view());
                        if (err.isError()) bmdsWarn("SysConf: clock/tzdata %s: %s", value.cStr(), err.desc());
                }
                if (SysConf::get("clock/zone", value).isOk()) {
                        const Error err = Clock::setTimeZone(value.view());
                        if (err.isError()) bmdsWarn("SysConf: clock/zone %s: %s", value.cStr(), err.desc());
                }
                return;
        }

        // log/level: the least a line must be to be written.
        BMDS_SYSCONF(logLevel, "log/level", Config::Type::Enum, "info",
                     "The least a log line must be to be written (info, warn, err; debug: programs' debug lines too)",
                     0, 0, "info|warn|err|debug", nullptr)

        // log/debug: each item a debug module (`log` lists them), or all.
        Error checkDebug(StringView value) {
                for (StringView name = IniReader::nextListItem(value); !name.isEmpty();
                     name = IniReader::nextListItem(value))
                        if (name != "all" && Log::findDebugModule(name) == nullptr) return Error::NotExist;
                return Error::Ok;
        }

        BMDS_SYSCONF(logDebug, "log/debug", Config::Type::String, "",
                     "The debug modules that log ('log' lists them; ; between; all: every one)", 0, 256, nullptr,
                     checkDebug)

        constinit StaticObject<SysConf::Watcher> logWatcher;

        // The log's parameters applied: at boot and on each change.
        void applyLog(void *) {
                String value;
                if (SysConf::get("log/level", value).isOk())
                        Log::setLevel(value.view() == "err"     ? LogLevel::Err
                                      : value.view() == "warn"  ? LogLevel::Warn
                                      : value.view() == "debug" ? LogLevel::Debug
                                                                : LogLevel::Info);
                if (SysConf::get("log/debug", value).isError()) return;
                for (const DebugModule &m : Log::debugModules()) m.setEnabled(false);
                StringView list = value.view();
                for (StringView name = IniReader::nextListItem(list); !name.isEmpty();
                     name = IniReader::nextListItem(list)) {
                        if (name == "all") {
                                for (const DebugModule &m : Log::debugModules()) m.setEnabled(true);
                        } else if (const DebugModule *m = Log::findDebugModule(name)) {
                                m->setEnabled(true);
                        }
                }
                return;
        }

} // namespace

Error SysConf::init() {
        const detail::LoadReport report = detail::start(
                Span<const Param>(_bmds_conf_start, size_t(_bmds_conf_end - _bmds_conf_start)), StringView(StorePath));
        if (report.error.isError()) {
                bmdsErr("SysConf: %s", report.error.desc());
                return report.error;
        }
        if (report.badDeclarations != 0)
                bmdsErr("SysConf: %u declarations refused (the first: %s)", unsigned(report.badDeclarations),
                        report.firstBad);
        if (report.storeError.isError()) bmdsWarn("SysConf: %s: %s", StorePath, report.storeError.desc());
        if (report.malformed != 0)
                bmdsWarn("SysConf: %s: %u lines not understood", StorePath, unsigned(report.malformed));
        if (report.rejected != 0)
                bmdsWarn("SysConf: %s: %u saved values their type refuses: defaults used", StorePath,
                         unsigned(report.rejected));
        bmdsInfo("SysConf: %u parameters; %u saved values kept for keys not declared", unsigned(report.parameters),
                 unsigned(report.orphans - report.rejected));
        Error err = Home::start();
        if (err.isError()) bmdsWarn("SysConf: /home: %s", err.desc());
        err = Password::init();
        if (err.isError()) bmdsWarn("SysConf: /etc/passwd: %s", err.desc());
        applyClock(nullptr);
        clockWatcher.create();
        (void)clockWatcher->start("clock", applyClock, nullptr);
        applyLog(nullptr);
        logWatcher.create();
        (void)logWatcher->start("log", applyLog, nullptr);
        return Error::Ok;
}

BMDS_NAMESPACE_END
