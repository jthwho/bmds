/**
 * @file      rtpmidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * RTP-MIDI's packets (RFC 6295 §2-3; devplan/audio.md §23).
 */

#include <cstring>
#include <bmds/rtpmidi.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t SysexStart = 0xf0;
        constexpr uint8_t SysexEnd = 0xf7;
        constexpr uint8_t SysexCancel = 0xf4;

        // The command section header's bits.
        constexpr uint8_t LongHeader = 0x80; // B: a 12-bit length
        constexpr uint8_t Journal = 0x40;    // J
        constexpr uint8_t FirstDelta = 0x20; // Z
        constexpr uint8_t Phantom = 0x10;    // P

        uint16_t be16(const uint8_t *p) {
                return static_cast<uint16_t>((p[0] << 8) | p[1]);
        }

        uint32_t be32(const uint8_t *p) {
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        void putBe16(uint8_t *p, uint16_t v) {
                p[0] = static_cast<uint8_t>(v >> 8);
                p[1] = static_cast<uint8_t>(v);
                return;
        }

        void putBe32(uint8_t *p, uint32_t v) {
                p[0] = static_cast<uint8_t>(v >> 24);
                p[1] = static_cast<uint8_t>(v >> 16);
                p[2] = static_cast<uint8_t>(v >> 8);
                p[3] = static_cast<uint8_t>(v);
                return;
        }

} // namespace

Error RtpMidi::parse(ByteSpan datagram, Packet &out) {
        const uint8_t *d = datagram.data();
        const size_t   size = datagram.size();
        if (size < HeaderBytes + 1 || (d[0] >> 6) != 2 || (d[1] & 0x7f) != PayloadType) return Error::Invalid;
        out.sequence = be16(d + 2);
        out.timestamp = be32(d + 4);
        out.ssrc = be32(d + 8);
        size_t at = HeaderBytes + 4u * (d[0] & 0x0fu); // contributing sources
        if ((d[0] & 0x10) != 0) {                      // a header extension
                if (at + 4 > size) return Error::Invalid;
                at += 4 + 4u * be16(d + at + 2);
        }
        if (at >= size) return Error::Invalid;
        const uint8_t h = d[at++];
        size_t        length = h & 0x0fu;
        if ((h & LongHeader) != 0) {
                if (at >= size) return Error::Invalid;
                length = (length << 8) | d[at++];
        }
        if (at + length > size) return Error::Invalid;
        out.journal = (h & Journal) != 0;
        out.firstDelta = (h & FirstDelta) != 0;
        out.phantom = (h & Phantom) != 0;
        out.commands = ByteSpan(d + at, length);
        return Error::Ok;
}

Error RtpMidi::commands(const Packet &packet, uint8_t &status, ByteSink sink, void *arg) {
        const uint8_t *c = packet.commands.data();
        const size_t   n = packet.commands.size();
        size_t         i = 0;
        bool           first = true;
        while (i < n) {
                if (!first || packet.firstDelta) {
                        // A delta time: 1 to 4 bytes, 7 bits each, the last without its top bit.
                        size_t k = 0;
                        bool   more = true;
                        while (i < n && k < 4 && more) {
                                more = (c[i++] & 0x80) != 0;
                                ++k;
                        }
                        if (more) return Error::Invalid; // too long, or cut off
                        if (i == n) break;               // a delta with no command after it
                }
                first = false;
                const uint8_t b = c[i];
                if (b == SysexStart || b == SysexEnd) {
                        // A SysEx segment, to the byte that ends it: F7 (the end), F0 (more in
                        // another segment), F4 (cancelled).
                        size_t j = i + 1;
                        while (j < n && c[j] != SysexStart && c[j] != SysexEnd && c[j] != SysexCancel) ++j;
                        if (j == n) return Error::Invalid;
                        const uint8_t end = c[j];
                        if (end != SysexCancel) {
                                for (size_t k = i + 1; k < j; ++k)
                                        if (c[k] >= 0x80 && c[k] < 0xf8) return Error::Invalid;
                                if (b == SysexStart) sink(arg, SysexStart);
                                for (size_t k = i + 1; k < j; ++k) sink(arg, c[k]); // data, and real-time bytes
                                if (end == SysexEnd) sink(arg, SysexEnd);
                        }
                        status = 0;
                        i = j + 1;
                        continue;
                }
                uint8_t s = status;
                if ((b & 0x80) != 0) {
                        s = b;
                        ++i;
                } else if (s == 0) {
                        return Error::Invalid; // data with no status
                }
                if (s >= 0xf8) {
                        sink(arg, s); // real-time: running status untouched
                        continue;
                }
                status = s < 0xf0 ? s : 0; // system common ends running status
                const size_t data = dataBytes(s);
                if (i + data > n) return Error::Invalid;
                for (size_t k = 0; k < data; ++k)
                        if ((c[i + k] & 0x80) != 0) return Error::Invalid;
                sink(arg, s);
                for (size_t k = 0; k < data; ++k) sink(arg, c[i + k]);
                i += data;
        }
        return Error::Ok;
}

