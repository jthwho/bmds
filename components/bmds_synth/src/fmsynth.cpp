/**
 * @file      fmsynth.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A General MIDI synth with an OPL's voices (devplan/audio.md §26). The operators are the
 * chip's in the linear form (the user's choice, 2026-10-27): a 10-bit phase into a waveform's
 * period of the chip's own levels, times a gain; the gain from the attenuations added in the
 * chip's units (the envelope, total level, key scaling, tremolo, the MIDI volume) through its
 * exponent table, every 16 frames and ramped between. The waveforms, feedback, envelope rates,
 * key scaling, tremolo and vibrato as Nuked OPL3 reads the chip. The rest is not: only sounding
 * voices are computed, any number of them, at the output's rate, the pitch from the MIDI note in
 * floating point, and a voice's samples added into its MIDI channel's (panned once a block).
 */

#include <cmath>
#include <cstring>
#include <new>
#include <bmds/fmsynth.h>
#include <bmds/memory.h>
#include "fmtables.h"
#include "synthmath.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr int   MidiChannels = 16;
        constexpr int   DrumChannel = 9; // channel 10
        constexpr float ChipRate = 49716.0f;

        // GM System On: the whole SysEx in one event.
        constexpr uint8_t GmSystemOn[] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};

        // The chip's frequency multiples, doubled (the first is a half).
        constexpr uint8_t Mult2[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};

        // Key scale level: its base by the F-number's top four bits, and the shift each setting
        // takes it by (none, 3, 1.5, 6 dB an octave).
        constexpr uint8_t KslBase[16] = {0, 32, 40, 45, 48, 51, 53, 56, 56, 58, 59, 60, 61, 62, 63, 64};
        constexpr uint8_t KslShift[4] = {8, 1, 2, 0};

        // The fast rates' extra steps, by the rate's low bits, over four samples.
        constexpr uint8_t IncStep[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 0}};

        // An operator's character bits (the 0x20 register) it keeps.
        constexpr uint8_t Tremolo = 0x80;
        constexpr uint8_t Vibrato = 0x40;
        constexpr uint8_t Sustaining = 0x20;
        constexpr uint8_t KeyScaleRate = 0x10;

        // The envelope: 0 (loud) to 511 (silent) in 0.1875 dB steps; the chip calls an operator
        // off from 504.
        constexpr int32_t MaxLevel = 511;
        constexpr float   OffLevel = 504.0f;
        constexpr uint8_t QuickRelease = 60; // all sound off's rate: about 2.7 ms

        // From this attenuation on (78 dB), an operator's output rounds to nothing (a gain of 3 of
        // 32768 on a level of 8168): its gain is 0, and a voice silent for good is freed then, not
        // at the chip's 504 (94.5 dB): the last sixth of every release, voices no one hears.
        constexpr int32_t SilentAtt = 416;

        // A gain under this (16 of 32768, -66 dB) with no step to take is silent.
        constexpr int32_t SilentGain = 16 << 16;

        bool isSilent(int32_t gain) {
                return gain < SilentGain && gain > -SilentGain;
        }

        // A releasing voice (or a percussive one) 48 dB down on its envelope, total level and velocity
        // alone fades out in a few milliseconds (all sound off's rate) and is freed: measured on
        // Freedoom's songs, the voices sounding 25.3 → 20.7 on average and the notes stolen at the
        // limit 1,164 → 424, the sound within 0.04 dB in every 1/3-octave band (2026-10-27).
        constexpr int32_t FadeAtt = 256;

        // Pan gains: 1.0 is 1 << PanBits (a channel's sum of voices at ±8168, times 1024, fits an int32).
        constexpr int PanBits = 10;

        // The LFOs' common period in chip samples (the tremolo's 13,440 and the vibrato's 8,192), in
        // 1/65536s.
        constexpr uint64_t LfoPeriod16 = uint64_t(105 * 8192) << 16;

        // The modulation wheel's deepest vibrato, in cents.
        constexpr float WheelCents = 50.0f;

        // The output's level against the chip's, in dB: a voice at full level is ±4084 of 32768, as
        // one of an OPL3's channels (-18 dBFS). Raised so that a song is as loud as through the
        // wavetable and TimGM6mb: over Freedoom's 35 songs and Bricks', FM is 0.4 dB louder at the
        // median (2026-10-27, both on TinySoundFont's level curves). One gain (synth/gain) for both
        // engines; the limiter keeps the peaks below full scale.
        constexpr float CalibrationDb = 13.0f;


        enum Stage : uint8_t {
                Attack,
                Decay,
                Sustain,
                Release,
                Off,
        };

} // namespace

struct FmSynth::State {
                struct Op {
                                uint32_t       phase = 0;
                                uint32_t       inc = 0;        // a frame, vibrato applied
                                uint32_t       baseInc = 0;    // without it
                                const int16_t *wave = nullptr; // its waveform's period
                                int32_t        gain = 0;       // now (Q31)
                                int32_t        step = 0;       // added each frame, to the tick's target
                                float          level = MaxLevel;
                                uint16_t       fixedAtt = 0; // total level and key scale level (envelope units)
                                uint8_t        stage = Off;
                                uint8_t        mult2 = 2;      // the frequency multiple, doubled
                                uint8_t        attackRate = 0; // 0 to 63, key scaling applied
                                uint8_t        decayRate = 0;
                                uint8_t        releaseRate = 0;
                                uint8_t        sustainLevel = 0; // 0 to 31, × 16
                                uint8_t        flags = 0;        // Tremolo, Vibrato, Sustaining, KeyScaleRate
                };

