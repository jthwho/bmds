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
#include <bmds/audiodevice.h>
#include <bmds/boot.h>
#include <bmds/bootdisk.h>
#include <bmds/clipboard.h>
#include <bmds/filehandle.h>
#include <bmds/i2cbus.h>
#include <bmds/input.h>
#include <bmds/loader.h>
#include <bmds/midi.h>
#include <bmds/ramdisk.h>
#include <bmds/storage.h>
#include <bmds/sysconf.h>
#include <bmds/usbhost.h>
#include <bmds/usbserialconsole.h>
#include <bmds/vfs.h>

namespace {

        // Runs programs (stateless).
        constinit bmds::Loader loader;

        // Pid 1: the serial shell, a login shell (devplan/processes.md §3.4), leading a session on
        // the serial console. It never ends.
        bmds::Error startSerialShell() {
                bmds::Console &console = bmds::UsbSerialConsole::instance();
                auto [handle, err] = bmds::FileHandle::open(console, bmds::FileHandle::ReadWrite);
                if (err.isError()) return err;
                bmds::FdTable fds;
                (void)fds.set(bmds::FdTable::Stdin, handle);
                (void)fds.set(bmds::FdTable::Stdout, handle);
                (void)fds.set(bmds::FdTable::Stderr, handle);
                static constexpr bmds::StringView Args[] = {"sh", "-l"};
                bmds::ProgramLauncher::Request    request;
                request.path = "/bin/sh";
                request.args = bmds::Span<const bmds::StringView>(Args, 2);
                request.fds = &fds;
                request.cwd = "/";
                request.console = &console;
                request.session = true;
                return loader.start(request).error();
        }

        constexpr bmds::Boot::Stage SystemStages[] = {
                {"console", bmds::UsbSerialConsole::init},
                {"i2c", bmds::I2cBus::init},
                {"vfs", bmds::Vfs::init},
                {"boot", bmds::BootDisk::init},       // /boot: the profile and autoexec.sh
                {"sysconf", bmds::SysConf::init},     // the system configuration, saved on /boot; /home
                {"audio", bmds::AudioDevice::init},   // dsp0: made, not started; follows audio/dsp0/...
                {"midi", bmds::Midi::init},           // the router, /dev/midi*; before USB's MIDI devices
                {"clipboard", bmds::Clipboard::init}, // its configuration on /boot; /dev/clip0
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
