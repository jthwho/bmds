/**
 * @file      usbnomidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * USB MIDI left out (the recovery firmware, devplan/update.md §4: no MIDI, so no network, which
 * MIDI brings): in place of usbmidi.cpp and usbdevicemidi.cpp. A MIDI device plugged in gets no
 * ports (TinyUSB's own callbacks, which do nothing); as a USB device, no cables.
 */

#include "usbdevice_p.h"
#include "usbmidi.h"

BMDS_NAMESPACE_BEGIN

namespace detail {

        void usbMidiNamed(uint8_t, StringView) {
                return;
        }

        void usbDeviceMidiStart(uint8_t) {
                return;
        }

        void usbDeviceMidiStop() {
                return;
        }

        bool usbDeviceMidiDrain() {
                return false;
        }

        bool usbDeviceMidiWaiting() {
                return false;
        }

        uint32_t usbDeviceMidiDropped() {
                return 0;
        }

} // namespace detail

BMDS_NAMESPACE_END
