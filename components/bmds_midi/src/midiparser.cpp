/**
 * @file      midiparser.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * MIDI bytes into whole messages (devplan/audio.md §21).
 */

#include <bmds/midiparser.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t SysexStart = 0xf0;
        constexpr uint8_t SysexEnd = 0xf7;
        constexpr uint8_t TuneRequest = 0xf6;
        constexpr uint8_t RealtimeFirst = 0xf8;

} // namespace

int MidiParser::dataBytes(uint8_t status) {
        if (status < 0x80) return -1;
        if (status < 0xc0) return 2; // note off and on, aftertouch, control change
        if (status < 0xe0) return 1; // program change, channel pressure
        if (status < 0xf0) return 2; // pitch bend
        switch (status) {
                case 0xf1: return 1; // MIDI time code quarter frame
                case 0xf2: return 2; // song position
                case 0xf3: return 1; // song select
                case TuneRequest: return 0;
                case 0xf9:
                case 0xfd: return -1;
                default: return status >= RealtimeFirst ? 0 : -1;
        }
}

void MidiParser::reset() {
        _status = 0;
        _have = 0;
        _sysex = false;
        _pieceSize = 0;
        return;
}

void MidiParser::takePiece(MidiEvent &out) {
        out.size = _pieceSize;
        for (uint8_t i = 0; i < _pieceSize; ++i) out.data[i] = _piece[i];
        _pieceSize = 0;
        return;
}

size_t MidiParser::feed(uint8_t byte, MidiEvent (&out)[MaxEvents]) {
        // Real-time: alone, and nothing under way changes.
        if (byte >= RealtimeFirst) {
                if (dataBytes(byte) < 0) {
                        ++_errors;
                        return 0;
                }
                out[0].size = 1;
                out[0].data[0] = byte;
                return 1;
        }
        // Data: into the SysEx or the message under way.
        if (byte < 0x80) {
                if (_sysex) {
                        _piece[_pieceSize++] = byte;
                        if (_pieceSize < MidiEvent::MaxBytes) return 0;
                        takePiece(out[0]);
                        return 1;
                }
                if (_status == 0) {
                        ++_errors;
                        return 0;
                }
                _data[_have++] = byte;
                if (_have < _need) return 0;
                out[0].size = static_cast<uint8_t>(1 + _need);
                out[0].data[0] = _status;
                for (uint8_t i = 0; i < _need; ++i) out[0].data[1 + i] = _data[i];
                _have = 0;
                if (_status >= SysexStart) _status = 0; // system common: no running status
                return 1;
        }
        // A status byte. It ends a SysEx: F7 its own end, any other cuts it (closed with an F7).
        size_t n = 0;
        if (_sysex) {
                _piece[_pieceSize++] = SysexEnd; // at most MaxBytes - 1 were waiting
                takePiece(out[n++]);
                _sysex = false;
                if (byte == SysexEnd) return n;
                ++_errors;
        } else if (byte == SysexEnd) {
                ++_errors;
                _status = 0;
                _have = 0;
                return 0;
        }
        _have = 0;
        if (byte == SysexStart) {
                _status = 0;
                _sysex = true;
                _piece[0] = byte;
                _pieceSize = 1;
                return n;
        }
        const int need = dataBytes(byte);
        if (need < 0) { // F4, F5
                ++_errors;
                _status = 0;
                return n;
        }
        if (need == 0) { // F6
                out[n].size = 1;
                out[n].data[0] = byte;
                _status = 0;
                return n + 1;
        }
        _status = byte;
        _need = static_cast<uint8_t>(need);
        return n;
}

BMDS_NAMESPACE_END