void RtpMidiWriter::begin(uint16_t sequence, uint32_t timestamp, uint32_t ssrc) {
        _packet[0] = 0x80; // version 2
        _packet[1] = RtpMidi::PayloadType;
        putBe16(_packet + 2, sequence);
        putBe32(_packet + 4, timestamp);
        putBe32(_packet + 8, ssrc);
        _length = 0;
        _segmentOpen = false;
        return;
}

bool RtpMidiWriter::put(const uint8_t *bytes, size_t n) {
        std::memcpy(_packet + HeaderRoom + _length, bytes, n);
        _length += n;
        return true;
}

bool RtpMidiWriter::add(const MidiEvent &event) {
        const size_t n = event.size;
        if (n == 0 || n > MidiEvent::MaxBytes) return true;
        const uint8_t first = event.data[0];
        const bool    realtime = n == 1 && first >= 0xf8;
        const uint8_t zero = 0;
        if (_sysexOpen) {
                if (realtime && _segmentOpen) {
                        if (!room(1)) return false;
                        return put(&first, 1);
                }
                const bool piece = first < 0x80 || first == SysexEnd;
                if (piece) {
                        // The SysEx goes on: in this packet's segment, or a new one (F7 first).
                        const size_t lead = _segmentOpen ? 0 : (_length != 0 ? 2 : 1);
                        if (!room(lead + n)) return false;
                        if (!_segmentOpen) {
                                if (_length != 0) (void)put(&zero, 1);
                                (void)put(&SysexEnd, 1);
                                _segmentOpen = true;
                        }
                        (void)put(event.data, n);
                        if (event.data[n - 1] == SysexEnd) _sysexOpen = _segmentOpen = false;
                        return true;
                }
                if (!realtime) {
                        // Cut short by a message: the SysEx cancelled.
                        if (_segmentOpen) {
                                (void)put(&SysexCancel, 1); // room() kept a byte for it
                        } else {
                                if (!room((_length != 0 ? 1 : 0) + 2)) return false;
                                if (_length != 0) (void)put(&zero, 1);
                                (void)put(&SysexEnd, 1);
                                (void)put(&SysexCancel, 1);
                        }
                        _sysexOpen = _segmentOpen = false;
                }
        }
        // A command, or a SysEx's first piece; a delta of 0 before each after the first.
        if (!room((_length != 0 ? 1 : 0) + n)) return false;
        if (_length != 0) (void)put(&zero, 1);
        (void)put(event.data, n);
        if (first == SysexStart && event.data[n - 1] != SysexEnd) _sysexOpen = _segmentOpen = true;
        return true;
}

ByteSpan RtpMidiWriter::finish() {
        if (_segmentOpen) {
                _packet[HeaderRoom + _length++] = SysexStart; // continued in the next packet
                _segmentOpen = false;
        }
        _packet[RtpMidi::HeaderBytes] = static_cast<uint8_t>(LongHeader | ((_length >> 8) & 0x0f));
        _packet[RtpMidi::HeaderBytes + 1] = static_cast<uint8_t>(_length);
        return ByteSpan(_packet, HeaderRoom + _length);
}

void RtpMidiWriter::reset() {
        _length = 0;
        _sysexOpen = _segmentOpen = false;
        return;
}

BMDS_NAMESPACE_END
