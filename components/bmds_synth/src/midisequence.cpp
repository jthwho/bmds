/**
 * @file      midisequence.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A MIDI file's channel messages in time order (devplan/mediaplayer.md §12).
 */

#include <cstring>
#include <tml.h>
#include <bmds/memory.h>
#include <bmds/midisequence.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct MemoryStream {
                        const uint8_t *data;
                        size_t         size;
                        size_t         at;
        };

        int readMemory(void *arg, void *out, unsigned int bytes) {
                auto *m = static_cast<MemoryStream *>(arg);
                if (bytes > m->size - m->at) bytes = static_cast<unsigned int>(m->size - m->at);
                if (out != nullptr) std::memcpy(out, m->data + m->at, bytes); // null: a skip
                m->at += bytes;
                return static_cast<int>(bytes);
        }

        // A TinyMidiLoader message as bytes (false: not a channel message, as a tempo change).
        bool toMessage(const tml_message &m, MidiSequence::Message &out) {
                const auto status = static_cast<uint8_t>(m.type | (m.channel & 0x0f));
                out.ms = m.time;
                switch (m.type) {
                        case TML_NOTE_OFF:
                        case TML_NOTE_ON:
                        case TML_KEY_PRESSURE:
                        case TML_CONTROL_CHANGE:
                                out.length = 3;
                                out.data[0] = status;
                                out.data[1] = static_cast<uint8_t>(m.key) & 0x7f;
                                out.data[2] = static_cast<uint8_t>(m.velocity) & 0x7f;
                                return true;
                        case TML_PROGRAM_CHANGE:
                        case TML_CHANNEL_PRESSURE:
                                out.length = 2;
                                out.data[0] = status;
                                out.data[1] = static_cast<uint8_t>(m.program) & 0x7f;
                                out.data[2] = 0;
                                return true;
                        case TML_PITCH_BEND:
                                out.length = 3;
                                out.data[0] = status;
                                out.data[1] = static_cast<uint8_t>(m.pitch_bend & 0x7f);
                                out.data[2] = static_cast<uint8_t>((m.pitch_bend >> 7) & 0x7f);
                                return true;
                        default: return false;
                }
        }

        // The controllers a seek sets again, as kept per channel (-1: never set).
        constexpr uint8_t Kept[] = {0, 32, 1, 7, 10, 11, 91, 93};

        struct ChannelState {
                        int16_t controller[sizeof(Kept)];
                        int16_t program = -1;
                        int32_t bend = -1;       // 14 bits
                        int16_t rangeSemis = -1; // RPN 0's data entry
                        int16_t rangeCents = -1;
                        uint8_t rpnMsb = 127;
                        uint8_t rpnLsb = 127;
        };

} // namespace

Error MidiSequence::load(ByteSpan file) {
        clear();
        if (file.size() < 14 || file.size() > MaxFileBytes || std::memcmp(file.data(), "MThd", 4) != 0)
                return Error::Invalid;
        MemoryStream memory{file.data(), file.size(), 0};
        tml_stream   stream{&memory, readMemory};
        tml_message *first = tml_load(&stream);
        if (first == nullptr) return Error::Invalid;
        size_t n = 0;
        for (const tml_message *m = first; m != nullptr; m = m->next) {
                Message unused;
                if (toMessage(*m, unused)) ++n;
        }
        if (n == 0) {
                tml_free(first);
                return Error::Invalid;
        }
        _messages = static_cast<Message *>(Memory::alloc(MemSpace::Psram, n * sizeof(Message)));
        if (_messages == nullptr) {
                tml_free(first);
                return Error::NoMem;
        }
        for (const tml_message *m = first; m != nullptr; m = m->next) {
                Message msg;
                if (toMessage(*m, msg)) _messages[_count++] = msg;
        }
        tml_free(first);
        return Error::Ok;
}

void MidiSequence::clear() {
        Memory::free(_messages);
        _messages = nullptr;
        _count = 0;
        return;
}

size_t MidiSequence::indexAt(uint32_t ms) const {
        size_t lo = 0;
        size_t hi = _count;
        while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                if (_messages[mid].ms < ms) {
                        lo = mid + 1;
                } else {
                        hi = mid;
                }
        }
        return lo;
}

size_t MidiSequence::stateAt(uint32_t ms, Span<Message> out) const {
        ChannelState state[16];
        for (ChannelState &c : state)
                for (int16_t &v : c.controller) v = -1;
        const size_t end = indexAt(ms);
        for (size_t i = 0; i < end; ++i) {
                const Message &m = _messages[i];
                ChannelState  &c = state[m.data[0] & 0x0f];
                switch (m.data[0] & 0xf0) {
                        case 0xc0: c.program = m.data[1]; break;
                        case 0xe0: c.bend = m.data[1] | m.data[2] << 7; break;
                        case 0xb0: {
                                const uint8_t number = m.data[1];
                                const uint8_t value = m.data[2];
                                for (size_t k = 0; k < sizeof(Kept); ++k)
                                        if (Kept[k] == number) c.controller[k] = value;
                                if (number == 101) c.rpnMsb = value;
                                if (number == 100) c.rpnLsb = value;
                                if ((number == 6 || number == 38) && c.rpnMsb == 0 && c.rpnLsb == 0)
                                        (number == 6 ? c.rangeSemis : c.rangeCents) = value;
                                if (number == 121) { // reset controllers: the bend and the kept ones but volume and pan
                                        c.bend = -1;
                                        for (size_t k = 0; k < sizeof(Kept); ++k)
                                                if (Kept[k] == 1 || Kept[k] == 11) c.controller[k] = -1;
                                }
                                break;
                        }
                        default: break;
                }
        }
        size_t     n = 0;
        const auto add = [&](uint8_t status, uint8_t a, uint8_t b, uint8_t length) {
                if (n >= out.size() || n >= MaxStateMessages) return;
                Message &m = out[n++];
                m.ms = ms;
                m.length = length;
                m.data[0] = status;
                m.data[1] = a;
                m.data[2] = b;
        };
        for (uint8_t ch = 0; ch < 16; ++ch) {
                const ChannelState &c = state[ch];
                const auto          cc = uint8_t(0xb0 | ch);
                // The bank before the program it selects.
                for (size_t k = 0; k < 2; ++k)
                        if (c.controller[k] >= 0) add(cc, Kept[k], uint8_t(c.controller[k]), 3);
                if (c.program >= 0) add(uint8_t(0xc0 | ch), uint8_t(c.program), 0, 2);
                for (size_t k = 2; k < sizeof(Kept); ++k)
                        if (c.controller[k] >= 0) add(cc, Kept[k], uint8_t(c.controller[k]), 3);
                if (c.rangeSemis >= 0 || c.rangeCents >= 0) {
                        add(cc, 101, 0, 3);
                        add(cc, 100, 0, 3);
                        if (c.rangeSemis >= 0) add(cc, 6, uint8_t(c.rangeSemis), 3);
                        if (c.rangeCents >= 0) add(cc, 38, uint8_t(c.rangeCents), 3);
                        add(cc, 101, 127, 3); // RPN null: data entry stops
                        add(cc, 100, 127, 3);
                }
                if (c.bend >= 0) add(uint8_t(0xe0 | ch), uint8_t(c.bend & 0x7f), uint8_t(c.bend >> 7), 3);
        }
        return n;
}

BMDS_NAMESPACE_END
