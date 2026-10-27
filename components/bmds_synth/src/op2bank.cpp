/**
 * @file      op2bank.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * DMX's OP2 instrument banks (devplan/audio.md §26), read in place.
 */

#include <cstring>
#include <bmds/op2bank.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr char Magic[] = "#OPL_II#";

        // An operator's six bytes, in the file's order: the 0x20, 0x60, 0x80 and 0xE0 registers,
        // then 0x40's key scale bits (in place, the top two) and its level.
        Op2Bank::Operator readOperator(const uint8_t *p) {
                Op2Bank::Operator op;
                op.character = p[0];
                op.attackDecay = p[1];
                op.sustainRelease = p[2];
                op.waveform = p[3] & 0x07;
                op.keyScale = (p[4] >> 6) & 0x03;
                op.level = p[5] & 0x3f;
                return op;
        }

        // A voice's sixteen bytes: the modulator, the feedback and connection, the carrier, a spare,
        // the note offset (little-endian, signed).
        Op2Bank::Voice readVoice(const uint8_t *p) {
                Op2Bank::Voice v;
                v.modulator = readOperator(p);
                v.feedback = (p[6] >> 1) & 0x07;
                v.additive = (p[6] & 0x01) != 0;
                v.carrier = readOperator(p + 7);
                v.noteOffset = static_cast<int16_t>(p[14] | p[15] << 8);
                return v;
        }

} // namespace

Error Op2Bank::open(ByteSpan data) {
        if (data.size() < MinBytes || std::memcmp(data.data(), Magic, HeaderBytes) != 0) return Error::Invalid;
        _data = data.data();
        _size = data.size();
        return Error::Ok;
}

Op2Bank::Instrument Op2Bank::instrument(size_t index) const {
        Instrument in;
        if (_data == nullptr || index >= Instruments) return in;
        const uint8_t *p = _data + HeaderBytes + index * InstrumentBytes;
        in.flags = static_cast<uint16_t>(p[0] | p[1] << 8);
        in.fineTune = p[2];
        in.fixedNote = p[3] & 0x7f;
        in.voices[0] = readVoice(p + 4);
        in.voices[1] = readVoice(p + 20);
        return in;
}

StringView Op2Bank::name(size_t index) const {
        if (_data == nullptr || index >= Instruments || _size < FullBytes) return StringView();
        const auto *p = reinterpret_cast<const char *>(_data + MinBytes + index * NameBytes);
        size_t      n = 0;
        while (n < NameBytes && p[n] != '\0') ++n;
        return StringView(p, n);
}

BMDS_NAMESPACE_END
