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
#include <bmds/shell.h>
#include <bmds/storage.h>
#include <bmds/usbserialconsole.h>
#include <bmds/vfs.h>

namespace {

        bmds::Error startSerialShell() {
                return bmds::Shell::spawn(bmds::UsbSerialConsole::instance(), "shell");
        }

        constexpr bmds::Boot::Stage SystemStages[] = {
                {"console", bmds::UsbSerialConsole::init},
                {"vfs", bmds::Vfs::init},
                {"storage", bmds::Storage::init},
                {"shell", startSerialShell},
        };

} // namespace

extern "C" void app_main(void) {
        bmds::Boot::run(SystemStages);
        return;
}
