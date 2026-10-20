/**
 * @file      usbdevice_p.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The board as a USB device (devplan/audio.md §23): private to bmds_input.
 * The service and the console are in usbdevice.cpp, the MIDI cables in
 * usbdevicemidi.cpp (with TinyUSB's MIDI device callback).
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace detail {

        /** @brief The device was configured: @p cables MIDI ports made. On its task. */
        void usbDeviceMidiStart(uint8_t cables);

        /** @brief Unconfigured, or stopping: the ports go. On its task. */
        void usbDeviceMidiStop();

        /**
         * @brief Sends what waits, as much as TinyUSB takes. On its task.
         * @return True if packets still wait (TinyUSB's buffer full: try again soon).
         */
        bool usbDeviceMidiDrain();

        /** @brief Returns true while packets wait for room in TinyUSB's buffer. On its task. */
        bool usbDeviceMidiWaiting();

        /** @brief Returns the events dropped by a full ring. */
        uint32_t usbDeviceMidiDropped();

        /** @brief Returns true on the device's task. */
        bool usbDeviceIsOnTask();

        /**
         * @brief Runs @p fn on the device's task (TinyUSB's deferred call), if it runs.
         * @return False if the device isn't running (@p fn not called).
         */
        bool usbDeviceDefer(void (*fn)(void *), void *arg);

} // namespace detail

BMDS_NAMESPACE_END