                struct Voice {
                                Op       op[2];   // the modulator, the carrier
                                int32_t  fb0 = 0; // the modulator's last two outputs
                                int32_t  fb1 = 0;
                                uint32_t age = 0;
                                float    pitch = 0;  // semitones, without the bend
                                uint16_t volAtt = 0; // velocity, volume and expression (envelope units)
                                float    invFnum = 0; // 1 / fnum: vibrato's step, without a division a tick
                                uint16_t fnum = 0;   // the chip's F-number and block for the pitch:
                                uint8_t  block = 0;  // key scaling, vibrato's depth
                                uint8_t  channel = 0;
                                uint8_t  key = 0;
                                uint8_t  velocity = 0;
                                uint8_t  fbShift = 0; // 0: no feedback; else 9 - feedback
                                bool     additive = false;
                                bool     active = false;
                                bool     keyOn = false;
                                bool     held = false; // released while the sustain pedal was down
                                bool     fading = false; // FadeAtt passed: in its quick release
                };

                struct Channel {
                                float   bendSemis = 0; // the bend now, in semitones
                                uint8_t program = 0;
                                uint8_t volume = 100;
                                uint8_t expression = 127;
                                uint8_t pan = 64;
                                uint8_t modulation = 0;
                                bool    sustain = false;
                                uint8_t rpnMsb = 127;
                                uint8_t rpnLsb = 127;
                                uint8_t rangeSemis = 2;
                                uint8_t rangeCents = 0;
                                int16_t bend = 0;    // -8192 to 8191
                                int16_t gainL = 724; // the pan's (the centre's: 1024 cos(pi / 4))
                                int16_t gainR = 724;
                };

                // What a tick works out once for every voice.
                struct Lfo {
                                int32_t tremolo = 0; // envelope units
                                uint8_t vibPos = 0;  // the chip's eight steps
                                float   wheel = 0;   // -1 to 1, the modulation wheel's triangle
                };

                float    attackMul[64] = {}; // a tick's factor on the level + 1, by rate
                float    decayAdd[64] = {};  // a tick's steps, by rate
                float    incScale = 0;       // 2^32 / rate
                float    chipPerFrame = 0;   // chip samples a frame
                uint32_t chipPerFrame16 = 0; // the same in 1/65536s
                uint64_t lfoFrame = 0;       // the LFOs' last frame
                uint64_t lfoChip16 = 0;      // ... and the chip's samples then (1/65536s, modulo their period)
                float    outScale = 0;
                uint32_t rateHz = 48000;
                Channel  channels[MidiChannels];
                const int16_t *waves[8] = {}; // the eight waveforms' periods: open()'s copies, or flash's
                int32_t  mix[BlockFrames * 2];
                int32_t  sum[BlockFrames];        // a channel's voices
                uint8_t  order[SynthEngine::MaxVoices]; // the sounding voices by channel, a block's
                uint32_t ages = 0;
                uint32_t sounding = 0;
                uint16_t voiceCount = 0;
                Voice   *voices = nullptr; // after this, in the same allocation
};

namespace {

        using State = FmSynth::State;

        // ---- The operators

        // A waveform's entry at a phase's byte offset (twice its index, 0 to 0x7fe).
        inline int32_t entry(const int16_t *wave, uint32_t offset) {
                return *reinterpret_cast<const int16_t *>(reinterpret_cast<const uint8_t *>(wave) + offset);
        }

        // An entry (four times the chip's level) at a gain: twice the chip's level (one mulh). Gains
        // are Q31 (the exponent table's 2042 at no attenuation is 0.997 of 1.0): a tick's steps keep
        // their fractions, so a slow change at a low level isn't rounded away (in Q15 a step under
        // one was nothing: a decay ran 11 % slow at 32 frames a tick).
        inline int32_t atGain(int32_t entry, int32_t gain) {
                return int32_t((int64_t(entry) * gain) >> 32);
        }

        // A part of a block between ticks: its frames, and the gains' steps a frame.
        struct Segment {
                        uint32_t frames;
                        int32_t  modStep;
                        int32_t  carStep;
        };

        // A voice's segments' frames added into @p sum. Levels are doubled (the tables'), so a level
        // adds straight to a phase's byte offset: the modulator into the carrier's phase (FM) or
        // into the sum (additive), and its feedback (the average of its last two outputs, as the
        // chip's) into its own when @p Fb. The gains ramp to each tick's targets.
        template <bool Fb, bool Additive>
        void renderVoice(State::Voice &v, int32_t *sum, const Segment *segments, size_t count) {
                State::Op     &mo = v.op[0];
                State::Op     &co = v.op[1];
                const int16_t *mw = mo.wave;
                const int16_t *cw = co.wave;
                uint32_t       mp = mo.phase;
                uint32_t       cp = co.phase;
                const uint32_t mi = mo.inc;
                const uint32_t ci = co.inc;
                int32_t        mg = mo.gain;
                int32_t        cg = co.gain;
                int32_t        o0 = v.fb0;
                int32_t        o1 = v.fb1;
                const unsigned shift = v.fbShift;
                for (size_t k = 0; k < count; ++k) {
                        const int32_t  ms = segments[k].modStep;
                        const int32_t  cs = segments[k].carStep;
                        const uint32_t n = segments[k].frames;
                        for (uint32_t i = 0; i < n; ++i) {
                                mg += ms;
                                cg += cs;
                                const uint32_t fb = Fb ? uint32_t((o0 + o1) >> shift) : 0;
                                const int32_t  m = atGain(entry(mw, ((mp >> 21) + fb) & 0x7fe), mg);
                                mp += mi;
                                if (Fb) {
                                        o0 = o1;
                                        o1 = m;
                                }
                                const uint32_t at = Additive ? (cp >> 21) & 0x7fe : ((cp >> 21) + uint32_t(m)) & 0x7fe;
                                const int32_t  c = atGain(entry(cw, at), cg);
                                cp += ci;
                                *sum++ += Additive ? c + m : c;
                        }
                }
                mo.phase = mp;
                co.phase = cp;
                mo.gain = mg;
                co.gain = cg;
                v.fb0 = o0;
                v.fb1 = o1;
                return;
        }

