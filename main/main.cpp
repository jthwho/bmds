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
#include <bmds/apps.h>
#include <bmds/audiodevice.h>
#include <bmds/boot.h>
#include <bmds/cfg.h>
#include <bmds/clipboard.h>
#include <bmds/ethernetdevice.h>
#include <bmds/flashdisk.h>
#include <bmds/netstack.h>
#include <bmds/filehandle.h>
#include <bmds/i2cbus.h>
#include <bmds/input.h>
#include <bmds/pads.h>
#include <bmds/programcontrols.h>
#include <bmds/loader.h>
#include <bmds/logfiles.h>
#include <bmds/midi.h>
#include <bmds/netmidi.h>
#include <bmds/romdisk.h>
#include <bmds/guiservice.h>
#include <bmds/netmounts.h>
#include <bmds/sshservice.h>
#include <bmds/storage.h>
#include <bmds/synthfile.h>
#include <bmds/sysconf.h>
#include <bmds/sysmon.h>
#include <bmds/tmpfs.h>
#include <bmds/update.h>
#include <bmds/usbdevice.h>
#include <bmds/usbhost.h>
#include <bmds/usbserialconsole.h>
#include <bmds/vfs.h>
#include <bmds/voices.h>
#include <bmds/wavetable.h>

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

        // The SSH server, while net/ssh/server says so (devplan/network.md §24): programs run by the loader.
        bmds::Error startGui() {
                return bmds::GuiService::init(loader);
        }

        bmds::Error startSsh() {
                return bmds::SshService::init(loader);
        }

        // The network filesystems kept mounted from net/mounts (devplan/netfs.md step 4).
        bmds::Error startNetMounts() {
                return bmds::NetMounts::init(loader);
        }

        // The board as a USB device while usb/device/mode says so (devplan/audio.md §23): its
        // serial port's shells run by the loader.
        bmds::Error startUsbDevice() {
                return bmds::UsbDevice::init(loader);
        }

        constexpr bmds::Boot::Stage SystemStages[] = {
                {"console", bmds::UsbSerialConsole::init},
                {"i2c", bmds::I2cBus::init},
                {"vfs", bmds::Vfs::init},
                {"flash", bmds::FlashDisk::init},     // /flash: the board's own files, the update's records
                {"recovery", bmds::Update::watch},    // crashes in a row, Esc at power on: to recovery
                {"rom", bmds::RomDisk::init},         // /rom: the files that come with the firmware
                {"storage", bmds::Storage::init},     // the card: before /cfg, which may be on it
                {"cfg", bmds::Cfg::init},             // /cfg: the card's configuration, else /flash's
                {"sysconf", bmds::SysConf::init},     // the system configuration, saved in /cfg; /home
                {"logs", bmds::LogFiles::init},       // /dev/log; the log's files in log/dir once it mounts
                {"update", bmds::Update::init},       // the last update's result logged (devplan/update.md)
                {"sysmon", bmds::Sysmon::init},       // the tasks' CPU time, the system's counters
                {"audio", bmds::AudioDevice::init},   // dsp0: made, not started; follows audio/dsp0/...
                {"midi", bmds::Midi::init},           // the router, /dev/midi*; before USB's MIDI devices
                {"clipboard", bmds::Clipboard::init}, // its configuration in /cfg; /dev/clip0
                {"tmp", bmds::TmpFs::init},
                {"apps", bmds::Apps::init},        // the installed applications, /apps (after the card's mount)
                {"programs", bmds::Loader::init},  // the process table
                {"synth", bmds::SoundFonts::init}, // fonts synths share; after the loader: its cache is reclaimed first
                {"wavetable", bmds::Wavetable::init}, // the built-in synth's port, always there (midi/connect)
                {"speech", bmds::Voices::init},       // the voices speakers share
                {"input", bmds::Input::init},
                {"pads", bmds::Pads::init},                // the gamepads' table; before USB attaches any
                {"controls", bmds::ProgramControls::init}, // programs' actions, changed live
                {"gui", startGui},                         // programs' windows (gui_open), while the window system runs
                {"usb", bmds::UsbHost::init},
                {"usbdev", startUsbDevice},           // usb/device/mode: the console port as a USB device
                {"net", bmds::EthernetDevice::init},  // eth0 (devplan/network.md)
                {"ip", bmds::NetStack::init},         // lwIP, lo, and eth0's addresses from sysconf
                {"ssh", startSsh},                    // dropbear from the card while net/ssh/server is on
                {"guinet", bmds::GuiService::listen}, // remote programs on the loopback while gui/listen says so
                {"netmounts", startNetMounts},        // net/mounts' filesystems, mounted once the network is up
                {"netmidi", bmds::NetMidi::init},     // MIDI over the network while midi/net/enabled is on
                {"shell", startSerialShell},
                {"healthy", bmds::Update::confirm}, // the boot got here: kept if it was on probation
        };

        // Past the boot's table, the boot would stop at its first line: found here instead.
        static_assert(sizeof(SystemStages) / sizeof(SystemStages[0]) <= bmds::Boot::MaxSystemStages,
                      "more boot stages than Boot::MaxSystemStages");

} // namespace

extern "C" void app_main(void) {
        bmds::Boot::run(SystemStages);
        return;
}
