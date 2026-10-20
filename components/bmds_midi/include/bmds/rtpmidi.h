/**
 * @file      rtpmidi.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/midievent.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief RTP-MIDI's packets (RFC 6295; `devplan/audio.md` §23): an RTP header
 *        and a MIDI command section, read and written. Portable.
 *
 * Reading turns the command list into MIDI bytes for a `MidiParser`: delta
 * times dropped, running status kept (a command without its status gets the
 * last one), SysEx segments joined (`F0`...`F0`, `F7`...`F0`, `F7`...`F7`),
 * a cancelled segment (`F4`) dropped. The recovery journal is skipped.
 */
namespace RtpMidi {

        /** @brief The RTP payload type AppleMIDI sends (dynamic, 97). */
        constexpr uint8_t PayloadType = 97;

        /** @brief RTP's fixed header. */
        constexpr size_t HeaderBytes = 12;

        /** @brief A received packet's fields. */
        struct Packet {
                        uint16_t sequence = 0;       ///< RTP's sequence number.
                        uint32_t timestamp = 0;      ///< RTP's timestamp (AppleMIDI: 100 µs units).
                        uint32_t ssrc = 0;           ///< The sender.
                        bool     journal = false;    ///< A recovery journal follows the commands (skipped).
                        bool     firstDelta = false; ///< `Z`: the first command has a delta time.
                        bool     phantom = false;    ///< `P`: the first command's status is the last packet's.
                        ByteSpan commands;           ///< The command list.
        };

        /**
         * @brief Reads @p datagram's RTP header and command section header into @p out.
         * @return `Ok`, or `Invalid` (not RTP version 2 with payload type 97, or cut short).
         */
        Error parse(ByteSpan datagram, Packet &out);

        /** @brief Receives MIDI bytes as `commands()` reads them. */
        using ByteSink = void (*)(void *arg, uint8_t byte);

        /**
         * @brief Gives @p packet's commands to @p sink as MIDI bytes. @p status is the
         *        running status, carried from packet to packet (0: none).
         * @return `Ok`, or `Invalid` where the list stopped making sense (what came
         *         before it was given).
         */
        Error commands(const Packet &packet, uint8_t &status, ByteSink sink, void *arg);

        /** @brief The data bytes a status byte takes (SysEx and undefined ones: 0). */
        constexpr size_t dataBytes(uint8_t status) {
                switch (status & 0xf0) {
                        case 0x80:
                        case 0x90:
                        case 0xa0:
                        case 0xb0:
                        case 0xe0: return 2;
                        case 0xc0:
                        case 0xd0: return 1;
                        default: break;
                }
                switch (status) {
                        case 0xf1:
                        case 0xf3: return 1;
                        case 0xf2: return 2;
                        default: return 0;
                }
        }

} // namespace RtpMidi

/**
 * @brief Writes RTP-MIDI packets from events: an RTP header, a command section
 *        with no journal, each command after the first with a delta of 0.
 *
 * A SysEx that comes in pieces goes in one segment while it fits; one left open
 * at the end of a packet ends it with `F0` and goes on in the next with `F7`.
 * A real-time event inside an open SysEx goes inside its segment, as MIDI
 * allows. A message that cuts a SysEx short cancels its segment (`F4`).
 *
 * @par Thread Safety
 * One user at a time.
 */
class RtpMidiWriter {
        public:
                /** @brief The largest packet it writes (well inside an Ethernet frame). */
                static constexpr size_t MaxPacket = 512;

                /** @brief Starts a packet. An open SysEx carries on from the last. */
                void begin(uint16_t sequence, uint32_t timestamp, uint32_t ssrc);

                /**
                 * @brief Adds @p event.
                 * @return False if it doesn't fit: `finish()` this packet and add it to the next.
                 */
                bool add(const MidiEvent &event);

                /** @brief Returns true if no command was added since `begin()`. */
                bool isEmpty() const { return _length == 0; }

                /** @brief Ends the packet (an open SysEx segment with `F0`) and returns it. */
                ByteSpan finish();

                /** @brief Forgets an open SysEx (a new session). */
                void reset();

        private:
                bool put(const uint8_t *bytes, size_t n);
                bool room(size_t n) const { return HeaderRoom + _length + n + 1 <= MaxPacket; } // + F0 or F4

                static constexpr size_t HeaderRoom = RtpMidi::HeaderBytes + 2; // the long command header

                uint8_t _packet[MaxPacket] = {};
                size_t  _length = 0;          // the command list's bytes so far
                bool    _sysexOpen = false;   // a SysEx not ended yet (across packets)
                bool    _segmentOpen = false; // its segment is in this packet
};

BMDS_NAMESPACE_END