        // A voice whose modulator is silent the whole block: its carrier alone (no modulation, nothing
        // added), the modulator's phase moved on and its feedback emptied.
        void renderCarrier(State::Voice &v, int32_t *sum, const Segment *segments, size_t count) {
                State::Op     &mo = v.op[0];
                State::Op     &co = v.op[1];
                const int16_t *cw = co.wave;
                uint32_t       cp = co.phase;
                const uint32_t ci = co.inc;
                int32_t        cg = co.gain;
                uint32_t       frames = 0;
                for (size_t k = 0; k < count; ++k) {
                        const int32_t  cs = segments[k].carStep;
                        const uint32_t n = segments[k].frames;
                        for (uint32_t i = 0; i < n; ++i) {
                                cg += cs;
                                *sum++ += atGain(entry(cw, (cp >> 21) & 0x7fe), cg);
                                cp += ci;
                        }
                        frames += n;
                }
                co.phase = cp;
                co.gain = cg;
                mo.phase += mo.inc * frames;
                v.fb0 = v.fb1 = 0;
                return;
        }

        // A voice silent the whole block: its phases moved on, nothing computed.
        void skipVoice(State::Voice &v, const Segment *segments, size_t count) {
                uint32_t frames = 0;
                for (size_t k = 0; k < count; ++k) frames += segments[k].frames;
                for (State::Op &op : v.op) op.phase += op.inc * frames;
                v.fb0 = v.fb1 = 0;
                return;
        }

        using RenderFn = void (*)(State::Voice &, int32_t *, const Segment *, size_t);

        // By feedback and connection.
        constexpr RenderFn Renderers[2][2] = {
                {renderVoice<false, false>, renderVoice<false, true>},
                {renderVoice<true, false>, renderVoice<true, true>},
        };

        // ---- The envelopes

        // Rate @p rate's average change a chip sample: its decay's steps, and its attack's factor
        // on the level + 1 as a base-2 logarithm (the chip's schedule, Nuked OPL3's reading).
        void rateAverages(unsigned rate, float &steps, float &attackLog2) {
                steps = 0;
                attackLog2 = 0;
                if (rate == 0) return;
                const unsigned hi = rate >> 2;
                const unsigned lo = rate & 3;
                const float    eighth = std::log2(7.0f / 8.0f); // an attack step at shift 1
                if (hi < 12) {
                        // A step of 1 now and then: 2^(hi - 13) a sample, more by the low bits.
                        steps = std::ldexp(float(4 + lo) / 4.0f, int(hi) - 13);
                        attackLog2 = steps * eighth;
                        return;
                }
                if (hi == 15) {
                        steps = 4; // the attack is instant
                        return;
                }
                for (unsigned k = 0; k < 4; ++k) {
                        const unsigned shift = (hi & 3) + IncStep[lo][k];
                        if (shift == 0) {
                                // Every other sample, a step of 1.
                                steps += 0.5f;
                                attackLog2 += 0.5f * eighth;
                        } else {
                                steps += float(1u << (shift - 1));
                                attackLog2 += std::log2(1.0f - float(1u << shift) / 16.0f);
                        }
                }
                steps /= 4;
                attackLog2 /= 4;
                return;
        }

        void advance(const State &s, State::Op &op) {
                switch (op.stage) {
                        case Attack:
                                if (op.attackRate >= 60) {
                                        op.level = 0;
                                } else {
                                        op.level = (op.level + 1.0f) * s.attackMul[op.attackRate] - 1.0f;
                                }
                                if (op.level <= 0) {
                                        op.level = 0;
                                        op.stage = Decay;
                                }
                                // Not off for being near silence: an attack starts there (a slow one
                                // stayed above OffLevel for its first tick and was taken for gone, so
                                // strings and pads never sounded; found 2026-10-27).
                                return;
                        case Decay:
                                // The chip stops within a step of the sustain level; a tick's steps
                                // would overshoot it (at the fastest rate by 66, 12 dB).
                                op.level += s.decayAdd[op.decayRate];
                                if (op.level >= float(op.sustainLevel * 16)) {
                                        op.level = float(op.sustainLevel * 16);
                                        op.stage = Sustain;
                                }
                                break;
                        case Sustain:
                                // A percussive operator goes on falling, at its release rate.
                                if ((op.flags & Sustaining) == 0) op.level += s.decayAdd[op.releaseRate];
                                break;
                        case Release: op.level += s.decayAdd[op.releaseRate]; break;
                        default: return;
                }
                if (op.level >= OffLevel) {
                        op.level = MaxLevel;
                        op.stage = Off;
                }
                return;
        }

        // ---- Pitch

        // The chip's F-number and block for @p hz, as DMX's driver chose them (the F-number in
        // 307-613, an octave): what key scaling and vibrato's depth go by.
        void fnumber(float hz, uint16_t &fnum, uint8_t &block) {
                float   f = hz * float(1u << 20) / ChipRate;
                uint8_t b = 0;
                while (f >= 614.0f && b < 7) {
                        f *= 0.5f;
                        ++b;
                }
                fnum = f > 1023.0f ? 1023 : uint16_t(f);
                block = b;
                return;
        }

        // The chip's vibrato: its eight steps, a fraction of the F-number (depth 7 cents or so,
        // DMX's setting of the shallow one).
        int32_t vibratoOffset(uint16_t fnum, uint8_t pos) {
                int32_t range = (fnum >> 7) & 7;
                if ((pos & 3) == 0) {
                        range = 0;
                } else if ((pos & 1) != 0) {
                        range >>= 1;
                }
                range >>= 1; // the shallow depth
                return (pos & 4) != 0 ? -range : range;
        }

} // namespace

