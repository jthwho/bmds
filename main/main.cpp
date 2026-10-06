/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ESP-IDF entry point. This is where the system is composed: the kernel boot
 * sequence runs its own stages, then the ones listed here.
 */

#include <sdkconfig.h>
#include <bmds/boot.h>
#include <bmds/bootdisk.h>
#include <bmds/i2cbus.h>
#include <bmds/input.h>
#include <bmds/loader.h>
#include <bmds/ramdisk.h>
#include <bmds/shell.h>
#include <bmds/storage.h>
#include <bmds/usbhost.h>
#include <bmds/usbserialconsole.h>
#include <bmds/vfs.h>

namespace {

        // Runs program files for the shells (stateless).
        constinit bmds::Loader loader;

        bmds::Error startSerialShell() {
                bmds::Shell::Startup startup;
                // At once: a script that wants the card waits for it (waitmount, devplan/storage.md §13).
                startup.script = "/boot/autoexec.sh";
                startup.launcher = &loader;
                return bmds::Shell::spawn(bmds::UsbSerialConsole::instance(), "shell", startup);
        }


        constexpr bmds::Boot::Stage SystemStages[] = {
                {"console", bmds::UsbSerialConsole::init},
                {"i2c", bmds::I2cBus::init},
                {"vfs", bmds::Vfs::init},
                {"boot", bmds::BootDisk::init}, // /boot: the profile and autoexec.sh
                {"storage", bmds::Storage::init},
                {"ramdisk", bmds::RamDisk::init},
                {"programs", bmds::Loader::init}, // the process table
                {"input", bmds::Input::init},
                {"usb", bmds::UsbHost::init},
                {"shell", startSerialShell},
        };

} // namespace

extern "C" void app_main(void) {
        bmds::Boot::run(SystemStages);
        return;
}
