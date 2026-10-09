/**
 * @file      usbmidi.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The USB host's MIDI devices (devplan/audio.md §21): private to
 * bmds_input. TinyUSB's MIDI callbacks are in usbmidi.cpp.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace detail {

        /**
         * @brief Device @p address's product string arrived: its MIDI ports, if
         *        any, take the name. On the host's task.
         */
        void usbMidiNamed(uint8_t address, StringView product);

} // namespace detail

BMDS_NAMESPACE_END
