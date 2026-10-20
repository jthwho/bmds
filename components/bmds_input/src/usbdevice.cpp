/**
 * @file      usbdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The board as a USB device (devplan/audio.md §23): the full-speed OTG
 * controller swapped onto the console port's PHY, TinyUSB's device stack on
 * a thread of its own while `usb/device/mode` says so, a serial port with a
 * shell on a pseudo-terminal, and MIDI cables (usbdevicemidi.cpp).
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_private/startup_internal.h>
#include <esp_private/usb_phy.h>
#include <esp_system.h>
#include <soc/lp_system_struct.h>
#include <soc/usb_serial_jtag_struct.h>
#include <soc/usb_wrap_struct.h>
#include <esp_rom_sys.h>
#include <tusb.h>
#include <device/usbd_pvt.h>
#include <bmds/board.h>
#include <bmds/fdtable.h>
#include <bmds/filehandle.h>
#include <bmds/log.h>
#include <bmds/mutex.h>
#include <bmds/programlauncher.h>
#include <bmds/pty.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/thread.h>
#include <bmds/usbdevice.h>
#include "usbdevice_p.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *ModeKey = "usb/device/mode";
        constexpr const char *CablesKey = "usb/device/midi-cables";

        constexpr uint8_t  RootPort = 0;      // TinyUSB's number for the full-speed controller
        constexpr uint16_t VendorId = 0xcafe; // TinyUSB's test vendor: not for a released device
        constexpr uint16_t ProductId = 0xb0d5;
        constexpr uint8_t  MaxCables = 4;

        // Interfaces and endpoints: the serial port (its notification, then its data), then MIDI.
        constexpr uint8_t  ItfCdc = 0;
        constexpr uint8_t  ItfMidi = 2;
        constexpr uint8_t  ItfCount = 4;
        constexpr uint8_t  EpCdcNotify = 0x81;
        constexpr uint8_t  EpCdcOut = 0x02;
        constexpr uint8_t  EpCdcIn = 0x82;
        constexpr uint8_t  EpMidiOut = 0x03;
        constexpr uint8_t  EpMidiIn = 0x83;
        constexpr uint16_t BulkSize = 64; // full speed's

        // Strings: the language, then these.
        enum : uint8_t {
                StrManufacturer = 1,
                StrProduct,
                StrSerial,
                StrConsole,
                StrCable1, // and the cables after it
                StrCount = StrCable1 + MaxCables
        };

        constexpr size_t MidiDescBytes(size_t cables) {
                return TUD_MIDI_DESC_HEAD_LEN + cables * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(cables);
        }
        constexpr size_t MaxConfigBytes = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + MidiDescBytes(MaxCables);

        // The device's pads off this long before USB-Serial/JTAG takes the port, so the computer sees
        // one leave before the other arrives (both pull D+ up: swapped at once, it saw neither).
        constexpr unsigned int DisconnectMs = 20;
        constexpr unsigned int RetryMs = 2;        // TinyUSB looked at again while something waits
        constexpr unsigned int StallMs = 200;      // the console's output dropped when unread this long
        constexpr unsigned int RespawnMs = 1000;   // a shell that ended at once: the next after this
        constexpr size_t       ConsoleChunk = 256; // the console's output read at a time

        struct State {
                        ProgramLauncher          *launcher = nullptr;
                        SysConf::Watcher          watcher;
                        Semaphore                 wake{1, 0}; // a setting changed (the thread while the device is down)
                        Mutex                     lock;       // the threads' starts and ends
                        Thread                    thread;
                        bool                      running = false; // the service thread runs (lock)
                        std::atomic<bool>         changed{false};
                        std::atomic<TaskHandle_t> task{nullptr}; // the service thread's, while the stack runs

                        // TinyUSB's deferred calls are made only while its device stack is up.
                        Mutex deferLock;
                        bool  up = false; // (deferLock)

                        void             *phy = nullptr; // usb_phy_handle_t
                        uint8_t           cables = 0;
                        std::atomic<bool> configured{false};
                        std::atomic<bool> suspended{false};

                        // The console. Its thread runs a shell on a pseudo-terminal and copies
                        // the shell's output to the serial port.
                        Mutex             cdcLock;          // TinyUSB's serial calls off the task; cdcAlive
                        bool              cdcAlive = false; // the device runs: the serial port may be used
                        std::atomic<bool> dtr{false};       // the computer has the port open
                        Semaphore         txSpace{1, 0};    // the serial port sent something (room)
                        Thread            session;
                        bool              sessionRunning = false; // (lock)
                        Mutex             ptyLock;                // pty, against its end
                        Pty              *pty = nullptr;          // the session's (ptyLock)
                        uint8_t           input[64];              // typed, not taken yet (the task)
                        uint8_t           inputAt = 0;
                        uint8_t           inputCount = 0;

                        // Descriptors, made when the device starts (cables vary).
                        uint8_t  config[MaxConfigBytes] = {};
                        uint16_t text[33] = {}; // a string descriptor being returned
        };

        constinit StaticObject<State> state;

        tusb_desc_device_t const DeviceDesc = {
                .bLength = sizeof(tusb_desc_device_t),
                .bDescriptorType = TUSB_DESC_DEVICE,
                .bcdUSB = 0x0200,
                // Interface association: the serial port is two interfaces.
                .bDeviceClass = TUSB_CLASS_MISC,
                .bDeviceSubClass = MISC_SUBCLASS_COMMON,
                .bDeviceProtocol = MISC_PROTOCOL_IAD,
                .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
                .idVendor = VendorId,
                .idProduct = ProductId,
                .bcdDevice = 0x0100,
                .iManufacturer = StrManufacturer,
                .iProduct = StrProduct,
                .iSerialNumber = StrSerial,
                .bNumConfigurations = 1,
        };

        // The configuration descriptor for @p cables MIDI cables, into state->config.
        void makeConfig(uint8_t cables) {
                uint8_t *p = state->config;
                auto     put = [&p](const uint8_t *bytes, size_t n) {
                        std::memcpy(p, bytes, n);
                        p += n;
                        return;
                };
                const size_t  total = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + MidiDescBytes(cables);
                const uint8_t head[] = {TUD_CONFIG_DESCRIPTOR(1, ItfCount, 0, total, 0, 100)};
                put(head, sizeof(head));
                const uint8_t cdc[] = {
                        TUD_CDC_DESCRIPTOR(ItfCdc, StrConsole, EpCdcNotify, 8, EpCdcOut, EpCdcIn, BulkSize)};
                put(cdc, sizeof(cdc));
                const uint8_t midi[] = {TUD_MIDI_DESC_HEAD(ItfMidi, 0, cables)};
                put(midi, sizeof(midi));
                for (uint8_t i = 1; i <= cables; ++i) {
                        const uint8_t jack[] = {TUD_MIDI_DESC_JACK_DESC(i, uint8_t(StrCable1 + i - 1))};
                        put(jack, sizeof(jack));
                }
                const uint8_t out[] = {TUD_MIDI_DESC_EP(EpMidiOut, BulkSize, cables)};
                put(out, sizeof(out));
                for (uint8_t i = 1; i <= cables; ++i) *p++ = TUD_MIDI_JACKID_IN_EMB(i);
                const uint8_t in[] = {TUD_MIDI_DESC_EP(EpMidiIn, BulkSize, cables)};
                put(in, sizeof(in));
                for (uint8_t i = 1; i <= cables; ++i) *p++ = TUD_MIDI_JACKID_OUT_EMB(i);
                return;
        }

        // The console port's PHY to the OTG controller (true) or back to USB-Serial/JTAG. Swapped,
        // USB-Serial/JTAG is on the second PHY, whose pins are GPIO26/27 (the PPS output): its
        // pads are off meanwhile (docs/platforms/esp32p4.md §4d).
        void swapPhys(bool device) {
                if (device) {
                        USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
                        LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 1;
                        LP_SYS.usb_ctrl.sw_usb_phy_sel = 1;
                } else {
                        LP_SYS.usb_ctrl.sw_usb_phy_sel = 0;
                        LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 0;
                        USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
                }
                return;
        }

        // The computer's device detached, then the console port back to USB-Serial/JTAG. The
        // controller's registers outlast a restart and a panic (D+ stayed pulled up across both,
        // and the computer never saw the device go): D+ is let go whatever the controller says,
        // held so long, then the pads go off and the override with them (the next start
        // attaches).
        void detachAndUnswap() {
                if (LP_SYS.usb_ctrl.sw_usb_phy_sel == 0) return;
                USB_WRAP.otg_conf.dp_pullup = 0;
                USB_WRAP.otg_conf.dm_pullup = 0;
                USB_WRAP.otg_conf.pad_pull_override = 1;
                esp_rom_delay_us(DisconnectMs * 1000);
                USB_WRAP.otg_conf.usb_pad_enable = 0;
                USB_WRAP.otg_conf.pad_pull_override = 0;
                swapPhys(false);
                return;
        }

        // Before a restart: the swap survives it, and the next boot's early output would be lost.
        void unswapAtShutdown() {
                detachAndUnswap();
                return;
        }

        struct Wanted {
                        bool    on = false;
                        uint8_t cables = 2;
        };

        Wanted wanted() {
                Wanted w;
                String mode(MemSpace::Psram);
                w.on = board().usbDeviceOnConsole && SysConf::get(StringView(ModeKey), mode).isOk() &&
                       mode.view() == StringView("midi");
                const Result<int32_t> cables = SysConf::getInt(StringView(CablesKey));
                if (cables.isOk()) w.cables = static_cast<uint8_t>(cables.value());
                return w;
        }

        void drainWatcher() {
                SysConf::Id id;
                while (state->watcher.next(id)) {}
                (void)state->watcher.overflowed();
                return;
        }

        void nothing(void *) {
                return; // a wake-up: the service loop looks at its flags
        }

        // ---- The console -----------------------------------------------------------------------

        // The shell's output to the serial port: dropped while the computer doesn't read it.
        void sendToComputer(const uint8_t *data, size_t n) {
                size_t done = 0;
                while (done < n) {
                        {
                                Mutex::Locker lock(state->cdcLock, WaitForever);
                                if (!state->cdcAlive || !state->dtr.load()) return;
                                done += tud_cdc_n_write(0, data + done, uint32_t(n - done));
                                (void)tud_cdc_n_write_flush(0);
                        }
                        if (done < n && state->txSpace.acquire(StallMs).isError()) return;
                }
                return;
        }

        // One interactive shell (the profile; autoexec.sh is the first serial shell's) on a
        // pseudo-terminal until it ends (or the computer closes the port, which
        // hangs it up). Its output is copied to the serial port.
        Error runShell() {
                auto [pty, err] = Pty::openCounted(MemSpace::Psram);
                if (err.isError()) return err;
                auto [emulator, eerr] = FileHandle::open(*pty, FileHandle::ReadWrite);
                if (eerr.isError()) {
                        pty->release();
                        return eerr;
                }
                {
                        Mutex::Locker lock(state->ptyLock, WaitForever);
                        state->pty = pty;
                }
                Error started = Error::Ok;
                {
                        auto [program, perr] = Pty::openProgramEnd(pty->number(), FileHandle::ReadWrite);
                        started = perr;
                        if (started.isOk()) {
                                FdTable fds;
                                (void)fds.set(FdTable::Stdin, program);
                                (void)fds.set(FdTable::Stdout, program);
                                (void)fds.set(FdTable::Stderr, program);
                                static constexpr StringView Args[] = {"sh", "-i"}; // not a login: no autoexec
                                ProgramLauncher::Request    request;
                                request.path = "/bin/sh";
                                request.args = Span<const StringView>(Args, 2);
                                request.fds = &fds;
                                request.cwd = "/";
                                request.console = &pty->console();
                                request.session = true;
                                started = state->launcher->start(request).error();
                        }
                } // the program's end is the shell's now: its going ends our reads
                if (started.isError()) {
                        bmdsWarn("USB device: no shell for the serial port: %s", started.desc());
                        pty->hangUp();
                }
                uint8_t buf[ConsoleChunk];
                for (;;) {
                        const Result<size_t> got = pty->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                        if (got.isError()) break; // EndOfFile: the shell and its children are gone
                        sendToComputer(buf, got.value());
                }
                {
                        Mutex::Locker lock(state->ptyLock, WaitForever);
                        state->pty = nullptr;
                }
                emulator.reset(); // the last of both ends: the pair is released
                return started;
        }

        bool portOpen() {
                Mutex::Locker lock(state->cdcLock, WaitForever);
                return state->cdcAlive && state->dtr.load();
        }

        // A shell after another while the computer keeps the port open.
        void sessionMain(void *) {
                for (;;) {
                        while (portOpen()) {
                                const int64_t startedUs = System::uptimeUs();
                                const Error   err = runShell();
                                if (err.isError() || System::uptimeUs() - startedUs < int64_t(RespawnMs) * 1000)
                                        Thread::sleep(RespawnMs);
                        }
                        Mutex::Locker lock(state->lock, WaitForever);
                        if (portOpen()) continue; // opened again while this one ended
                        state->sessionRunning = false;
                        state->session.detach();
                        return;
                }
        }

        // The computer opened the port: a session (one at a time; one hung up but not ended yet
        // is waited for by the next open).
        void startSession() {
                Mutex::Locker lock(state->lock, WaitForever);
                if (state->sessionRunning) return;
                Thread::Config config;
                config.name = "usbcon";
                config.stackBytes = 4096;
                config.stackSpace = MemSpace::Psram;
                config.kernel = true;
                const Error err = state->session.start(config, &sessionMain, nullptr);
                if (err.isError()) {
                        bmdsErr("USB device: no console thread: %s", err.desc());
                        return;
                }
                state->sessionRunning = true;
                return;
        }

        void hangUpSession() {
                Mutex::Locker lock(state->ptyLock, WaitForever);
                if (state->pty != nullptr) state->pty->hangUp();
                return;
        }

        // Typed input (on the task): to the shell as it takes it; what it can't take waits here
        // and keeps the rest in TinyUSB's buffer (the computer is held off). True: some waits.
        bool feedConsole() {
                for (;;) {
                        if (state->inputAt == state->inputCount) {
                                state->inputAt = state->inputCount = 0;
                                if (tud_cdc_n_available(0) == 0) return false;
                                state->inputCount = uint8_t(tud_cdc_n_read(0, state->input, sizeof(state->input)));
                                if (state->inputCount == 0) return false;
                        }
                        Mutex::Locker lock(state->ptyLock, WaitForever);
                        if (state->pty == nullptr) {
                                state->inputAt = state->inputCount = 0; // nobody to read it
                                tud_cdc_n_read_flush(0);
                                return false;
                        }
                        const Result<size_t> took = state->pty->write(
                                ByteSpan(state->input + state->inputAt, size_t(state->inputCount - state->inputAt)), 1);
                        if (took.isError() || took.value() == 0) {
                                if (took.error() == Error::BrokenPipe) state->inputAt = state->inputCount;
                                return took.error() != Error::BrokenPipe;
                        }
                        state->inputAt = uint8_t(state->inputAt + took.value());
                }
        }

        // ---- The device ------------------------------------------------------------------------

        Error startDevice(uint8_t cables) {
                state->cables = cables;
                makeConfig(cables);
                swapPhys(true);
                usb_phy_config_t phy = {};
                phy.controller = USB_PHY_CTRL_OTG;
                phy.target = USB_PHY_TARGET_INT;
                phy.otg_mode = USB_OTG_MODE_DEVICE;
                phy.otg_speed = USB_PHY_SPEED_FULL; // else ESP-IDF takes the UTMI PHY (the host's)
                usb_phy_handle_t handle = nullptr;
                const esp_err_t  ret = usb_new_phy(&phy, &handle);
                if (ret != ESP_OK) {
                        swapPhys(false);
                        return Error::fromEspErr(ret);
                }
                state->phy = handle;
                state->task.store(xTaskGetCurrentTaskHandle());
                tusb_rhport_init_t rh = {};
                rh.role = TUSB_ROLE_DEVICE;
                rh.speed = TUSB_SPEED_FULL;
                if (!tud_rhport_init(RootPort, &rh)) {
                        state->task.store(nullptr);
                        (void)usb_del_phy(handle);
                        state->phy = nullptr;
                        swapPhys(false);
                        return Error::IoError;
                }
                {
                        Mutex::Locker lock(state->deferLock, WaitForever);
                        state->up = true;
                }
                {
                        Mutex::Locker lock(state->cdcLock, WaitForever);
                        state->cdcAlive = true;
                }
                bmdsInfo("USB device: on (the console port is a serial port and %u MIDI cables)", cables);
                return Error::Ok;
        }

        void stopDevice() {
                {
                        Mutex::Locker lock(state->cdcLock, WaitForever);
                        state->cdcAlive = false;
                }
                state->dtr.store(false);
                state->txSpace.release(); // an output waiting for room gives up
                hangUpSession();
                state->inputAt = state->inputCount = 0;
                detail::usbDeviceMidiStop();
                state->configured.store(false);
                state->suspended.store(false);
                {
                        Mutex::Locker lock(state->deferLock, WaitForever);
                        state->up = false;
                }
                (void)tud_deinit(RootPort);
                state->task.store(nullptr);
                (void)usb_del_phy(static_cast<usb_phy_handle_t>(state->phy)); // its pads off
                state->phy = nullptr;
                Thread::sleep(DisconnectMs);
                swapPhys(false);
                bmdsInfo("USB device: off (the console port is USB-Serial/JTAG again)");
                return;
        }

        // The service: the device up while the setting says so, TinyUSB's events between.
        void serve(void *) {
                for (;;) {
                        drainWatcher();
                        state->changed.store(false);
                        const Wanted w = wanted();
                        if (!w.on) break;
                        const Error err = startDevice(w.cables);
                        if (err.isError()) {
                                bmdsErr("USB device: not started: %s", err.desc());
                                while (!state->changed.load()) (void)state->wake.acquire(WaitForever);
                                continue;
                        }
                        while (!state->changed.load()) {
                                const bool waiting =
                                        detail::usbDeviceMidiWaiting() || state->inputAt != state->inputCount;
                                tud_task_ext(waiting ? RetryMs : UINT32_MAX, false);
                                if (detail::usbDeviceMidiWaiting()) (void)detail::usbDeviceMidiDrain();
                                if (state->inputAt != state->inputCount) (void)feedConsole();
                        }
                        stopDevice();
                }
                Mutex::Locker lock(state->lock, WaitForever);
                state->running = false;
                state->thread.detach();
                return;
        }

        // On and not running: the thread (none while off).
        void startThread() {
                Mutex::Locker lock(state->lock, WaitForever);
                if (state->running || !wanted().on) return;
                Thread::Config config;
                config.name = "usbd";
                config.stackBytes = 4096;
                config.stackSpace = MemSpace::Psram;
                config.priority = 4; // the host's (usb0)
                config.kernel = true;
                const Error err = state->thread.start(config, &serve, nullptr);
                if (err.isError()) {
                        bmdsErr("USB device: no thread: %s", err.desc());
                        return;
                }
                state->running = true;
                return;
        }

        void settingsChanged(void *) {
                state->changed.store(true);
                state->wake.release();
                (void)detail::usbDeviceDefer(nothing, nullptr);
                startThread();
                return;
        }

} // namespace

bool detail::usbDeviceIsOnTask() {
        return state.isCreated() && state->task.load() == xTaskGetCurrentTaskHandle();
}

bool detail::usbDeviceDefer(void (*fn)(void *), void *arg) {
        if (!state.isCreated()) return false;
        Mutex::Locker lock(state->deferLock, WaitForever);
        if (!state->up) return false;
        usbd_defer_func(fn, arg, false);
        return true;
}

Error UsbDevice::init(ProgramLauncher &launcher) {
        state.create();
        state->launcher = &launcher;
        static constexpr Config::Param Params[] = {
                {"usb/device/mode", Config::Type::Enum, "off",
                 "The console port as a USB device to a computer: off (USB-Serial/JTAG, the console), or midi "
                 "(a serial port with a shell, and MIDI cables)",
                 0, 0, "off|midi", nullptr},
                {"usb/device/midi-cables", Config::Type::Int, "2", "MIDI cables the board has as a USB device", 1,
                 MaxCables, nullptr, nullptr},
        };
        for (const Config::Param &p : Params) {
                const Result<SysConf::Id> id = SysConf::declareKernel(p);
                if (id.isError()) {
                        bmdsErr("USB device: setting %s not declared: %s", p.key, id.error().desc());
                        return id.error();
                }
        }
        if (!board().usbDeviceOnConsole) return Error::Ok;
        const esp_err_t hooked = esp_register_shutdown_handler(unswapAtShutdown);
        if (hooked != ESP_OK)
                bmdsWarn("USB device: no shutdown handler (%s): a restart while on keeps the computer's device",
                         esp_err_to_name(hooked));
        const Error err = state->watcher.start(StringView("usb/device"), &settingsChanged, nullptr);
        if (err.isError()) return err;
        startThread();
        return Error::Ok;
}

UsbDevice::Status UsbDevice::status() {
        Status s;
        s.supported = board().usbDeviceOnConsole;
        if (!state.isCreated()) return s;
        {
                Mutex::Locker lock(state->deferLock, WaitForever);
                s.on = state->up;
        }
        s.configured = state->configured.load();
        s.suspended = state->suspended.load();
        s.console = state->dtr.load();
        s.cables = s.on ? state->cables : 0;
        s.midiDropped = detail::usbDeviceMidiDropped();
        return s;
}

BMDS_NAMESPACE_END

// Before anything prints: a swap left by a panic while the device ran (a restart undoes its own)
// is undone, so the console port is USB-Serial/JTAG again (docs/platforms/esp32p4.md §4d).
ESP_SYSTEM_INIT_FN(bmds_usb_phys_unswapped, CORE, BIT(0), 1) {
        bmds::detachAndUnswap();
        return ESP_OK;
}

// TinyUSB's device callbacks, on the device's task.
extern "C" {

uint8_t const *tud_descriptor_device_cb(void) {
        return reinterpret_cast<uint8_t const *>(&bmds::DeviceDesc);
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t) {
        return bmds::state->config;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t) {
        using namespace bmds;
        uint16_t *text = state->text;
        size_t    n = 0;
        if (index == 0) {
                text[1] = 0x0409; // English (US)
                n = 1;
        } else {
                char ascii[32] = {};
                switch (index) {
                        case StrManufacturer: std::snprintf(ascii, sizeof(ascii), "BMDS"); break;
                        case StrProduct: {
                                // The board's name, so two boards tell apart.
                                String name(MemSpace::Psram);
                                if (SysConf::get(StringView("system/hostname"), name).isError() || name.isEmpty())
                                        (void)name.assign(StringView("BMDS"));
                                std::snprintf(ascii, sizeof(ascii), "%.*s", int(name.size()), name.data());
                                break;
                        }
                        case StrSerial: std::snprintf(ascii, sizeof(ascii), "%s", System::serial()); break;
                        case StrConsole: std::snprintf(ascii, sizeof(ascii), "BMDS console"); break;
                        default:
                                if (index >= StrCable1 && index < StrCount) {
                                        std::snprintf(ascii, sizeof(ascii), "MIDI %u", unsigned(index - StrCable1 + 1));
                                        break;
                                }
                                return nullptr;
                }
                for (; ascii[n] != '\0' && n < 31; ++n) text[n + 1] = static_cast<uint8_t>(ascii[n]);
        }
        text[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | (2 * n + 2));
        return text;
}

void tud_mount_cb(void) {
        bmds::state->configured.store(true);
        bmds::state->suspended.store(false);
        bmds::detail::usbDeviceMidiStart(bmds::state->cables);
        return;
}

void tud_umount_cb(void) {
        bmds::state->configured.store(false);
        bmds::detail::usbDeviceMidiStop();
        if (bmds::state->dtr.exchange(false)) bmds::hangUpSession();
        return;
}

void tud_suspend_cb(bool) {
        bmds::state->suspended.store(true);
        return;
}

void tud_resume_cb(void) {
        bmds::state->suspended.store(false);
        return;
}

void tud_cdc_line_state_cb(uint8_t, bool dtr, bool) {
        using namespace bmds;
        const bool was = state->dtr.exchange(dtr);
        if (dtr && !was) startSession();
        if (!dtr && was) {
                hangUpSession();
                state->txSpace.release();
        }
        return;
}

void tud_cdc_rx_cb(uint8_t) {
        (void)bmds::feedConsole();
        return;
}

void tud_cdc_tx_complete_cb(uint8_t) {
        bmds::state->txSpace.release();
        return;
}

} // extern "C"
