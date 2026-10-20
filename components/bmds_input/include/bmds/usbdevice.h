/**
 * @file      usbdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

class ProgramLauncher;

/**
 * @brief The board as a USB device (`devplan/audio.md` §23): a serial port with a
 *        shell on it, and MIDI cables, to a computer on the console port.
 *
 * While `usb/device` is `midi`, the full-speed OTG controller takes the
 * console port's PHY from USB-Serial/JTAG (the board's `usbDeviceOnConsole`)
 * and TinyUSB's device stack runs on its own thread (`usbd`). The console
 * and JTAG are gone from the port meanwhile; `off` gives them back. The
 * setting is followed live; `usb device on` sets it without saving, so a
 * reboot always brings the console back. The swap survives a software
 * reset and a panic's, so it is cleared at startup and before a restart.
 *
 * - **MIDI:** `usb/device/midi-cables` (1 to 4) cables, each a port
 *   (`MidiRouter::Kind::UsbDevice`, "USB device N") while the computer has
 *   the device configured: its source is what the computer sends, its
 *   destination what goes back.
 * - **The console:** the computer opening the serial port (DTR) starts a
 *   shell on a pseudo-terminal (interactive, not a login); closing it hangs up.
 *
 * Settings: `usb/device` (enum `off|midi`, `off`), `usb/device/midi-cables`
 * (int 1 to 4, 2).
 *
 * @par Thread Safety
 * `init()` once, at boot; `status()` from any task.
 */
namespace UsbDevice {

        /** @brief What `status()` reports. */
        struct Status {
                        bool     supported = false;  ///< The board can be a device.
                        bool     on = false;         ///< The device runs (the PHYs swapped).
                        bool     configured = false; ///< A computer configured it.
                        bool     suspended = false;  ///< The computer suspended it.
                        bool     console = false;    ///< The computer has the serial port open.
                        uint8_t  cables = 0;         ///< MIDI cables (ports while configured).
                        uint32_t midiDropped = 0;    ///< MIDI events dropped (a full ring).
        };

        /**
         * @brief Boot stage: declares the settings and follows them, shells run
         *        through @p launcher (kept). After the program loader and MIDI.
         * @return `Ok` (also on a board that can't be a device), or the error
         *         declaring the settings.
         */
        Error init(ProgramLauncher &launcher);

        /** @brief Returns the state. */
        Status status();

} // namespace UsbDevice

BMDS_NAMESPACE_END