// ---- Voices and channels

namespace {

        // The LFOs at @p frame: the chip's tremolo (3.7 Hz, the shallow depth, as DMX set it) and
        // vibrato (6.1 Hz, eight steps), and the modulation wheel's triangle at the vibrato's rate.
        State::Lfo lfoAt(State &s, uint64_t frame) {
                // The chip's sample count, moved on from the last call's (frames only go forward), in
                // 1/65536s, modulo the LFOs' common period: no 64-bit division.
                if (frame >= s.lfoFrame) {
                        s.lfoChip16 += (frame - s.lfoFrame) * s.chipPerFrame16;
                } else {
                        s.lfoChip16 = 0;
                }
                s.lfoFrame = frame;
                while (s.lfoChip16 >= LfoPeriod16) s.lfoChip16 -= LfoPeriod16;
                const uint32_t chip = uint32_t(s.lfoChip16 >> 16);
                State::Lfo     lfo;
                const uint32_t trem = uint32_t((chip >> 6) % 210);
                lfo.tremolo = int32_t((trem < 105 ? trem : 210 - trem) >> 4);
                lfo.vibPos = uint8_t((chip >> 10) & 7);
                const uint32_t w = uint32_t(chip & 8191);
                lfo.wheel = float(w < 4096 ? w : 8192 - w) / 2048.0f - 1.0f;
                return lfo;
        }

        // The velocity's, the volume's and the expression's attenuation: TinySoundFont's curves (the
        // velocity's amplitude, the volume and expression's product cubed), so a song's balance and
        // a game's volume (controller 7) sound alike through either engine.
        uint16_t volumeAtt(const State::Channel &c, uint8_t velocity) {
                const uint32_t a = uint32_t(FmTables::LevelAtt[velocity & 0x7f]) +
                                   3u * (uint32_t(FmTables::LevelAtt[c.volume]) + FmTables::LevelAtt[c.expression]);
                return uint16_t(a > MaxLevel ? MaxLevel : a);
        }

        // Equal power: 1 hard left, 64 the centre, 127 hard right (0 as 1).
        void panGains(State::Channel &c) {
                const float x = float(c.pan > 1 ? c.pan - 1 : 0) / 126.0f * 1.5707963f;
                c.gainL = int16_t(std::lround(std::cos(x) * float(1 << PanBits)));
                c.gainR = int16_t(std::lround(std::sin(x) * float(1 << PanBits)));
                return;
        }

        // The voice's pitch with the channel's bend: its F-number and block, and its operators'
        // increments (vibrato off until the next tick).
        void tune(const State &s, State::Voice &v) {
                const float semis = v.pitch + s.channels[v.channel].bendSemis;
                const float hz = 440.0f * bmdsSynthExp2((semis - 69.0f) / 12.0f);
                fnumber(hz, v.fnum, v.block);
                v.invFnum = v.fnum != 0 ? 1.0f / float(v.fnum) : 0.0f;
                for (State::Op &op : v.op) {
                        const float inc = hz * float(op.mult2) * 0.5f * s.incScale;
                        op.baseInc = op.inc = inc >= 2147483648.0f ? 0x80000000u : uint32_t(inc);
                }
                return;
        }

        // An operator from the bank's registers, keyed on: the phase at 0, the attack from silence.
        void initOp(const State &s, State::Op &op, const Op2Bank::Operator &o) {
                op = State::Op();
                op.flags = o.character & 0xf0;
                op.mult2 = Mult2[o.character & 0x0f];
                op.wave = s.waves[o.waveform & 0x07];
                const unsigned sl = o.sustainRelease >> 4;
                op.sustainLevel = uint8_t(sl == 15 ? 31 : sl);
                op.stage = Attack;
                op.level = MaxLevel;
                op.gain = 0;
                return;
        }

        // The rates and the fixed attenuation, which key scaling takes from the F-number and block.
        void scaleOp(State::Op &op, const Op2Bank::Operator &o, uint16_t fnum, uint8_t block) {
                unsigned ksv = unsigned(block) << 1 | ((fnum >> 8) & 1); // the note select DMX set
                if ((op.flags & KeyScaleRate) == 0) ksv >>= 2;
                const auto rate = [ksv](unsigned r) {
                        const unsigned v = r * 4 + ksv;
                        return uint8_t(r == 0 ? 0 : v > 63 ? 63 : v);
                };
                op.attackRate = rate(o.attackDecay >> 4);
                op.decayRate = rate(o.attackDecay & 0x0f);
                op.releaseRate = rate(o.sustainRelease & 0x0f);
                int ksl = (KslBase[fnum >> 6] << 2) - ((8 - block) << 5);
                if (ksl < 0) ksl = 0;
                op.fixedAtt = uint16_t(o.level * 4 + (ksl >> KslShift[o.keyScale & 3]));
                return;
        }

        // A voice's tick: its envelopes advanced and their targets for the next 16 frames (the
        // carrier, and an additive voice's modulator, at the voice's volume), the vibrato. Returns
        // false once it is silent.
        // True once @p op is silent for good: its envelope and total level alone past `SilentAtt` (the
        // volume could come back, the envelope can't) in a stage that only falls, its gain at 0.
        bool goneForGood(const State::Op &op) {
                const bool falls = op.stage == Off || op.stage == Release || op.stage == Decay ||
                                   (op.stage == Sustain && (op.flags & Sustaining) == 0);
                return falls && op.step == 0 && isSilent(op.gain) && int32_t(op.level) + op.fixedAtt >= SilentAtt;
        }

