/**
 * @file      midiparser.h
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
 * @brief MIDI bytes into whole messages (`devplan/audio.md` §21): one per
 *        source, so running status never crosses sources.
 *
 * - Running status: data bytes after a channel message reuse its status.
 *   System common messages (`F1`-`F6`) clear it.
 * - Real-time bytes (`F8`-`FF`) are events of their own wherever they come,
 *   even inside another message or a SysEx, which go on after them.
 * - SysEx (`F0` ... `F7`) comes out in pieces of up to `MidiEvent::MaxBytes`.
 *   Any other status byte ends one (MIDI 1.0 allows it): the parser closes it
 *   with an `F7` of its own, so every SysEx a reader sees ends in `F7`, and
 *   counts a cut.
 * - Thrown away and counted: data bytes with no status, `F7` outside a
 *   SysEx, the undefined `F4`, `F5`, `F9` and `FD`.
 *
 * Only `size` and `data` of the events are set.
 *
 * @par Thread Safety
 * Not thread-safe: one per source, used by one thread at a time.
 */
class MidiParser {
        public:
                /** @brief The most events one byte makes (a SysEx closed, then `F6`). */
                static constexpr size_t MaxEvents = 2;

                /**
                 * @brief Takes @p byte.
                 * @return The number of events it completed, in @p out.
                 * @nonblocking
                 */
                size_t feed(uint8_t byte, MidiEvent (&out)[MaxEvents]);

                /** @brief Forgets a message under way and the running status. */
                void reset();

                /** @brief Returns the bytes thrown away and the SysExes cut, since the last `takeErrors()`. */
                uint32_t errors() const { return _errors; }

                /** @brief Returns `errors()` and sets it to 0. */
                uint32_t takeErrors() {
                        const uint32_t e = _errors;
                        _errors = 0;
                        return e;
                }

                /**
                 * @brief Returns the data bytes the message starting with @p status
                 *        takes: 0 to 2, or -1 for SysEx's `F0` (until `F7`) and for
                 *        bytes that start nothing (data, `F7`, the undefined ones).
                 */
                static int dataBytes(uint8_t status);

        private:
                // A SysEx's piece so far, out as an event.
                void takePiece(MidiEvent &out);

                uint8_t  _status = 0; // the running status, or the system common message under way; 0: none
                uint8_t  _need = 0;   // its data bytes
                uint8_t  _have = 0;   // collected
                uint8_t  _data[2] = {};
                bool     _sysex = false;
                uint8_t  _piece[MidiEvent::MaxBytes] = {};
                uint8_t  _pieceSize = 0;
                uint32_t _errors = 0;
};

BMDS_NAMESPACE_END
