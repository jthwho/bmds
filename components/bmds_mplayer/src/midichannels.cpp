/**
 * @file      midichannels.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "midichannels.h"

#include <cstring>

BMDS_NAMESPACE_BEGIN

namespace {

        // The controllers it follows (General MIDI's numbers).
        constexpr uint8_t BankMsb = 0;
        constexpr uint8_t Volume = 7;
        constexpr uint8_t Pan = 10;
        constexpr uint8_t Expression = 11;
        constexpr uint8_t BankLsb = 32;
        constexpr uint8_t AllSoundOff = 120;
        constexpr uint8_t ResetControllers = 121;
        constexpr uint8_t AllNotesOff = 123;

} // namespace

MidiChannels::MidiChannels(uint32_t rateHz) : _rateHz(rateHz != 0 ? rateHz : 48000) {
        reset();
}

void MidiChannels::resetChannels() {
        for (Channel &c : _channels) c = Channel();
        std::memset(_velocity, 0, sizeof(_velocity));
        return;
}

void MidiChannels::reset() {
        resetChannels();
        _head = 0;
        _count = 0;
        _overflowed = false;
        _tempo = DefaultTempo;
        _beatsPerBar = 4;
        _beatNote = 4;
        _frame = 0;
        _beats = 0;
        _barStartBeat = 0;
        _barsBefore = 0;
        return;
}

void MidiChannels::add(const Activity &activity) {
        if (_count == QueueSize) {
                _overflowed = true;
                return;
        }
        _queue[(_head + _count) % QueueSize] = activity;
        ++_count;
        return;
}

void MidiChannels::beatsTo(uint64_t frame) {
        if (frame <= _frame) return;
        // Quarter notes at the tempo, in the signature's beats.
        const double quarters = static_cast<double>(frame - _frame) * 1e6 /
                                (static_cast<double>(_tempo) * static_cast<double>(_rateHz));
        _beats += quarters * _beatNote / 4;
        _frame = frame;
        return;
}

void MidiChannels::advanceTo(uint64_t frame) {
        while (_count > 0 && _queue[_head].frame <= frame) {
                const Activity a = _queue[_head];
                _head = (_head + 1) % QueueSize;
                --_count;
                beatsTo(a.frame);
                apply(a);
        }
        beatsTo(frame);
        return;
}

void MidiChannels::noteOff(Channel &c, size_t channel, uint8_t key) {
        if (_velocity[channel][key] == 0) return;
        _velocity[channel][key] = 0;
        if (c.notes > 0) --c.notes;
        c.loudest = 0;
        if (c.notes > 0)
                for (const uint8_t v : _velocity[channel])
                        if (v > c.loudest) c.loudest = v;
        return;
}

void MidiChannels::apply(const Activity &a) {
        const size_t ch = a.channel & 15;
        Channel     &c = _channels[ch];
        switch (a.kind) {
                case Activity::Kind::NoteOn:
                        if (a.b == 0) {
                                noteOff(c, ch, a.a & 127);
                                break;
                        }
                        if (_velocity[ch][a.a & 127] == 0) ++c.notes;
                        _velocity[ch][a.a & 127] = a.b;
                        if (a.b > c.loudest) c.loudest = a.b;
                        if (a.b > c.struck) c.struck = a.b;
                        c.used = true;
                        break;
                case Activity::Kind::NoteOff: noteOff(c, ch, a.a & 127); break;
                case Activity::Kind::Program: c.program = a.a & 127; break;
                case Activity::Kind::Control:
                        switch (a.a) {
                                case BankMsb: c.bankMsb = a.b; break;
                                case BankLsb: c.bankLsb = a.b; break;
                                case Volume: c.volume = a.b; break;
                                case Pan: c.pan = a.b; break;
                                case Expression: c.expression = a.b; break;
                                case ResetControllers:
                                        c.volume = Channel().volume;
                                        c.pan = Channel().pan;
                                        c.expression = Channel().expression;
                                        break;
                                case AllSoundOff:
                                case AllNotesOff:
                                        std::memset(_velocity[ch], 0, sizeof(_velocity[ch]));
                                        c.notes = 0;
                                        c.loudest = 0;
                                        break;
                                default: break;
                        }
                        break;
                case Activity::Kind::Tempo:
                        if (a.value != 0) _tempo = a.value;
                        break;
                case Activity::Kind::TimeSignature:
                        if (a.a != 0 && a.b != 0) {
                                // A signature starts a bar: the bars before it, a part one whole.
                                const double since = _beats - _barStartBeat - 1e-6;
                                if (since > 0) _barsBefore += static_cast<uint32_t>(since / _beatsPerBar) + 1;
                                _beatsPerBar = a.a;
                                _beatNote = a.b;
                                _barStartBeat = _beats;
                        }
                        break;
                case Activity::Kind::Reset: resetChannels(); break;
        }
        return;
}

uint8_t MidiChannels::takeStruck(size_t channel) {
        Channel      &c = _channels[channel];
        const uint8_t s = c.struck;
        c.struck = 0;
        return s;
}

MidiChannels::Position MidiChannels::position() const {
        Position p;
        // A hair over, so a beat landing exactly on its frame counts as reached.
        const double since = _beats - _barStartBeat + 1e-6;
        const auto   whole = static_cast<uint64_t>(since > 0 ? since : 0);
        p.bar = _barsBefore + static_cast<uint32_t>(whole / _beatsPerBar + 1);
        p.beat = static_cast<uint32_t>(whole % _beatsPerBar + 1);
        p.tempo = _tempo;
        p.beatsPerBar = _beatsPerBar;
        p.beatNote = _beatNote;
        return p;
}

BMDS_NAMESPACE_END