        // Past FadeAtt for good: releasing (or percussive) and quiet enough on what can't change.
        bool fadedOut(const State::Op &op, int32_t velocityAtt) {
                if (op.stage == Off) return true;
                const bool falls = op.stage == Release || (op.stage == Sustain && (op.flags & Sustaining) == 0);
                return falls && int32_t(op.level) + op.fixedAtt + velocityAtt >= FadeAtt;
        }

        void quickRelease(State::Voice &v);

        bool tickVoice(const State &s, State::Voice &v, const State::Lfo &lfo) {
                const State::Channel &c = s.channels[v.channel];
                for (int k = 0; k < 2; ++k) {
                        State::Op &op = v.op[k];
                        advance(s, op);
                        int32_t a = int32_t(op.level) + op.fixedAtt;
                        if ((op.flags & Tremolo) != 0) a += lfo.tremolo;
                        if (k == 1 || v.additive) a += v.volAtt;
                        // The chip's exponent table: 0.1875 dB steps, 8 of its 1/256 octaves; nothing
                        // from SilentAtt on.
                        const uint32_t l = uint32_t(a) << 3;
                        const int32_t  target =
                                a >= SilentAtt ? 0
                                                : int32_t(((uint32_t(FmTables::Exp[l & 0xff]) << 4) >> (l >> 8)) << 16);
                        op.step = (target - op.gain) / int32_t(FmSynth::TickFrames);
                }
                // Heard no more: the carrier (and an additive voice's modulator) silent for good.
                if (goneForGood(v.op[1]) && (!v.additive || goneForGood(v.op[0]))) return false;
                if (!v.fading) {
                        const int32_t velocityAtt = FmTables::LevelAtt[v.velocity & 0x7f];
                        if (fadedOut(v.op[1], velocityAtt) && (!v.additive || fadedOut(v.op[0], velocityAtt))) {
                                quickRelease(v);
                                v.fading = true;
                        }
                }
                if (c.modulation != 0 || ((v.op[0].flags | v.op[1].flags) & Vibrato) != 0) {
                        const float wheel =
                                c.modulation != 0
                                        ? bmdsSynthExp2(WheelCents / 1200.0f * float(c.modulation) / 127.0f * lfo.wheel)
                                        : 1.0f;
                        const float chip = 1.0f + float(vibratoOffset(v.fnum, lfo.vibPos)) * v.invFnum;
                        for (State::Op &op : v.op)
                                op.inc = uint32_t(float(op.baseInc) *
                                                  ((op.flags & Vibrato) != 0 ? chip * wheel : wheel));
                }
                return true;
        }

        void release(State::Voice &v) {
                v.keyOn = false;
                v.held = false;
                for (State::Op &op : v.op)
                        if (op.stage != Off) op.stage = Release;
                return;
        }

        // All sound off's: a release of a few milliseconds.
        void quickRelease(State::Voice &v) {
                for (State::Op &op : v.op) op.releaseRate = QuickRelease;
                release(v);
                return;
        }

        // A voice for a note: a free one, else the quietest released one, else the oldest; never
        // one of the note's own (from @p firstAge on).
        State::Voice &allocate(State &s, uint32_t firstAge) {
                State::Voice *released = nullptr;
                State::Voice *oldest = nullptr;
                for (size_t i = 0; i < s.voiceCount; ++i) {
                        State::Voice &v = s.voices[i];
                        if (!v.active) return v;
                        if (v.age >= firstAge) continue;
                        if (!v.keyOn && !v.held) {
                                if (released == nullptr || v.op[1].level > released->op[1].level ||
                                    (v.op[1].level == released->op[1].level && v.age < released->age))
                                        released = &v;
                        } else if (oldest == nullptr || v.age < oldest->age) {
                                oldest = &v;
                        }
                }
                State::Voice &v = released != nullptr ? *released : *oldest;
                v.active = false;
                --s.sounding;
                return v;
        }

        void noteOn(State &s, const Op2Bank &bank, FmSynth::Stats &stats, uint64_t frame, uint8_t ch, uint8_t key,
                    uint8_t velocity) {
                const State::Channel &c = s.channels[ch];
                const size_t          index = ch == DrumChannel ? Op2Bank::drumIndex(key) : c.program;
                if (index >= Op2Bank::Instruments) return;
                const Op2Bank::Instrument in = bank.instrument(index);
                const size_t              count = (in.flags & Op2Bank::DoubleVoice) != 0 ? 2 : 1;
                if (s.sounding + count > s.voiceCount) ++stats.notesAtLimit;
                const uint32_t   firstAge = s.ages + 1;
                const State::Lfo lfo = lfoAt(s, frame);
                for (size_t k = 0; k < count && k < s.voiceCount; ++k) {
                        State::Voice         &v = allocate(s, firstAge);
                        const Op2Bank::Voice &bv = in.voices[k];
                        v = State::Voice();
                        v.channel = ch;
                        v.key = key;
                        v.velocity = velocity;
                        v.age = ++s.ages;
                        v.active = true;
                        v.keyOn = true;
                        ++s.sounding;
                        v.pitch = (in.flags & Op2Bank::FixedPitch) != 0 ? float(in.fixedNote)
                                                                        : float(int(key) + bv.noteOffset);
                        if (k == 1) v.pitch += float(int(in.fineTune / 2) - 64) / 32.0f; // DMX's detune
                        v.fbShift = uint8_t(bv.feedback != 0 ? 9 - bv.feedback : 0);
                        v.additive = bv.additive;
                        initOp(s, v.op[0], bv.modulator);
                        initOp(s, v.op[1], bv.carrier);
                        tune(s, v);
                        scaleOp(v.op[0], bv.modulator, v.fnum, v.block);
                        scaleOp(v.op[1], bv.carrier, v.fnum, v.block);
                        v.volAtt = volumeAtt(c, velocity);
                        (void)tickVoice(s, v, lfo);
                }
                return;
        }

