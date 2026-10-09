/**
 * @file      midiusbpacker.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/midievent.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief MIDI events into USB-MIDI 1.0 event packets (`devplan/audio.md`
 *        §21): four bytes each, the cable and Code Index Number first, then
 *        up to three MIDI bytes. One per cable sent to: a SysEx goes out three
 *        bytes a packet, so its bytes wait here between events.
 *
 * Takes whole messages, as `MidiParser` makes them: channel and system
 * common messages, real-time bytes (sent at once, even inside a SysEx), and
 * SysEx pieces. Data bytes outside a SysEx are thrown away.
 *
 * @par Thread Safety
 * Not thread-safe: one per cable, used by one thread at a time.
 */
class MidiUsbPacker {
        public:
                /** @brief The most packets one event makes (two bytes waiting, five more, the last `F7`). */
                static constexpr size_t MaxPackets = 3;

                /** @brief A packet. */
                using Packet = uint8_t[4];

                /**
                 * @brief Packs @p event for cable @p cable (0 to 15).
                 * @return The packets completed, in @p out.
                 * @nonblocking
                 */
                size_t pack(uint8_t cable, const MidiEvent &event, Packet (&out)[MaxPackets]);

                /** @brief Forgets a SysEx under way. */
                void reset() {
                        _inSysex = false;
                        _count = 0;
                        return;
                }

        private:
                bool    _inSysex = false;
                uint8_t _bytes[3] = {}; // a SysEx's bytes not yet in a packet
                uint8_t _count = 0;
};

BMDS_NAMESPACE_END
