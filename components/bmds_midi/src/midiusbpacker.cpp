/**
 * @file      midiusbpacker.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * MIDI events into USB-MIDI 1.0 event packets (devplan/audio.md §21; the USB
 * Device Class Definition for MIDI Devices 1.0, §4: the Code Index Numbers).
 */

#include <bmds/midiparser.h>
#include <bmds/midiusbpacker.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t CinSystemCommon2 = 0x2; // a 2-byte system common message
        constexpr uint8_t CinSystemCommon3 = 0x3; // a 3-byte one
        constexpr uint8_t CinSysexStart = 0x4;    // a SysEx starts or goes on: 3 bytes
        constexpr uint8_t CinSysexEnd1 = 0x5;     // it ends with 1 byte (also a 1-byte system common)
        constexpr uint8_t CinSingleByte = 0xf;    // one byte (real-time)

        void put(MidiUsbPacker::Packet &p, uint8_t cable, uint8_t cin, uint8_t a, uint8_t b, uint8_t c) {
                p[0] = static_cast<uint8_t>(cable << 4 | cin);
                p[1] = a;
                p[2] = b;
                p[3] = c;
                return;
        }

} // namespace

size_t MidiUsbPacker::pack(uint8_t cable, const MidiEvent &e, Packet (&out)[MaxPackets]) {
        cable &= 0x0f;
        if (e.size == 0) return 0;
        const uint8_t first = e.data[0];
        if (first >= 0xf8) {
                put(out[0], cable, CinSingleByte, first, 0, 0);
                return 1;
        }
        if (first == 0xf0 || (first < 0x80 && _inSysex)) {
                size_t n = 0;
                for (uint8_t i = 0; i < e.size; ++i) {
                        const uint8_t b = e.data[i];
                        if (b == 0xf0) {
                                _inSysex = true;
                                _count = 0;
                        }
                        _bytes[_count++] = b;
                        if (b == 0xf7) {
                                // Ends with 1, 2 or 3 bytes: CIN 5, 6, 7.
                                put(out[n++], cable, static_cast<uint8_t>(CinSysexEnd1 + _count - 1), _bytes[0],
                                    _count > 1 ? _bytes[1] : 0, _count > 2 ? _bytes[2] : 0);
                                reset();
                        } else if (_count == 3) {
                                put(out[n++], cable, CinSysexStart, _bytes[0], _bytes[1], _bytes[2]);
                                _count = 0;
                        }
                }
                return n;
        }
        if (first < 0x80) return 0; // data with no SysEx under way
        reset();                    // a status ends a SysEx (the parser closed it already)
        const int data = MidiParser::dataBytes(first);
        if (data < 0 || e.size < 1 + data) return 0;
        uint8_t cin = 0;
        if (first < 0xf0) {
                cin = first >> 4; // channel messages: the status's top nibble
        } else {
                cin = data == 0 ? CinSysexEnd1 : data == 1 ? CinSystemCommon2 : CinSystemCommon3;
        }
        put(out[0], cable, cin, first, data > 0 ? e.data[1] : 0, data > 1 ? e.data[2] : 0);
        return 1;
}

BMDS_NAMESPACE_END
