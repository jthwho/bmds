/**
 * @file      sinetone.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Tones and the system sounds (devplan/audio.md §16, §17).
 */

#include <cmath>
#include <bmds/sinetone.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr float TwoPi = 6.28318530717958647692f;

        using Note = ToneSequence::Note;

        // The sounds, in SystemSound's order: short, and quiet next to full scale.
        constexpr Note Bell[] = {{880, 120, -1400, 3000}};
        constexpr Note Click[] = {{2400, 6, -2000, 1500}};
        constexpr Note ErrorNotes[] = {{440, 90, -1400, 3000}, {330, 150, -1400, 3000}};
        constexpr Note Notify[] = {{660, 80, -1600, 3000}, {990, 120, -1600, 3000}};

        struct Sound {
                        const char      *name;
                        Span<const Note> notes;
        };

        constexpr Sound Sounds[SystemSoundCount] = {
                {"bell", Span<const Note>(Bell)},
                {"click", Span<const Note>(Click)},
                {"error", Span<const Note>(ErrorNotes)},
                {"notify", Span<const Note>(Notify)},
        };

        uint32_t framesOf(uint64_t us, uint32_t rateHz) {
                return static_cast<uint32_t>(us * rateHz / 1'000'000);
        }

} // namespace

void SineTone::start(uint32_t frequencyHz, uint32_t rateHz, int32_t levelCentiDb, Slots slots, uint32_t fadeUs) {
        const float w = TwoPi * static_cast<float>(frequencyHz) / static_cast<float>(rateHz);
        _re = 1;
        _im = 0;
        _stepRe = std::cos(w);
        _stepIm = std::sin(w);
        _amplitude = std::pow(10.0f, static_cast<float>(levelCentiDb) / 2000.0f);
        const uint32_t fadeFrames = framesOf(fadeUs, rateHz);
        _fadeFrames = fadeFrames == 0 ? 1 : fadeFrames;
        _fadeStep = 1.0f / static_cast<float>(_fadeFrames);
        _fadePosition = 0;
        _fadingIn = true;
        _slots = slots;
        return;
}

void SineTone::fadeOut() {
        _fadingIn = false;
        return;
}

void SineTone::fill(float *mix, size_t frames) {
        if (isFinished()) return;
        const bool left = (_slots & Left) != 0;
        const bool right = (_slots & Right) != 0;
        for (size_t i = 0; i < frames; ++i) {
                // The fade in whole frames, so it ends exactly when it should.
                if (_fadingIn) {
                        if (_fadePosition < _fadeFrames) ++_fadePosition;
                } else if (_fadePosition > 0) {
                        --_fadePosition;
                }
                const float v = _amplitude * (static_cast<float>(_fadePosition) * _fadeStep) * _im;
                if (left) mix[2 * i] += v;
                if (right) mix[2 * i + 1] += v;
                const float re = _re * _stepRe - _im * _stepIm;
                _im = _re * _stepIm + _im * _stepRe;
                _re = re;
        }
        // Keep the phasor on the unit circle (rounding drifts it a little each frame).
        const float k = 1.0f / std::sqrt(_re * _re + _im * _im);
        _re *= k;
        _im *= k;
        return;
}

void ToneSequence::start(Span<const Note> notes, uint32_t rateHz, SineTone::Slots slots) {
        _notes = notes;
        _index = 0;
        _rateHz = rateHz;
        _slots = slots;
        _framesLeft = 0;
        _tone = SineTone();
        next();
        return;
}

void ToneSequence::startTone(uint32_t frequencyHz, uint32_t ms, int32_t centiDb, uint32_t rateHz,
                             SineTone::Slots slots) {
        _notes = Span<const Note>();
        _index = 0;
        _rateHz = rateHz;
        _slots = slots;
        _tone.start(frequencyHz, rateHz, centiDb, slots);
        const uint32_t all = framesOf(static_cast<uint64_t>(ms) * 1000, rateHz);
        const uint32_t fade = framesOf(SineTone::DefaultFadeUs, rateHz);
        _framesLeft = all > fade ? all - fade : 0;
        return;
}

void ToneSequence::next() {
        if (_index >= _notes.size()) return;
        const Note &n = _notes[_index++];
        // A rest: a tone too quiet to hear, for its length.
        _tone.start(n.frequencyHz != 0 ? n.frequencyHz : 1000, _rateHz, n.frequencyHz != 0 ? n.centiDb : -20000, _slots,
                    n.fadeUs);
        const uint32_t all = framesOf(static_cast<uint64_t>(n.ms) * 1000, _rateHz);
        const uint32_t fade = framesOf(n.fadeUs, _rateHz);
        _framesLeft = all > fade ? all - fade : 0;
        return;
}

void ToneSequence::cancel() {
        _index = _notes.size();
        _framesLeft = 0;
        _tone.fadeOut();
        return;
}

bool ToneSequence::fill(float *mix, size_t frames) {
        if (_tone.isFinished()) {
                if (_index >= _notes.size()) return false;
                next();
        }
        size_t done = 0;
        if (_framesLeft > 0) {
                done = frames < _framesLeft ? frames : _framesLeft;
                _tone.fill(mix, done);
                _framesLeft -= static_cast<uint32_t>(done);
        }
        if (done < frames) {
                _tone.fadeOut();
                _tone.fill(mix + 2 * done, frames - done);
        }
        return !isFinished();
}

Span<const ToneSequence::Note> systemSoundNotes(SystemSound sound) {
        const auto i = static_cast<size_t>(sound);
        return i < SystemSoundCount ? Sounds[i].notes : Span<const Note>();
}

const char *systemSoundName(SystemSound sound) {
        const auto i = static_cast<size_t>(sound);
        return i < SystemSoundCount ? Sounds[i].name : "?";
}

BMDS_NAMESPACE_END