        void noteOff(State &s, uint8_t ch, uint8_t key) {
                const bool sustain = s.channels[ch].sustain;
                for (size_t i = 0; i < s.voiceCount; ++i) {
                        State::Voice &v = s.voices[i];
                        if (!v.active || !v.keyOn || v.channel != ch || v.key != key) continue;
                        if (sustain) {
                                v.keyOn = false;
                                v.held = true;
                        } else {
                                release(v);
                        }
                }
                return;
        }

        void bendChanged(State &s, uint8_t ch) {
                State::Channel &c = s.channels[ch];
                c.bendSemis = float(c.bend) / 8192.0f * (float(c.rangeSemis) + float(c.rangeCents) / 100.0f);
                for (size_t i = 0; i < s.voiceCount; ++i)
                        if (s.voices[i].active && s.voices[i].channel == ch) tune(s, s.voices[i]);
                return;
        }

        void control(State &s, uint8_t ch, uint8_t number, uint8_t value) {
                State::Channel &c = s.channels[ch];
                switch (number) {
                        case 1: c.modulation = value; return;
                        case 6: // data entry: RPN 0, the bend's range
                        case 38:
                                if (c.rpnMsb != 0 || c.rpnLsb != 0) return;
                                if (number == 6) {
                                        c.rangeSemis = value > 24 ? 24 : value;
                                } else {
                                        c.rangeCents = value > 99 ? 99 : value;
                                }
                                bendChanged(s, ch);
                                return;
                        case 7:
                        case 11:
                                (number == 7 ? c.volume : c.expression) = value;
                                for (size_t i = 0; i < s.voiceCount; ++i) {
                                        State::Voice &v = s.voices[i];
                                        if (v.active && v.channel == ch) v.volAtt = volumeAtt(c, v.velocity);
                                }
                                return;
                        case 10:
                                c.pan = value;
                                panGains(c);
                                return;
                        case 64:
                                c.sustain = value >= 64;
                                if (!c.sustain) {
                                        for (size_t i = 0; i < s.voiceCount; ++i) {
                                                State::Voice &v = s.voices[i];
                                                if (v.active && v.held && v.channel == ch) release(v);
                                        }
                                }
                                return;
                        case 98: // NRPN: none taken, and RPN data entry stops
                        case 99: c.rpnMsb = c.rpnLsb = 127; return;
                        case 100: c.rpnLsb = value; return;
                        case 101: c.rpnMsb = value; return;
                        case 120: // all sound off
                                for (size_t i = 0; i < s.voiceCount; ++i)
                                        if (s.voices[i].active && s.voices[i].channel == ch) quickRelease(s.voices[i]);
                                return;
                        case 121: // reset all controllers
                                c.modulation = 0;
                                c.expression = 127;
                                c.bend = 0;
                                c.rpnMsb = c.rpnLsb = 127;
                                control(s, ch, 64, 0);
                                control(s, ch, 11, 127);
                                bendChanged(s, ch);
                                return;
                        case 123: // all notes off (and the mode changes, which imply it)
                        case 124:
                        case 125:
                        case 126:
                        case 127:
                                for (size_t i = 0; i < s.voiceCount; ++i) {
                                        State::Voice &v = s.voices[i];
                                        if (v.active && v.keyOn && v.channel == ch) noteOff(s, ch, v.key);
                                }
                                return;
                        default: return; // bank select and the rest: taken, ignored
                }
        }

        void resetAll(State &s) {
                for (size_t i = 0; i < s.voiceCount; ++i)
                        if (s.voices[i].active) quickRelease(s.voices[i]);
                for (State::Channel &c : s.channels) c = State::Channel();
                return;
        }

