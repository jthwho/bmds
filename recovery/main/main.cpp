/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The recovery firmware's entry point (devplan/update.md §4): the console, the card, /flash, the
 * screen and the keyboard, and a shell on each of the serial console and the screen, run without
 * the program loader (which brings the whole system with it).
 */

#include <sdkconfig.h>
#include <bmds/boot.h>
#include <bmds/cfg.h>
#include <bmds/flashdisk.h>
#include <bmds/builtinprograms.h>
#include <bmds/filehandle.h>
#include <bmds/i2cbus.h>
#include <bmds/input.h>
#include <bmds/logfiles.h>
#include <bmds/shell.h>
#include <bmds/storage.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>
#include <bmds/update.h>
#include <bmds/usbhost.h>
#include <bmds/usbserialconsole.h>
#include <bmds/vfs.h>
#include "recoveryscreen.h"

namespace {

        constinit bmds::Thread serialShellThread;
        constinit bmds::Thread screenShellThread;

        // A console's shell: a login shell, run again when it ends.
        void consoleShell(void *arg) {
                bmds::Console &console = *static_cast<bmds::Console *>(arg);
                for (;;) {
                        auto [handle, err] = bmds::FileHandle::open(console, bmds::FileHandle::ReadWrite);
                        if (err.isError()) return;
                        bmds::FdTable fds;
                        (void)fds.set(bmds::FdTable::Stdin, handle);
                        (void)fds.set(bmds::FdTable::Stdout, handle);
                        (void)fds.set(bmds::FdTable::Stderr, handle);
                        static constexpr bmds::StringView Args[] = {"sh", "-l"};
                        bmds::BuiltinProgram::Context     context;
                        context.fds = &fds;
                        context.cwd = "/";
                        context.args = bmds::Span<const bmds::StringView>(Args, 2);
                        context.console = &console;
                        context.interrupt = &console.interrupt();
                        context.program = "sh";
                        (void)bmds::Shell::programMain(context);
                }
        }

        bmds::Error startShells() {
                bmds::Thread::Config config;
                config.name = "sh";
                config.stackBytes = CONFIG_BMDS_SHELL_STACK_BYTES;
                config.stackSpace = bmds::MemSpace::Psram;
                bmds::Error    err = serialShellThread.start(config, consoleShell, &bmds::UsbSerialConsole::instance());
                bmds::Console *screen = bmds::RecoveryScreen::console();
                if (err.isOk() && screen != nullptr) err = screenShellThread.start(config, consoleShell, screen);
                return err;
        }

        constexpr bmds::Boot::Stage SystemStages[] = {
                {"console", bmds::UsbSerialConsole::init},
                {"i2c", bmds::I2cBus::init}, // the display's bridge
                {"vfs", bmds::Vfs::init},
                {"flash", bmds::FlashDisk::init}, // /flash: the update's intent and result
                {"storage", bmds::Storage::init}, // the card, where packages are; before /cfg
                {"cfg", bmds::Cfg::init},         // /cfg: the card's configuration, else /flash's
                {"sysconf", bmds::SysConf::init}, // the system configuration, saved in /cfg
                {"logs", bmds::LogFiles::init},   // /dev/log; the log's files once the card mounts
                {"input", bmds::Input::init},
                {"usb", bmds::UsbHost::init},           // the keyboard
                {"screen", bmds::RecoveryScreen::init}, // Recovery Mode, the update, a terminal
                {"update", bmds::Update::init},         // the intent applied, once the card is in
                {"shell", startShells},
        };

} // namespace

extern "C" void app_main(void) {
        bmds::Boot::run(SystemStages);
        return;
}
