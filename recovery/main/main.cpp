/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The recovery firmware's entry point (devplan/update.md §4): the console, the card, /boot and a
 * shell, run without the program loader (which brings the whole system with it).
 */

#include <sdkconfig.h>
#include <bmds/boot.h>
#include <bmds/bootdisk.h>
#include <bmds/builtinprograms.h>
#include <bmds/filehandle.h>
#include <bmds/logfiles.h>
#include <bmds/shell.h>
#include <bmds/storage.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>
#include <bmds/update.h>
#include <bmds/usbserialconsole.h>
#include <bmds/vfs.h>

namespace {

        constinit bmds::Thread shellThread;

        // The serial console's shell: a login shell, run again when it ends.
        void serialShell(void *) {
                bmds::Console &console = bmds::UsbSerialConsole::instance();
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

        bmds::Error startSerialShell() {
                bmds::Thread::Config config;
                config.name = "sh";
                config.stackBytes = CONFIG_BMDS_SHELL_STACK_BYTES;
                config.stackSpace = bmds::MemSpace::Psram;
                return shellThread.start(config, serialShell, nullptr);
        }

        constexpr bmds::Boot::Stage SystemStages[] = {
                {"console", bmds::UsbSerialConsole::init},
                {"vfs", bmds::Vfs::init},
                {"boot", bmds::BootDisk::init},   // /boot: the update's intent and result
                {"sysconf", bmds::SysConf::init}, // the system configuration, saved on /boot
                {"logs", bmds::LogFiles::init},   // /dev/log; the log's files once the card mounts
                {"storage", bmds::Storage::init}, // the card, where packages are
                {"update", bmds::Update::init},   // the intent applied, once the card is in
                {"shell", startSerialShell},
        };

} // namespace

extern "C" void app_main(void) {
        bmds::Boot::run(SystemStages);
        return;
}