        // @p n frames (at most a block) from frame @p pos into @p out: each channel's voices summed,
        // then panned into the mix.
        // Returns the block's peak (the limiter's to know).
        float renderBlock(State &s, float *out, uint64_t pos, size_t n, uint8_t channels) {
                std::memset(s.mix, 0, n * channels * sizeof(int32_t));
                // The ticks in it: where, and the LFOs there.
                constexpr size_t MaxTicks = FmSynth::BlockFrames / FmSynth::TickFrames + 1;
                size_t           tickAt[MaxTicks];
                State::Lfo       lfos[MaxTicks];
                size_t           ticks = 0;
                for (size_t off = (FmSynth::TickFrames - pos % FmSynth::TickFrames) % FmSynth::TickFrames; off < n;
                     off += FmSynth::TickFrames) {
                        tickAt[ticks] = off;
                        lfos[ticks] = lfoAt(s, pos + off);
                        ++ticks;
                }
                // The sounding voices by their channel's pan (a counting sort): channels with the same
                // pan (most are at the centre) are summed together and panned once; in mono, all.
                uint8_t  panOf[MidiChannels];
                int16_t  panL[MidiChannels];
                int16_t  panR[MidiChannels];
                uint8_t  pans = 0;
                uint16_t first[MidiChannels + 1] = {};
                uint16_t used = 0; // the channels with voices
                for (size_t i = 0; i < s.voiceCount; ++i)
                        if (s.voices[i].active) used = uint16_t(used | 1u << s.voices[i].channel);
                for (int c = 0; c < MidiChannels; ++c) {
                        if ((used & (1u << c)) == 0) continue;
                        const int16_t l = channels == 2 ? s.channels[c].gainL : 0;
                        const int16_t r = channels == 2 ? s.channels[c].gainR : 0;
                        uint8_t       b = 0;
                        while (b < pans && (panL[b] != l || panR[b] != r)) ++b;
                        if (b == pans) {
                                panL[b] = l;
                                panR[b] = r;
                                ++pans;
                        }
                        panOf[c] = b;
                }
                for (size_t i = 0; i < s.voiceCount; ++i)
                        if (s.voices[i].active) ++first[panOf[s.voices[i].channel] + 1];
                for (int b = 0; b < pans; ++b) first[b + 1] += first[b];
                uint16_t next[MidiChannels];
                std::memcpy(next, first, sizeof(next));
                for (size_t i = 0; i < s.voiceCount; ++i)
                        if (s.voices[i].active) s.order[next[panOf[s.voices[i].channel]]++] = uint8_t(i);
                for (int c = 0; c < pans; ++c) {
                        if (first[c] == first[c + 1]) continue;
                        std::memset(s.sum, 0, n * sizeof(int32_t));
                        for (uint16_t k = first[c]; k < first[c + 1]; ++k) {
                                State::Voice &v = s.voices[s.order[k]];
                                // The ticks first, the gains moved on as the frames will move them (a
                                // tick's steps go from where the last left them), then the frames at once.
                                const int32_t startGain[2] = {v.op[0].gain, v.op[1].gain};
                                Segment       segments[MaxTicks + 1];
                                size_t        count = 0;
                                size_t        off = 0;
                                size_t        t = 0;
                                bool          alive = true;
                                while (off < n) {
                                        if (t < ticks && tickAt[t] == off) {
                                                alive = tickVoice(s, v, lfos[t]);
                                                ++t;
                                                if (!alive) break;
                                        }
                                        const size_t end = t < ticks ? tickAt[t] : n;
                                        const auto   frames = uint32_t(end - off);
                                        segments[count++] = {frames, v.op[0].step, v.op[1].step};
                                        v.op[0].gain += v.op[0].step * int32_t(frames);
                                        v.op[1].gain += v.op[1].step * int32_t(frames);
                                        off = end;
                                }
                                v.op[0].gain = startGain[0];
                                v.op[1].gain = startGain[1];
                                // An operator silent the whole block (its gain 0 and staying so) isn't
                                // computed: the carrier alone, or nothing.
                                bool modSilent = isSilent(startGain[0]);
                                bool carSilent = isSilent(startGain[1]);
                                for (size_t i = 0; i < count; ++i) {
                                        modSilent = modSilent && segments[i].modStep == 0;
                                        carSilent = carSilent && segments[i].carStep == 0;
                                }
                                if (carSilent && (modSilent || !v.additive)) {
                                        skipVoice(v, segments, count);
                                } else if (modSilent) {
                                        renderCarrier(v, s.sum, segments, count);
                                } else {
                                        Renderers[v.fbShift != 0][v.additive](v, s.sum, segments, count);
                                }
                                if (!alive) {
                                        v.active = false;
                                        --s.sounding;
                                }
                        }
                        if (channels == 2) {
                                const int32_t gl = panL[c];
                                const int32_t gr = panR[c];
                                for (size_t k = 0; k < n; ++k) {
                                        s.mix[2 * k] += s.sum[k] * gl;
                                        s.mix[2 * k + 1] += s.sum[k] * gr;
                                }
                        } else {
                                for (size_t k = 0; k < n; ++k) s.mix[k] += s.sum[k] << PanBits;
                        }
                }
                float peak = 0.0f;
                for (size_t k = 0; k < n * channels; ++k) {
                        out[k] = float(s.mix[k]) * s.outScale;
                        peak = std::fmax(peak, std::fabs(out[k]));
                }
                return peak;
        }

} // namespace

FmSynth::~FmSynth() {
        close();
}

Error FmSynth::open(const Op2Bank &bank, const Config &config) {
        if (_state != nullptr) return Error::InvalidState;
        if (!bank.isOpen()) return Error::Invalid;
        Config c = config;
        if (c.maxVoices == 0) c.maxVoices = SynthEngine::DefaultVoices;
        if (c.rateHz < 8000 || c.rateHz > 96000 || c.channels < 1 || c.channels > 2 ||
            c.maxVoices > SynthEngine::MaxVoices ||
            !(c.gainDb >= SynthEngine::MinGainDb && c.gainDb <= SynthEngine::MaxGainDb))
                return Error::Invalid;
        _events = static_cast<Event *>(Memory::alloc(MemSpace::Psram, SynthEngine::QueueEvents * sizeof(Event)));
        if (_events == nullptr) return Error::NoMem;
        const size_t bytes = sizeof(State) + c.maxVoices * sizeof(State::Voice);
        // In the TCM when it has room (no cache between: a game's memory traffic on the other core,
        // which the cores' shared L1 data cache feels, doesn't reach it), else internal RAM.
        void *mem = Memory::alloc(MemSpace::Fast, bytes);
        if (mem == nullptr) mem = Memory::alloc(MemSpace::Internal, bytes);
        if (mem == nullptr) {
                close();
                return Error::NoMem;
        }
        _state = ::new (mem) State();
        State &s = *_state;
        s.voiceCount = c.maxVoices;
        s.voices = reinterpret_cast<State::Voice *>(static_cast<uint8_t *>(mem) + sizeof(State));
        // The waveforms (8 × 2 KB) in internal RAM while playing, each from flash only without room
        // for it: read from flash they go through the caches a drawing program's PSRAM traffic
        // churns, and rendering stalled on them (FM took 35-40% of a core beside BMDS Demo #1,
        // devplan/demo.md §10). A block each, so a fragmented heap still takes them. Given back by
        // close(): the service closes the synth after 5 s of quiet.
        for (unsigned w = 0; w < 8; ++w) {
                auto *copy =
                        static_cast<int16_t *>(Memory::alloc(MemSpace::Internal, sizeof(FmTables::Waves.level[w])));
                if (copy != nullptr) std::memcpy(copy, FmTables::Waves.level[w], sizeof(FmTables::Waves.level[w]));
                _waves[w] = copy;
                s.waves[w] = copy != nullptr ? copy : FmTables::Waves.level[w];
        }

        for (size_t i = 0; i < s.voiceCount; ++i) ::new (&s.voices[i]) State::Voice();
        s.rateHz = c.rateHz;
        s.incScale = 4294967296.0f / float(c.rateHz);
        s.chipPerFrame = ChipRate / float(c.rateHz);
        s.chipPerFrame16 = uint32_t(std::lround(s.chipPerFrame * 65536.0f));
        const float tick = float(TickFrames) * s.chipPerFrame;
        for (unsigned r = 0; r < 64; ++r) {
                float steps = 0;
                float attackLog2 = 0;
                rateAverages(r, steps, attackLog2);
                s.decayAdd[r] = steps * tick;
                s.attackMul[r] = bmdsSynthExp2(attackLog2 * tick);
        }
        // The levels are doubled (the wave tables').
        s.outScale = bmdsSynthExp2((c.gainDb + CalibrationDb) / 6.0206f) / (2.0f * float(1 << PanBits) * 32768.0f);
        _bank = &bank;
        _config = c;
        _limiter.setup(c.rateHz);
        _head = _count = 0;
        _frames = 0;
        _stats = Stats();
        return Error::Ok;
}

