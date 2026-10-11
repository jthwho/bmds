/**
 * @file      sysconfsystem.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's own parameters (devplan/notifications.md §2): its host name, by default bmds- and
 * the serial number's last six characters; text templates filled with the system's values
 * (devplan/templates.md). Target only: the serial is the board's.
 */

#include <bmds/clock.h>
#include <bmds/hostname.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/texttemplate.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        Error checkHostName(StringView value) {
                return HostName::isValid(value) ? Error::Ok : Error::Invalid;
        }

        BMDS_SYSCONF(systemHostname, "system/hostname", Config::Type::String, "",
                     "The system's name: letters, digits and - (empty: bmds- and the serial number's last six)", 0,
                     int32_t(HostName::MaxBytes), nullptr, checkHostName)

} // namespace

size_t SysConf::hostname(char (&out)[HostName::MaxBytes + 1]) {
        String configured(MemSpace::Psram);
        if (get(StringView(bmdsConf_systemHostname.key), configured).isError()) configured.clear();
        return HostName::resolve(configured.view(), StringView(System::serial()), out);
}

Error SysConf::expand(StringView text, String &out) {
        char hostName[HostName::MaxBytes + 1];
        (void)hostname(hostName);
        const LocalTime      now = Clock::localTime(Clock::now());
        TextTemplate::Values values;
        values.home = Vfs::HomePath;
        values.hostname = StringView(hostName);
        values.serial = StringView(System::serial());
        values.time = &now;
        return TextTemplate::expand(text, values, out);
}

BMDS_NAMESPACE_END
