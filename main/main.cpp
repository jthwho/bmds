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

#include <bmds/boot.h>
#include <bmds/loader.h>
#include <bmds/ramdisk.h>
#include <bmds/shell.h>
#include <bmds/storage.h>
#include <bmds/usbserialconsole.h>
#include <bmds/vfs.h>

namespace {

        // Runs program files for the shells (stateless).
        constinit bmds::Loader loader;

        // How long the first shell waits for the SD card before autoexec.sh
        // (a card is normally mounted well within this).
        constexpr unsigned int StorageWaitMs = 3000;

        void waitForStorage() {
                (void)bmds::Storage::waitForFirstMount(StorageWaitMs);
                return;
        }

        bmds::Error startSerialShell() {
                bmds::Shell::Startup startup;
                startup.wait = waitForStorage;
                startup.script = "/sd0/p0/autoexec.sh";
                startup.launcher = &loader;
                return bmds::Shell::spawn(bmds::UsbSerialConsole::instance(), "shell", startup);
        }

        constexpr bmds::Boot::Stage SystemStages[] = {
                {"console", bmds::UsbSerialConsole::init},
                {"vfs", bmds::Vfs::init},
                {"storage", bmds::Storage::init},
                {"ramdisk", bmds::RamDisk::init},
                {"programs", bmds::Loader::init}, // the process table
                {"shell", startSerialShell},
        };

} // namespace

extern "C" void app_main(void) {
        bmds::Boot::run(SystemStages);
        return;
}