size_t FmSynth::wavesInFlash() const {
        size_t n = 0;
        for (const int16_t *w : _waves) n += w == nullptr;
        return isOpen() ? n : 0;
}

void FmSynth::close() {
        if (_state != nullptr) {
                _state->~State();
                Memory::free(_state);
        }
        _state = nullptr;
        for (int16_t *&w : _waves) {
                Memory::free(w);
                w = nullptr;
        }
        _bank = nullptr;
        Memory::free(_events);
        _events = nullptr;
        _head = _count = 0;
        return;
}

Error FmSynth::queue(const Event &event) {
        if (_state == nullptr) return Error::InvalidState;
        if (event.length == 0 || event.length > SynthEngine::MaxEventBytes) return Error::Invalid;
        if (_count == SynthEngine::QueueEvents) {
                ++_stats.queueFull;
                return Error::Full;
        }
        // In frame order: from the newest back to the first at or before its frame.
        size_t at = _count;
        while (at > 0 && _events[(_head + at - 1) % SynthEngine::QueueEvents].frame > event.frame) {
                _events[(_head + at) % SynthEngine::QueueEvents] = _events[(_head + at - 1) % SynthEngine::QueueEvents];
                --at;
        }
        _events[(_head + at) % SynthEngine::QueueEvents] = event;
        ++_count;
        return Error::Ok;
}

void FmSynth::apply(const Event &e, uint64_t frame) {
        State        &s = *_state;
        const uint8_t status = e.data[0];
        const uint8_t ch = status & 0x0f;
        const uint8_t d1 = e.length > 1 ? e.data[1] & 0x7f : 0;
        const uint8_t d2 = e.length > 2 ? e.data[2] & 0x7f : 0;
        if (e.length == sizeof(GmSystemOn) && std::memcmp(e.data, GmSystemOn, sizeof(GmSystemOn)) == 0) {
                resetAll(s);
                return;
        }
        switch (status & 0xf0) {
                case 0x80:
                        if (e.length < 3) break;
                        noteOff(s, ch, d1);
                        return;
                case 0x90:
                        if (e.length < 3) break;
                        if (d2 == 0) {
                                noteOff(s, ch, d1);
                        } else {
                                noteOn(s, *_bank, _stats, frame, ch, d1, d2);
                        }
                        return;
                case 0xb0:
                        if (e.length < 3) break;
                        control(s, ch, d1, d2);
                        return;
                case 0xc0:
                        if (e.length < 2) break;
                        s.channels[ch].program = d1;
                        return;
                case 0xe0:
                        if (e.length < 3) break;
                        s.channels[ch].bend = int16_t((d1 | d2 << 7) - 8192);
                        bendChanged(s, ch);
                        return;
                case 0xa0: // key pressure, channel pressure: taken, not used
                case 0xd0: return;
                default: break;
        }
        if (status >= 0xf8) return; // real-time
        ++_stats.ignored;
        return;
}

void FmSynth::render(float *out, size_t frames) {
        const uint8_t channels = _config.channels;
        if (_state == nullptr) {
                std::memset(out, 0, frames * channels * sizeof(float));
                return;
        }
        size_t done = 0;
        float  peak = 0.0f;
        while (done < frames) {
                // What is due by this frame, then up to the next event's frame, a block at a time.
                while (_count > 0 && _events[_head].frame <= _frames + done) {
                        apply(_events[_head], _frames + done);
                        _head = (_head + 1) % SynthEngine::QueueEvents;
                        --_count;
                }
                size_t until = frames;
                if (_count > 0 && _events[_head].frame - _frames < frames)
                        until = static_cast<size_t>(_events[_head].frame - _frames);
                while (done < until) {
                        const size_t n = until - done < BlockFrames ? until - done : BlockFrames;
                        peak = std::fmax(peak,
                                         renderBlock(*_state, out + done * channels, _frames + done, n, channels));
                        done += n;
                }
        }
        _stats.limited += _limiter.process(out, frames, channels, peak);
        _frames += frames;
        if (_state->sounding > _stats.peakVoices) _stats.peakVoices = _state->sounding;
        return;
}

void FmSynth::reset() {
        _head = _count = 0;
        if (_state != nullptr) resetAll(*_state);
        return;
}

FmSynth::Stats FmSynth::stats() const {
        Stats s = _stats;
        s.frames = _frames;
        s.voices = _state != nullptr ? _state->sounding : 0;
        s.maxVoices = _config.maxVoices;
        s.queued = static_cast<uint32_t>(_count);
        return s;
}

BMDS_NAMESPACE_END
